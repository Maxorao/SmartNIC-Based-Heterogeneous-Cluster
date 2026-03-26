/*
 * comch_host_doca31.c — Host-side Comch implementation for DOCA 3.1
 *
 * Wraps the DOCA 3.x async/callback model behind the synchronous
 * comch_api.h interface.  Selected when COMCH_HOST_DOCA_VER >= 30.
 *
 * Compile: part of comch_host.c (included via preprocessor, not linked
 *          separately), so it inherits all includes from comch_host.c.
 *
 * Environment: tianjin / fujian / helong x86  — DOCA 3.1.0, Ubuntu 22.04
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <doca_comch.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_pe.h>
#include <doca_error.h>
#include <doca_log.h>

#include "../comch_api.h"

DOCA_LOG_REGISTER(COMCH_HOST_31);

/* ── Receive ring buffer ────────────────────────────────────────────────────── */

#define RECV_RING_SIZE  64   /* must be power-of-2 */
#define SEND_TASK_POOL  8

struct recv_slot {
    uint8_t  buf[COMCH_MAX_MSG_SIZE];
    uint32_t len;
};

/* ── Context ──────────────────────────────────────────────────────────────────  */

struct comch_host_ctx {
    struct doca_dev              *dev;
    struct doca_pe               *pe;
    struct doca_comch_client     *client;
    struct doca_comch_connection *conn;       /* set once connected */

    /* Lock-free ring (single-producer = PE callback, single-consumer = caller) */
    struct recv_slot   ring[RECV_RING_SIZE];
    volatile uint32_t  ring_head;             /* written by recv callback */
    volatile uint32_t  ring_tail;             /* read/advanced by consumer */

    volatile doca_error_t send_result;
    volatile int          send_done;          /* 0 = pending, 1 = done */
    volatile int          connected;          /* 0 = connecting, 1 = up */
};

/* ── Helper: open device by PCI address string ──────────────────────────────── */

static doca_error_t open_dev_by_pci(const char *pci_str, struct doca_dev **dev_out)
{
    struct doca_devinfo **devs = NULL;
    uint32_t nb = 0;
    doca_error_t res;
    char addr[32];
    bool found = false;

    *dev_out = NULL;

    res = doca_devinfo_create_list(&devs, &nb);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_devinfo_create_list: %s", doca_error_get_name(res));
        return res;
    }

    for (uint32_t i = 0; i < nb; i++) {
        if (doca_devinfo_get_pci_addr_str(devs[i], addr) != DOCA_SUCCESS)
            continue;

        /* pci_addr_str returns "XX:XX.X" with leading zeros */
        bool match = false;
        if (strcmp(pci_str, "auto") == 0) {
            /* pick the first device that supports comch client */
            match = (doca_comch_cap_client_is_supported(devs[i]) == DOCA_SUCCESS);
        } else {
            match = (strcasecmp(addr, pci_str) == 0);
        }

        if (match) {
            res = doca_dev_open(devs[i], dev_out);
            if (res == DOCA_SUCCESS) {
                DOCA_LOG_INFO("Opened host device %s", addr);
                found = true;
            }
            break;
        }
    }

    doca_devinfo_destroy_list(devs);

    if (!found)
        return DOCA_ERROR_NOT_FOUND;
    return res;
}

/* ── Send callbacks ─────────────────────────────────────────────────────────── */

static void send_ok_cb(struct doca_comch_task_send *task,
                        union doca_data task_ud,
                        union doca_data ctx_ud)
{
    (void)task_ud;
    comch_host_ctx_t *ctx = (comch_host_ctx_t *)ctx_ud.ptr;
    ctx->send_result = DOCA_SUCCESS;
    ctx->send_done   = 1;
    doca_task_free(doca_comch_task_send_as_task(task));
}

static void send_err_cb(struct doca_comch_task_send *task,
                         union doca_data task_ud,
                         union doca_data ctx_ud)
{
    (void)task_ud;
    comch_host_ctx_t *ctx = (comch_host_ctx_t *)ctx_ud.ptr;
    ctx->send_result = doca_task_get_status(doca_comch_task_send_as_task(task));
    ctx->send_done   = 1;
    DOCA_LOG_WARN("Send failed: %s", doca_error_get_name(ctx->send_result));
    doca_task_free(doca_comch_task_send_as_task(task));
}

/* ── Recv callback ───────────────────────────────────────────────────────────── */

static void recv_cb(struct doca_comch_event_msg_recv *event,
                     uint8_t *recv_buf, uint32_t msg_len,
                     struct doca_comch_connection *connection)
{
    (void)event;
    union doca_data ud = doca_comch_connection_get_user_data(connection);
    comch_host_ctx_t *ctx = (comch_host_ctx_t *)ud.ptr;

    uint32_t next = (ctx->ring_head + 1) & (RECV_RING_SIZE - 1);
    if (next == ctx->ring_tail) {
        DOCA_LOG_WARN("Recv ring full, dropping message (%u bytes)", msg_len);
        return;
    }

    uint32_t copy_len = msg_len < COMCH_MAX_MSG_SIZE ? msg_len : COMCH_MAX_MSG_SIZE;
    memcpy(ctx->ring[ctx->ring_head].buf, recv_buf, copy_len);
    ctx->ring[ctx->ring_head].len = copy_len;
    /* publish: advance head after data is written */
    __atomic_store_n(&ctx->ring_head, next, __ATOMIC_RELEASE);
}

/* ── Connection-state callback (detects when client is connected) ───────────── */

static void conn_state_cb(struct doca_comch_event_connection_status_changed *event,
                           struct doca_comch_connection *connection,
                           uint8_t change_success)
{
    (void)event;
    if (!change_success)
        return;

    /* Retrieve ctx pointer from connection user-data */
    union doca_data ud = doca_comch_connection_get_user_data(connection);
    comch_host_ctx_t *ctx = (comch_host_ctx_t *)ud.ptr;
    if (!ctx)
        return;

    ctx->conn      = connection;
    ctx->connected = 1;
    DOCA_LOG_INFO("Comch client connected");
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

doca_error_t comch_host_init(comch_host_ctx_t **ctx_out,
                               const char        *pci_addr,
                               const char        *service_name)
{
    doca_error_t res;
    comch_host_ctx_t *ctx;

    if (!ctx_out || !pci_addr || !service_name)
        return DOCA_ERROR_INVALID_VALUE;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return DOCA_ERROR_NO_MEMORY;

    /* 1. Open the BF PF device on the host */
    res = open_dev_by_pci(pci_addr, &ctx->dev);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Cannot open device '%s': %s", pci_addr, doca_error_get_name(res));
        goto err_free;
    }

    /* 2. Create progress engine */
    res = doca_pe_create(&ctx->pe);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_pe_create: %s", doca_error_get_name(res));
        goto err_dev;
    }

    /* 3. Create Comch client (service_name is the connection key) */
    res = doca_comch_client_create(ctx->dev, service_name, &ctx->client);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_comch_client_create: %s", doca_error_get_name(res));
        goto err_pe;
    }

    /* 4. Configure message / queue sizes */
    doca_comch_client_set_max_msg_size(ctx->client, COMCH_MAX_MSG_SIZE);
    doca_comch_client_set_recv_queue_size(ctx->client, RECV_RING_SIZE);

    /* 5. Register send-task callbacks */
    union doca_data ctx_ud = { .ptr = ctx };
    res = doca_comch_client_task_send_set_conf(ctx->client,
                                               send_ok_cb, send_err_cb,
                                               SEND_TASK_POOL);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("task_send_set_conf: %s", doca_error_get_name(res));
        goto err_client;
    }

    /* 6. Register recv event callback */
    res = doca_comch_client_event_msg_recv_register(ctx->client, recv_cb);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("event_msg_recv_register: %s", doca_error_get_name(res));
        goto err_client;
    }

    /* 7. Attach context to progress engine */
    res = doca_pe_connect_ctx(ctx->pe, doca_comch_client_as_ctx(ctx->client));
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("doca_pe_connect_ctx: %s", doca_error_get_name(res));
        goto err_client;
    }

    /* 8. Set ctx user-data so callbacks can reach us */
    res = doca_ctx_set_user_data(doca_comch_client_as_ctx(ctx->client), ctx_ud);
    if (res != DOCA_SUCCESS) {
        /* Non-fatal: try without user-data on ctx (some versions don't support it) */
        DOCA_LOG_WARN("doca_ctx_set_user_data: %s (continuing)", doca_error_get_name(res));
    }

    /* 9. Start context — connection handshake begins asynchronously */
    res = doca_ctx_start(doca_comch_client_as_ctx(ctx->client));
    if (res != DOCA_SUCCESS && res != DOCA_ERROR_IN_PROGRESS) {
        DOCA_LOG_ERR("doca_ctx_start: %s", doca_error_get_name(res));
        goto err_client;
    }

    /* 10. Poll PE until connected or timeout (5 s) */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 5;

    while (!ctx->connected) {
        doca_pe_progress(ctx->pe);

        /* Also try get_connection as a fallback (some versions skip the callback) */
        if (!ctx->conn) {
            struct doca_comch_connection *c = NULL;
            if (doca_comch_client_get_connection(ctx->client, &c) == DOCA_SUCCESS && c) {
                union doca_data ud = { .ptr = ctx };
                doca_comch_connection_set_user_data(c, ud);
                ctx->conn      = c;
                ctx->connected = 1;
                DOCA_LOG_INFO("Comch client connected (polled)");
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            DOCA_LOG_ERR("Connection timeout (5 s) — is forward_routine running?");
            res = DOCA_ERROR_TIME_OUT;
            goto err_client;
        }
    }

    *ctx_out = ctx;
    DOCA_LOG_INFO("comch_host_init OK (DOCA 3.1, device=%s, service=%s)",
                  pci_addr, service_name);
    return DOCA_SUCCESS;

err_client:
    doca_comch_client_destroy(ctx->client);
err_pe:
    doca_pe_destroy(ctx->pe);
err_dev:
    doca_dev_close(ctx->dev);
err_free:
    free(ctx);
    return res;
}

doca_error_t comch_host_send(comch_host_ctx_t *ctx, const void *msg, size_t len)
{
    struct doca_comch_task_send *task;
    doca_error_t res;

    if (!ctx || !msg || len == 0 || len > COMCH_MAX_MSG_SIZE)
        return DOCA_ERROR_INVALID_VALUE;
    if (!ctx->connected)
        return DOCA_ERROR_NOT_CONNECTED;

    ctx->send_done   = 0;
    ctx->send_result = DOCA_SUCCESS;

    res = doca_comch_client_task_send_alloc_init(ctx->client, ctx->conn,
                                                  msg, (uint32_t)len, &task);
    if (res != DOCA_SUCCESS) {
        DOCA_LOG_ERR("task_send_alloc_init: %s", doca_error_get_name(res));
        return res;
    }

    res = doca_task_submit(doca_comch_task_send_as_task(task));
    if (res != DOCA_SUCCESS) {
        doca_task_free(doca_comch_task_send_as_task(task));
        return res;
    }

    /* Spin PE until completion callback fires */
    while (!ctx->send_done)
        doca_pe_progress(ctx->pe);

    return ctx->send_result;
}

doca_error_t comch_host_recv(comch_host_ctx_t *ctx, void *msg, size_t *len)
{
    if (!ctx || !msg || !len)
        return DOCA_ERROR_INVALID_VALUE;

    /* Drive the PE to process any incoming events */
    doca_pe_progress(ctx->pe);

    uint32_t tail = __atomic_load_n(&ctx->ring_tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&ctx->ring_head, __ATOMIC_ACQUIRE);

    if (tail == head)
        return DOCA_ERROR_AGAIN;

    struct recv_slot *slot = &ctx->ring[tail];
    uint32_t copy = slot->len < *len ? slot->len : (uint32_t)*len;
    memcpy(msg, slot->buf, copy);
    *len = copy;

    __atomic_store_n(&ctx->ring_tail,
                     (tail + 1) & (RECV_RING_SIZE - 1),
                     __ATOMIC_RELEASE);
    return DOCA_SUCCESS;
}

doca_error_t comch_host_recv_blocking(comch_host_ctx_t *ctx, void *msg,
                                        size_t *len, uint32_t timeout_ms)
{
    struct timespec deadline = {0};
    bool use_timeout = (timeout_ms > 0);

    if (use_timeout) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    for (;;) {
        doca_error_t res = comch_host_recv(ctx, msg, len);
        if (res != DOCA_ERROR_AGAIN)
            return res;

        if (use_timeout) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
                return DOCA_ERROR_TIME_OUT;
        }
    }
}

void comch_host_destroy(comch_host_ctx_t *ctx)
{
    if (!ctx) return;

    if (ctx->client) {
        doca_ctx_stop(doca_comch_client_as_ctx(ctx->client));
        /* Drain PE until context fully stops */
        for (int i = 0; i < 1000 && doca_pe_progress(ctx->pe); i++)
            ;
        doca_comch_client_destroy(ctx->client);
    }
    if (ctx->pe)  doca_pe_destroy(ctx->pe);
    if (ctx->dev) doca_dev_close(ctx->dev);
    free(ctx);
}
