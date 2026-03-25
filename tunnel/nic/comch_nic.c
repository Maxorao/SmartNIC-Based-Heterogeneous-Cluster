/*
 * comch_nic.c — DOCA Comch NIC-side (BlueField-3 ARM) endpoint
 *
 * Listens for connections from the host x86 via PCIe Comch.
 * Unlike the host side, the NIC side does NOT need a device representor —
 * it runs directly on the BlueField-3 ARM OS and opens the NIC's own device.
 *
 * Build: see tunnel/nic/Makefile  (compile on ARM or cross-compile)
 *
 * Usage (library):
 *   comch_nic_ctx_t ctx;
 *   comch_nic_init(&ctx, "03:00.0");   // BDF of the NIC's own PF
 *   // peer_addr is set after the first successful recv
 *   comch_nic_recv(&ctx, buf, &len);
 *   comch_nic_send(&ctx, buf, len);
 *   comch_nic_destroy(&ctx);
 */

#include "comch_nic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <doca_comm_channel.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>

#include "../../common/protocol.h"
#include "../../common/timing.h"

DOCA_LOG_REGISTER(COMCH_NIC);

/* ------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------ */

static doca_error_t open_device_by_pci(const char *pci_addr,
                                        struct doca_dev **out_dev)
{
    struct doca_devinfo **dev_list = NULL;
    uint32_t nb_devs = 0;
    doca_error_t ret;

    ret = doca_devinfo_create_list(&dev_list, &nb_devs);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_devinfo_create_list: %s",
                     doca_error_get_descr(ret));
        return ret;
    }

    ret = DOCA_ERROR_NOT_FOUND;
    for (uint32_t i = 0; i < nb_devs; i++) {
        char bdf[DOCA_DEVINFO_PCI_ADDR_SIZE] = {0};
        if (doca_devinfo_get_pci_addr_str(dev_list[i], bdf) != DOCA_SUCCESS)
            continue;
        if (strncasecmp(bdf, pci_addr, strlen(pci_addr)) == 0) {
            ret = doca_dev_open(dev_list[i], out_dev);
            if (ret != DOCA_SUCCESS) {
                DOCA_LOG_ERR("doca_dev_open(%s): %s",
                             pci_addr, doca_error_get_descr(ret));
            }
            break;
        }
    }

    doca_devinfo_destroy_list(dev_list);

    if (ret == DOCA_ERROR_NOT_FOUND)
        DOCA_LOG_ERR("No device found for PCI %s", pci_addr);

    return ret;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

doca_error_t comch_nic_init(comch_nic_ctx_t *ctx, const char *pci_addr)
{
    doca_error_t ret;

    memset(ctx, 0, sizeof(*ctx));

    /* 1. Open NIC's own PF — no representor needed on NIC side */
    ret = open_device_by_pci(pci_addr, &ctx->dev);
    if (ret != DOCA_SUCCESS) return ret;

    /* 2. Create Comch endpoint */
    ret = doca_comm_channel_ep_create(&ctx->ep);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("ep_create: %s", doca_error_get_descr(ret));
        doca_dev_close(ctx->dev);
        ctx->dev = NULL;
        return ret;
    }

    /* 3. Attach device */
    ret = doca_comm_channel_ep_set_device(ctx->ep, ctx->dev);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("ep_set_device: %s", doca_error_get_descr(ret));
        goto err_ep;
    }

    /* 4. Configure endpoint */
    ret = doca_comm_channel_ep_set_max_msg_size(ctx->ep, COMCH_MAX_MSG_SIZE);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("ep_set_max_msg_size: %s", doca_error_get_descr(ret));
        goto err_ep;
    }

    ret = doca_comm_channel_ep_set_send_queue_size(ctx->ep, COMCH_QUEUE_DEPTH);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("ep_set_send_queue_size: %s", doca_error_get_descr(ret));
        goto err_ep;
    }

    ret = doca_comm_channel_ep_set_recv_queue_size(ctx->ep, COMCH_QUEUE_DEPTH);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("ep_set_recv_queue_size: %s", doca_error_get_descr(ret));
        goto err_ep;
    }

    /* 5. Listen — NIC side is the server */
    ret = doca_comm_channel_ep_listen(ctx->ep, SERVICE_NAME);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("ep_listen(%s): %s", SERVICE_NAME,
                     doca_error_get_descr(ret));
        goto err_ep;
    }

    DOCA_LOG_INFO("comch_nic: listening on service '%s' (PCI %s)",
                  SERVICE_NAME, pci_addr);
    return DOCA_SUCCESS;

err_ep:
    doca_comm_channel_ep_destroy(ctx->ep);
    ctx->ep = NULL;
    doca_dev_close(ctx->dev);
    ctx->dev = NULL;
    return ret;
}

doca_error_t comch_nic_send(comch_nic_ctx_t *ctx,
                             const void *msg, size_t len)
{
    doca_error_t ret;
    uint32_t retries = 0;

    if (!ctx->peer_addr) {
        DOCA_LOG_ERR("comch_nic_send: no peer_addr — recv at least one msg first");
        return DOCA_ERROR_BAD_STATE;
    }

    do {
        ret = doca_comm_channel_ep_sendto(ctx->ep, msg, len,
                                          DOCA_CC_MSG_FLAG_NONE,
                                          ctx->peer_addr);
        if (ret == DOCA_SUCCESS) return DOCA_SUCCESS;
        if (ret != DOCA_ERROR_AGAIN) {
            DOCA_LOG_ERR("ep_sendto: %s", doca_error_get_descr(ret));
            return ret;
        }
        retries++;
    } while (retries < COMCH_SEND_RETRY_MAX);

    DOCA_LOG_WARN("ep_sendto: send queue full after %u retries", retries);
    return DOCA_ERROR_AGAIN;
}

/* Non-blocking receive.  Sets ctx->peer_addr on first call. */
doca_error_t comch_nic_recv(comch_nic_ctx_t *ctx,
                             void *msg, size_t *len)
{
    struct doca_comm_channel_addr_t *src = NULL;
    doca_error_t ret;

    ret = doca_comm_channel_ep_recvfrom(ctx->ep, msg, len,
                                         DOCA_CC_MSG_FLAG_NONE, &src);
    if (ret == DOCA_SUCCESS && src && !ctx->peer_addr) {
        /* Capture peer address from the first received message */
        ctx->peer_addr = src;
        DOCA_LOG_INFO("comch_nic: first message received — peer captured");
    }
    return ret;
}

/* Blocking receive with millisecond timeout */
doca_error_t comch_nic_recv_blocking(comch_nic_ctx_t *ctx,
                                      void *msg, size_t *len,
                                      uint32_t timeout_ms)
{
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    doca_error_t ret;

    while (now_ns() < deadline) {
        ret = comch_nic_recv(ctx, msg, len);
        if (ret == DOCA_SUCCESS)    return DOCA_SUCCESS;
        if (ret != DOCA_ERROR_AGAIN) return ret;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000L };
        nanosleep(&ts, NULL);
    }
    return DOCA_ERROR_TIME_OUT;
}

void comch_nic_destroy(comch_nic_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->ep && ctx->peer_addr) {
        doca_error_t ret = doca_comm_channel_ep_disconnect(ctx->ep,
                                                            ctx->peer_addr);
        if (ret != DOCA_SUCCESS)
            DOCA_LOG_WARN("ep_disconnect: %s", doca_error_get_descr(ret));
        ctx->peer_addr = NULL;
    }

    if (ctx->ep) {
        doca_comm_channel_ep_destroy(ctx->ep);
        ctx->ep = NULL;
    }

    if (ctx->dev) {
        doca_dev_close(ctx->dev);
        ctx->dev = NULL;
    }

    DOCA_LOG_INFO("comch_nic: destroyed");
}
