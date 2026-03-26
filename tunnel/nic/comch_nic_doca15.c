/*
 * comch_nic_doca15.c — NIC-side Comch implementation for DOCA 1.5 (BF2 ARM)
 *
 * Uses the legacy doca_comm_channel.h synchronous polling API.
 * Selected when COMCH_NIC_DOCA_VER < 30.
 *
 * Environment: BF2 (MBF2M516A), DOCA 1.5.4003, Ubuntu 20.04 ARM
 *
 * Key differences vs DOCA 3.x:
 *   - uint16_t (not uint32_t) for queue/msg size properties
 *   - Synchronous sendto/recvfrom (no Progress Engine)
 *   - Server side needs BOTH dev AND dev_rep (host-facing representor)
 *   - PCI addresses use doca_pci_bdf struct (bus:8 device:5 function:3)
 *   - doca_devinfo_list_create() / doca_devinfo_list_destroy() naming
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <doca_comm_channel.h>
#include <doca_dev.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_types.h>   /* doca_pci_bdf */

#include "../comch_api.h"

DOCA_LOG_REGISTER(COMCH_NIC_15);

/* ── Context ──────────────────────────────────────────────────────────────── */

struct comch_nic_ctx {
    struct doca_dev                  *dev;
    struct doca_dev_rep              *dev_rep;
    struct doca_comm_channel_ep_t    *ep;
    struct doca_comm_channel_addr_t  *peer_addr;   /* set after first recv */
};

/* ── PCI helpers ──────────────────────────────────────────────────────────── */

/*
 * Parse a PCI address string ("BB:DD.F") into a doca_pci_bdf.
 * Handles both "03:00.0" (common) and "0000:03:00.0" (with domain, ignored).
 */
static doca_error_t parse_pci(const char *s, struct doca_pci_bdf *bdf)
{
    const char *p = s;
    unsigned bus = 0, dev = 0, func = 0;

    /* Skip optional domain "XXXX:" */
    if (strlen(s) > 7 && s[4] == ':')
        p = s + 5;

    if (sscanf(p, "%x:%x.%x", &bus, &dev, &func) != 3) {
        DOCA_LOG_ERR("Cannot parse PCI address '%s'", s);
        return DOCA_ERROR_INVALID_VALUE;
    }

    bdf->bus      = (uint16_t)(bus  & 0xFF);
    bdf->device   = (uint16_t)(dev  & 0x1F);
    bdf->function = (uint16_t)(func & 0x07);
    return DOCA_SUCCESS;
}

/* Open main device by PCI BDF string */
static doca_error_t open_dev(const char *pci_str, struct doca_dev **dev_out)
{
    struct doca_devinfo **devs = NULL;
    uint32_t nb = 0;
    struct doca_pci_bdf target;
    doca_error_t res;
    bool found = false;

    *dev_out = NULL;

    res = parse_pci(pci_str, &target);
    if (res != DOCA_SUCCESS) return res;

    /* DOCA 1.5 uses doca_devinfo_list_create (same name as 3.x) */
    res = doca_devinfo_list_create(&devs, &nb);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_devinfo_list_create: %d", res);
        return res;
    }

    for (uint32_t i = 0; i < nb; i++) {
        struct doca_pci_bdf cur = {0};
        if (doca_devinfo_get_pci_addr(devs[i], &cur) != DOCA_SUCCESS)
            continue;
        if (cur.raw == target.raw) {
            res = doca_dev_open(devs[i], dev_out);
            if (res == DOCA_SUCCESS) {
                DOCA_LOG_INFO("Opened NIC device %s", pci_str);
                found = true;
            }
            break;
        }
    }

    doca_devinfo_list_destroy(devs);

    if (!found) {
        DOCA_LOG_ERR("No device found for PCI %s", pci_str);
        return DOCA_ERROR_NOT_FOUND;
    }
    return res;
}

/*
 * Open the host-facing representor (pf0hpf) by PCI address.
 *
 * If rep_pci is NULL or "auto", enumerates NET representors and picks
 * the first one (typically pf0hpf, which represents the host's PF0).
 *
 * When running on BF2, the representor's PCI address from the ARM side
 * is typically the same as the device PCI (03:00.0) but DOCA enumerates
 * it separately through doca_devinfo_rep.
 */
static doca_error_t open_rep(struct doca_dev *dev, const char *rep_pci,
                               struct doca_dev_rep **rep_out)
{
    struct doca_devinfo_rep **reps = NULL;
    uint32_t nb = 0;
    doca_error_t res;
    bool found = false;
    bool auto_select = (!rep_pci || strcmp(rep_pci, "auto") == 0);
    struct doca_pci_bdf target = {0};

    *rep_out = NULL;

    if (!auto_select) {
        res = parse_pci(rep_pci, &target);
        if (res != DOCA_SUCCESS) return res;
    }

    /* DOCA_DEV_REP_FILTER_NET enumerates host-facing representors */
    res = doca_devinfo_rep_create_list(dev, DOCA_DEV_REP_FILTER_NET,
                                        &reps, &nb);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_devinfo_rep_create_list: %d", res);
        return res;
    }

    DOCA_LOG_INFO("Found %u NET representor(s)", nb);

    for (uint32_t i = 0; i < nb; i++) {
        bool match = false;

        if (auto_select) {
            match = true;   /* pick first available */
        } else {
            struct doca_pci_bdf cur = {0};
            if (doca_devinfo_rep_get_pci_addr(reps[i], &cur) == DOCA_SUCCESS)
                match = (cur.raw == target.raw);
        }

        if (match) {
            res = doca_dev_rep_open(reps[i], rep_out);
            if (res == DOCA_SUCCESS) {
                DOCA_LOG_INFO("Opened representor %s",
                              auto_select ? "(auto)" : rep_pci);
                found = true;
            }
            break;
        }
    }

    doca_devinfo_rep_destroy_list(reps);

    if (!found) {
        DOCA_LOG_ERR("No suitable representor found%s%s",
                     auto_select ? "" : " for PCI ",
                     auto_select ? "" : rep_pci);
        return DOCA_ERROR_NOT_FOUND;
    }
    return res;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

doca_error_t comch_nic_init(comch_nic_ctx_t **ctx_out,
                              const char       *dev_pci,
                              const char       *rep_pci,
                              const char       *service_name)
{
    doca_error_t res;
    comch_nic_ctx_t *ctx;

    if (!ctx_out || !dev_pci || !service_name)
        return DOCA_ERROR_INVALID_VALUE;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return DOCA_ERROR_NO_MEMORY;

    /* 1. Open main device */
    res = open_dev(dev_pci, &ctx->dev);
    if (res != DOCA_SUCCESS) goto err_free;

    /* 2. Open host-facing representor (required for server side in DOCA 1.5) */
    res = open_rep(ctx->dev, rep_pci, &ctx->dev_rep);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to open representor — try passing rep_pci explicitly");
        goto err_dev;
    }

    /* 3. Create endpoint */
    res = doca_comm_channel_ep_create(&ctx->ep);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_comm_channel_ep_create: %d", res);
        goto err_rep;
    }

    /* 4. Attach device and representor */
    res = doca_comm_channel_ep_set_device(ctx->ep, ctx->dev);
    if (res != DOCA_SUCCESS) { DOCA_LOG_ERR("set_device: %d", res); goto err_ep; }

    res = doca_comm_channel_ep_set_device_rep(ctx->ep, ctx->dev_rep);
    if (res != DOCA_SUCCESS) { DOCA_LOG_ERR("set_device_rep: %d", res); goto err_ep; }

    /* 5. Properties (uint16_t in DOCA 1.5) */
    res = doca_comm_channel_ep_set_max_msg_size(ctx->ep,
                                                 (uint16_t)COMCH_MAX_MSG_SIZE);
    if (res != DOCA_SUCCESS) { DOCA_LOG_ERR("set_max_msg_size: %d", res); goto err_ep; }

    res = doca_comm_channel_ep_set_send_queue_size(ctx->ep, (uint16_t)16);
    if (res != DOCA_SUCCESS) { DOCA_LOG_ERR("set_send_queue_size: %d", res); goto err_ep; }

    res = doca_comm_channel_ep_set_recv_queue_size(ctx->ep, (uint16_t)16);
    if (res != DOCA_SUCCESS) { DOCA_LOG_ERR("set_recv_queue_size: %d", res); goto err_ep; }

    /* 6. Start listening — server role */
    res = doca_comm_channel_ep_listen(ctx->ep, service_name);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_comm_channel_ep_listen('%s'): %d", service_name, res);
        goto err_ep;
    }

    *ctx_out = ctx;
    DOCA_LOG_INFO("comch_nic_init OK (DOCA 1.5, device=%s, service=%s)",
                  dev_pci, service_name);
    return DOCA_SUCCESS;

err_ep:
    doca_comm_channel_ep_destroy(ctx->ep);
err_rep:
    doca_dev_rep_close(ctx->dev_rep);
err_dev:
    doca_dev_close(ctx->dev);
err_free:
    free(ctx);
    return res;
}

doca_error_t comch_nic_send(comch_nic_ctx_t *ctx, const void *msg, size_t len)
{
    if (!ctx || !msg || len == 0 || len > COMCH_MAX_MSG_SIZE)
        return DOCA_ERROR_INVALID_VALUE;
    if (!ctx->peer_addr) {
        DOCA_LOG_ERR("comch_nic_send: no peer — must recv first");
        return DOCA_ERROR_NOT_CONNECTED;
    }

    doca_error_t res;
    int retries = 0;

    do {
        res = doca_comm_channel_ep_sendto(ctx->ep, msg, len,
                                           DOCA_CC_MSG_FLAG_NONE,
                                           ctx->peer_addr);
        if (res == DOCA_SUCCESS) return DOCA_SUCCESS;
        if (res != DOCA_ERROR_AGAIN) {
            DOCA_LOG_ERR("ep_sendto: %d", res);
            return res;
        }
        /* Send queue full — brief spin */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000L };
        nanosleep(&ts, NULL);
    } while (++retries < 1000);

    return DOCA_ERROR_AGAIN;
}

doca_error_t comch_nic_recv(comch_nic_ctx_t *ctx, void *msg, size_t *len)
{
    if (!ctx || !msg || !len)
        return DOCA_ERROR_INVALID_VALUE;

    /*
     * recvfrom is non-blocking; DOCA_ERROR_AGAIN means no message yet.
     * On the first successful call it also implicitly accepts the client
     * connection and populates peer_addr.
     */
    return doca_comm_channel_ep_recvfrom(ctx->ep, msg, len,
                                          DOCA_CC_MSG_FLAG_NONE,
                                          &ctx->peer_addr);
}

doca_error_t comch_nic_recv_blocking(comch_nic_ctx_t *ctx, void *msg,
                                       size_t *len, uint32_t timeout_ms)
{
    struct timespec deadline = {0};
    bool use_timeout = (timeout_ms > 0);

    if (use_timeout) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    for (;;) {
        doca_error_t res = comch_nic_recv(ctx, msg, len);
        if (res != DOCA_ERROR_AGAIN)
            return res;

        if (use_timeout) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec))
                return DOCA_ERROR_TIME_OUT;
        }

        /* Yield ~1 µs to avoid busy-poll saturating the PCIe bus */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000L };
        nanosleep(&ts, NULL);
    }
}

void comch_nic_destroy(comch_nic_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->ep) {
        if (ctx->peer_addr)
            doca_comm_channel_ep_disconnect(ctx->ep, ctx->peer_addr);
        doca_comm_channel_ep_destroy(ctx->ep);
    }
    if (ctx->dev_rep) doca_dev_rep_close(ctx->dev_rep);
    if (ctx->dev)     doca_dev_close(ctx->dev);
    free(ctx);
    DOCA_LOG_INFO("comch_nic: destroyed");
}
