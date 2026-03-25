/*
 * comch_host.c — DOCA Comch host-side (x86) endpoint
 *
 * Provides a kernel-bypass PCIe messaging channel between the host CPU
 * and the BlueField-3 SmartNIC ARM.  Uses DOCA Communications Channel
 * (Comch) from DOCA 2.7.0.
 *
 * Build: see tunnel/host/Makefile
 *
 * Usage (library):
 *   comch_host_ctx_t ctx;
 *   comch_host_init(&ctx, "03:00.0");
 *   comch_host_send(&ctx, buf, len);
 *   comch_host_recv(&ctx, buf, &len);
 *   comch_host_destroy(&ctx);
 */

#include "comch_host.h"

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

DOCA_LOG_REGISTER(COMCH_HOST);

/* ------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------ */

/*
 * Open the DOCA device matching the given PCI address.
 * Iterates the device list; compares BDF strings case-insensitively.
 */
static doca_error_t open_device_by_pci(const char *pci_addr,
                                        struct doca_dev **out_dev)
{
    struct doca_devinfo **dev_list = NULL;
    uint32_t nb_devs = 0;
    doca_error_t ret;

    ret = doca_devinfo_create_list(&dev_list, &nb_devs);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_devinfo_create_list failed: %s",
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
                DOCA_LOG_ERR("doca_dev_open(%s) failed: %s",
                             pci_addr, doca_error_get_descr(ret));
            }
            break;
        }
    }

    doca_devinfo_destroy_list(dev_list);

    if (ret == DOCA_ERROR_NOT_FOUND)
        DOCA_LOG_ERR("No DOCA device found for PCI %s", pci_addr);

    return ret;
}

/*
 * Open the NET representor of the BlueField-3 visible from the host.
 * The host sees the SmartNIC's net function as a representor port.
 */
static doca_error_t open_rep_by_pci(struct doca_dev *dev,
                                     const char *pci_addr,
                                     struct doca_dev_rep **out_rep)
{
    struct doca_devinfo_rep **rep_list = NULL;
    uint32_t nb_reps = 0;
    doca_error_t ret;

    ret = doca_devinfo_rep_create_list(dev, DOCA_DEVINFO_REP_FILTER_NET,
                                       &rep_list, &nb_reps);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_devinfo_rep_create_list failed: %s",
                     doca_error_get_descr(ret));
        return ret;
    }

    ret = DOCA_ERROR_NOT_FOUND;
    for (uint32_t i = 0; i < nb_reps; i++) {
        char bdf[DOCA_DEVINFO_REP_PCI_ADDR_SIZE] = {0};
        if (doca_devinfo_rep_get_pci_addr_str(rep_list[i], bdf) != DOCA_SUCCESS)
            continue;
        /* Match on bus:device.function prefix — representor may differ */
        if (strncasecmp(bdf, pci_addr, 5) == 0) {   /* match "BB:DD" */
            ret = doca_dev_rep_open(rep_list[i], out_rep);
            if (ret != DOCA_SUCCESS) {
                DOCA_LOG_ERR("doca_dev_rep_open failed: %s",
                             doca_error_get_descr(ret));
            }
            break;
        }
    }

    doca_devinfo_rep_destroy_list(rep_list);

    if (ret == DOCA_ERROR_NOT_FOUND)
        DOCA_LOG_ERR("No representor found for PCI prefix %s", pci_addr);

    return ret;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

doca_error_t comch_host_init(comch_host_ctx_t *ctx, const char *pci_addr)
{
    doca_error_t ret;

    memset(ctx, 0, sizeof(*ctx));

    /* 1. Open PF device */
    ret = open_device_by_pci(pci_addr, &ctx->dev);
    if (ret != DOCA_SUCCESS) return ret;

    /* 2. Open NET representor (required on host side for Comch) */
    ret = open_rep_by_pci(ctx->dev, pci_addr, &ctx->dev_rep);
    if (ret != DOCA_SUCCESS) {
        doca_dev_close(ctx->dev);
        ctx->dev = NULL;
        return ret;
    }

    /* 3. Create Comch endpoint */
    ret = doca_comm_channel_ep_create(&ctx->ep);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_comm_channel_ep_create: %s",
                     doca_error_get_descr(ret));
        goto err_rep;
    }

    /* 4. Attach device and representor */
    ret = doca_comm_channel_ep_set_device(ctx->ep, ctx->dev);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("ep_set_device: %s", doca_error_get_descr(ret));
        goto err_ep;
    }

    ret = doca_comm_channel_ep_set_device_rep(ctx->ep, ctx->dev_rep);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("ep_set_device_rep: %s", doca_error_get_descr(ret));
        goto err_ep;
    }

    /* 5. Configure queue sizes and max message size */
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

    /* 6. Connect to NIC-side listener (SERVICE_NAME from protocol.h) */
    ret = doca_comm_channel_ep_connect(ctx->ep, SERVICE_NAME, &ctx->peer_addr);
    if (ret != DOCA_SUCCESS) {
        DOCA_LOG_ERR("ep_connect(%s): %s", SERVICE_NAME,
                     doca_error_get_descr(ret));
        goto err_ep;
    }

    DOCA_LOG_INFO("comch_host: connected to NIC service '%s' on PCI %s",
                  SERVICE_NAME, pci_addr);
    return DOCA_SUCCESS;

err_ep:
    doca_comm_channel_ep_destroy(ctx->ep);
    ctx->ep = NULL;
err_rep:
    doca_dev_rep_close(ctx->dev_rep);
    ctx->dev_rep = NULL;
    doca_dev_close(ctx->dev);
    ctx->dev = NULL;
    return ret;
}

doca_error_t comch_host_send(comch_host_ctx_t *ctx,
                              const void *msg, size_t len)
{
    doca_error_t ret;
    uint32_t retries = 0;

    /* Retry on DOCA_ERROR_AGAIN (send queue full) with bounded spin */
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

/* Non-blocking receive — returns DOCA_ERROR_AGAIN if no message ready */
doca_error_t comch_host_recv(comch_host_ctx_t *ctx,
                              void *msg, size_t *len)
{
    struct doca_comm_channel_addr_t *src = NULL;
    doca_error_t ret;

    ret = doca_comm_channel_ep_recvfrom(ctx->ep, msg, len,
                                         DOCA_CC_MSG_FLAG_NONE, &src);
    return ret;  /* DOCA_SUCCESS, DOCA_ERROR_AGAIN, or other error */
}

/* Blocking receive with timeout.  Spins until message arrives or timeout. */
doca_error_t comch_host_recv_blocking(comch_host_ctx_t *ctx,
                                       void *msg, size_t *len,
                                       uint32_t timeout_ms)
{
    uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    doca_error_t ret;

    while (now_ns() < deadline) {
        ret = comch_host_recv(ctx, msg, len);
        if (ret == DOCA_SUCCESS)   return DOCA_SUCCESS;
        if (ret != DOCA_ERROR_AGAIN) return ret;
        /* Yield a little to avoid saturating PCIe with read requests */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000L }; /* 1 µs */
        nanosleep(&ts, NULL);
    }
    return DOCA_ERROR_TIME_OUT;
}

void comch_host_destroy(comch_host_ctx_t *ctx)
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

    if (ctx->dev_rep) {
        doca_dev_rep_close(ctx->dev_rep);
        ctx->dev_rep = NULL;
    }

    if (ctx->dev) {
        doca_dev_close(ctx->dev);
        ctx->dev = NULL;
    }

    DOCA_LOG_INFO("comch_host: destroyed");
}
