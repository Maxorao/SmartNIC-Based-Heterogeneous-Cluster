/*
 * rdma_transport.c — RDMA Send/Recv transport implementation.
 *
 * Uses rdma_cm for connection management (exchanges QP info over TCP),
 * then plain ibverbs for data transfer.
 *
 * Memory registration strategy:
 *   - Single MR covering an aligned pool of (recv_depth + 4) slots of msg_size.
 *   - Recv slots: first recv_depth slots are pre-posted as WR IDs 0..N-1.
 *   - Send slots: next 4 slots rotated round-robin (ID SEND_ID_BASE+i).
 *
 * Error handling:
 *   - All failures print to stderr with rdma_t: prefix.
 *   - Last error message is also kept on the endpoint for querying.
 */

#define _GNU_SOURCE
#include "rdma_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

/* ---------------------------------------------------------------------- */
/* Internal types                                                         */
/* ---------------------------------------------------------------------- */

#define SEND_SLOTS       4
#define MAX_INLINE_DATA  256
#define CM_EVENT_TIMEOUT_MS 15000

struct rdma_endpoint {
    /* Connection management */
    struct rdma_event_channel* ec;
    struct rdma_cm_id*         listen_id;   /* server only */
    struct rdma_cm_id*         id;          /* active connection id */

    /* Verbs resources */
    struct ibv_pd* pd;
    struct ibv_cq* send_cq;
    struct ibv_cq* recv_cq;
    struct ibv_qp* qp;

    /* Buffer pool */
    void*     buf;              /* (recv_depth + SEND_SLOTS) * msg_size */
    size_t    buf_size;
    struct ibv_mr* mr;

    /* Config */
    uint32_t  msg_size;
    uint32_t  recv_depth;
    bool      is_server;

    /* Send slot round-robin */
    uint32_t  send_next;
    uint32_t  send_outstanding;

    /* State */
    bool      connected;
    char      last_err[256];
};

/* WR ID encoding: low bits = slot index, high bit = is_send */
#define WR_ID_RECV(slot)  ((uint64_t)(slot))
#define WR_ID_SEND(slot)  ((uint64_t)(1ULL << 32) | (slot))
#define WR_ID_IS_SEND(id) (((id) >> 32) & 1)
#define WR_ID_SLOT(id)    ((uint32_t)((id) & 0xffffffff))

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static void set_err(rdma_endpoint_t* ep, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ep->last_err, sizeof(ep->last_err), fmt, ap);
    va_end(ap);
    fprintf(stderr, "rdma_t: %s\n", ep->last_err);
}

static void* slot_addr(rdma_endpoint_t* ep, uint32_t slot)
{
    return (char*)ep->buf + (size_t)slot * ep->msg_size;
}

static int post_one_recv(rdma_endpoint_t* ep, uint32_t slot)
{
    struct ibv_sge sge = {
        .addr   = (uintptr_t)slot_addr(ep, slot),
        .length = ep->msg_size,
        .lkey   = ep->mr->lkey,
    };
    struct ibv_recv_wr wr = {
        .wr_id      = WR_ID_RECV(slot),
        .sg_list    = &sge,
        .num_sge    = 1,
        .next       = NULL,
    };
    struct ibv_recv_wr* bad = NULL;
    int rc = ibv_post_recv(ep->qp, &wr, &bad);
    if (rc) {
        set_err(ep, "ibv_post_recv failed: %s", strerror(rc));
        return -rc;
    }
    return 0;
}

static int post_all_initial_recvs(rdma_endpoint_t* ep)
{
    for (uint32_t i = 0; i < ep->recv_depth; i++) {
        int rc = post_one_recv(ep, i);
        if (rc) return rc;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* QP and buffer setup                                                    */
/* ---------------------------------------------------------------------- */

static int setup_qp(rdma_endpoint_t* ep, struct rdma_cm_id* id)
{
    ep->pd = ibv_alloc_pd(id->verbs);
    if (!ep->pd) { set_err(ep, "ibv_alloc_pd failed"); return -ENOMEM; }

    uint32_t cqe = (ep->recv_depth + SEND_SLOTS) * 2 + 16;
    ep->recv_cq = ibv_create_cq(id->verbs, cqe, NULL, NULL, 0);
    if (!ep->recv_cq) { set_err(ep, "ibv_create_cq(recv) failed"); return -ENOMEM; }
    ep->send_cq = ibv_create_cq(id->verbs, cqe, NULL, NULL, 0);
    if (!ep->send_cq) { set_err(ep, "ibv_create_cq(send) failed"); return -ENOMEM; }

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = ep->send_cq,
        .recv_cq = ep->recv_cq,
        .qp_type = IBV_QPT_RC,
        .cap = {
            .max_send_wr  = SEND_SLOTS + 4,
            .max_recv_wr  = ep->recv_depth + 4,
            .max_send_sge = 1,
            .max_recv_sge = 1,
            .max_inline_data = MAX_INLINE_DATA,
        },
    };
    if (rdma_create_qp(id, ep->pd, &qp_attr)) {
        set_err(ep, "rdma_create_qp failed: %s", strerror(errno));
        return -errno;
    }
    ep->qp = id->qp;

    /* Allocate buffer pool: recv_depth + SEND_SLOTS message slots */
    uint32_t total_slots = ep->recv_depth + SEND_SLOTS;
    ep->buf_size = (size_t)total_slots * ep->msg_size;
    if (posix_memalign(&ep->buf, 4096, ep->buf_size) != 0) {
        set_err(ep, "posix_memalign failed"); return -ENOMEM;
    }
    memset(ep->buf, 0, ep->buf_size);

    ep->mr = ibv_reg_mr(ep->pd, ep->buf, ep->buf_size,
                        IBV_ACCESS_LOCAL_WRITE);
    if (!ep->mr) { set_err(ep, "ibv_reg_mr failed: %s", strerror(errno)); return -errno; }

    return post_all_initial_recvs(ep);
}

/* ---------------------------------------------------------------------- */
/* Server: listen/accept                                                  */
/* ---------------------------------------------------------------------- */

static int resolve_addr_from_str(const char* ip_str, uint16_t port,
                                  struct sockaddr_in* out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);
    if (!ip_str || !*ip_str) {
        out->sin_addr.s_addr = INADDR_ANY;
        return 0;
    }
    if (inet_pton(AF_INET, ip_str, &out->sin_addr) != 1) {
        return -1;
    }
    return 0;
}

static int wait_cm_event(rdma_endpoint_t* ep,
                         enum rdma_cm_event_type expect,
                         struct rdma_cm_event** out_event)
{
    struct pollfd pfd = { .fd = ep->ec->fd, .events = POLLIN };
    int rc = poll(&pfd, 1, CM_EVENT_TIMEOUT_MS);
    if (rc == 0) {
        set_err(ep, "cm event timeout waiting for %s",
                rdma_event_str(expect));
        return -ETIMEDOUT;
    }
    if (rc < 0) {
        set_err(ep, "poll(ec) failed: %s", strerror(errno));
        return -errno;
    }
    struct rdma_cm_event* ev = NULL;
    if (rdma_get_cm_event(ep->ec, &ev)) {
        set_err(ep, "rdma_get_cm_event failed: %s", strerror(errno));
        return -errno;
    }
    if (ev->event != expect) {
        set_err(ep, "unexpected cm event: got %s, expected %s",
                rdma_event_str(ev->event), rdma_event_str(expect));
        rdma_ack_cm_event(ev);
        return -EPROTO;
    }
    *out_event = ev;
    return 0;
}

rdma_endpoint_t* rdma_endpoint_create_server(const char* bind_ip,
                                              uint16_t port,
                                              uint32_t msg_size,
                                              uint32_t recv_depth)
{
    if (msg_size == 0 || recv_depth == 0) return NULL;

    rdma_endpoint_t* ep = calloc(1, sizeof(*ep));
    if (!ep) return NULL;
    ep->is_server = true;
    ep->msg_size = msg_size;
    ep->recv_depth = recv_depth;

    ep->ec = rdma_create_event_channel();
    if (!ep->ec) { set_err(ep, "rdma_create_event_channel failed"); goto err; }

    if (rdma_create_id(ep->ec, &ep->listen_id, NULL, RDMA_PS_TCP)) {
        set_err(ep, "rdma_create_id failed: %s", strerror(errno)); goto err;
    }

    struct sockaddr_in addr;
    if (resolve_addr_from_str(bind_ip, port, &addr) < 0) {
        set_err(ep, "invalid bind_ip: %s", bind_ip ? bind_ip : "(null)");
        goto err;
    }
    if (rdma_bind_addr(ep->listen_id, (struct sockaddr*)&addr)) {
        set_err(ep, "rdma_bind_addr failed: %s", strerror(errno)); goto err;
    }
    if (rdma_listen(ep->listen_id, 1)) {
        set_err(ep, "rdma_listen failed: %s", strerror(errno)); goto err;
    }

    /* Accept one connection */
    struct rdma_cm_event* ev = NULL;
    if (wait_cm_event(ep, RDMA_CM_EVENT_CONNECT_REQUEST, &ev) < 0) goto err;
    ep->id = ev->id;
    rdma_ack_cm_event(ev);

    if (setup_qp(ep, ep->id) < 0) goto err;

    struct rdma_conn_param cp = {
        .initiator_depth = 1,
        .responder_resources = 1,
        .rnr_retry_count = 7,
    };
    if (rdma_accept(ep->id, &cp)) {
        set_err(ep, "rdma_accept failed: %s", strerror(errno)); goto err;
    }
    if (wait_cm_event(ep, RDMA_CM_EVENT_ESTABLISHED, &ev) < 0) goto err;
    rdma_ack_cm_event(ev);

    ep->connected = true;
    return ep;

err:
    rdma_endpoint_destroy(ep);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Client: connect                                                        */
/* ---------------------------------------------------------------------- */

rdma_endpoint_t* rdma_endpoint_create_client(const char* server_ip,
                                              uint16_t port,
                                              uint32_t msg_size)
{
    if (!server_ip || msg_size == 0) return NULL;

    rdma_endpoint_t* ep = calloc(1, sizeof(*ep));
    if (!ep) return NULL;
    ep->is_server = false;
    ep->msg_size = msg_size;
    ep->recv_depth = 32;   /* default small recv pool for client replies */

    ep->ec = rdma_create_event_channel();
    if (!ep->ec) { set_err(ep, "rdma_create_event_channel failed"); goto err; }

    if (rdma_create_id(ep->ec, &ep->id, NULL, RDMA_PS_TCP)) {
        set_err(ep, "rdma_create_id failed: %s", strerror(errno)); goto err;
    }

    struct sockaddr_in addr;
    if (resolve_addr_from_str(server_ip, port, &addr) < 0) {
        set_err(ep, "invalid server_ip: %s", server_ip); goto err;
    }

    if (rdma_resolve_addr(ep->id, NULL, (struct sockaddr*)&addr, 5000)) {
        set_err(ep, "rdma_resolve_addr failed: %s", strerror(errno)); goto err;
    }
    struct rdma_cm_event* ev = NULL;
    if (wait_cm_event(ep, RDMA_CM_EVENT_ADDR_RESOLVED, &ev) < 0) goto err;
    rdma_ack_cm_event(ev);

    if (rdma_resolve_route(ep->id, 5000)) {
        set_err(ep, "rdma_resolve_route failed: %s", strerror(errno)); goto err;
    }
    if (wait_cm_event(ep, RDMA_CM_EVENT_ROUTE_RESOLVED, &ev) < 0) goto err;
    rdma_ack_cm_event(ev);

    if (setup_qp(ep, ep->id) < 0) goto err;

    struct rdma_conn_param cp = {
        .initiator_depth = 1,
        .responder_resources = 1,
        .retry_count = 7,
        .rnr_retry_count = 7,
    };
    if (rdma_connect(ep->id, &cp)) {
        set_err(ep, "rdma_connect failed: %s", strerror(errno)); goto err;
    }
    if (wait_cm_event(ep, RDMA_CM_EVENT_ESTABLISHED, &ev) < 0) goto err;
    rdma_ack_cm_event(ev);

    ep->connected = true;
    return ep;

err:
    rdma_endpoint_destroy(ep);
    return NULL;
}

/* ---------------------------------------------------------------------- */
/* Send                                                                   */
/* ---------------------------------------------------------------------- */

static int reap_send_completions(rdma_endpoint_t* ep, int max_wait_us)
{
    struct ibv_wc wc;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (ep->send_outstanding >= SEND_SLOTS) {
        int n = ibv_poll_cq(ep->send_cq, 1, &wc);
        if (n < 0) {
            set_err(ep, "ibv_poll_cq(send) error");
            return -EIO;
        }
        if (n == 1) {
            if (wc.status != IBV_WC_SUCCESS) {
                set_err(ep, "send WC error: %s (%d)",
                        ibv_wc_status_str(wc.status), wc.status);
                return -EIO;
            }
            ep->send_outstanding--;
            continue;
        }
        if (max_wait_us >= 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L +
                              (t1.tv_nsec - t0.tv_nsec) / 1000L;
            if (elapsed_us >= max_wait_us) return -ETIMEDOUT;
        }
    }
    return 0;
}

int rdma_endpoint_send(rdma_endpoint_t* ep, const void* data, uint32_t len)
{
    if (!ep || !ep->connected) return -ENOTCONN;
    if (len > ep->msg_size) return -EMSGSIZE;

    /* Ensure a send slot is available */
    if (ep->send_outstanding >= SEND_SLOTS) {
        int rc = reap_send_completions(ep, -1);
        if (rc) return rc;
    }

    uint32_t slot = ep->recv_depth + (ep->send_next % SEND_SLOTS);
    ep->send_next++;

    void* sbuf = slot_addr(ep, slot);
    memcpy(sbuf, data, len);

    bool use_inline = (len <= MAX_INLINE_DATA);

    struct ibv_sge sge = {
        .addr   = (uintptr_t)sbuf,
        .length = len,
        .lkey   = ep->mr->lkey,
    };
    struct ibv_send_wr wr = {
        .wr_id      = WR_ID_SEND(slot),
        .sg_list    = &sge,
        .num_sge    = 1,
        .opcode     = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED | (use_inline ? IBV_SEND_INLINE : 0),
        .next       = NULL,
    };
    struct ibv_send_wr* bad = NULL;
    int rc = ibv_post_send(ep->qp, &wr, &bad);
    if (rc) {
        set_err(ep, "ibv_post_send failed: %s", strerror(rc));
        return -rc;
    }
    ep->send_outstanding++;

    /* Reap up to 1 completion to keep pipeline shallow (latency-sensitive) */
    struct ibv_wc wc;
    int n = ibv_poll_cq(ep->send_cq, 1, &wc);
    if (n > 0) {
        if (wc.status != IBV_WC_SUCCESS) {
            set_err(ep, "send WC error: %s",
                    ibv_wc_status_str(wc.status));
            return -EIO;
        }
        ep->send_outstanding--;
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Recv                                                                   */
/* ---------------------------------------------------------------------- */

int rdma_endpoint_recv(rdma_endpoint_t* ep, void* buf, uint32_t buf_cap,
                       uint32_t* out_len, int64_t timeout_us)
{
    if (!ep || !ep->connected) return -ENOTCONN;
    if (!buf || !out_len) return -EINVAL;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    struct ibv_wc wc;
    for (;;) {
        int n = ibv_poll_cq(ep->recv_cq, 1, &wc);
        if (n < 0) {
            set_err(ep, "ibv_poll_cq(recv) error");
            return -EIO;
        }
        if (n == 1) break;

        if (timeout_us == RDMA_POLL_NONBLOCK) return -EAGAIN;
        if (timeout_us > 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000L +
                           (t1.tv_nsec - t0.tv_nsec) / 1000L;
            if (elapsed >= timeout_us) return -ETIMEDOUT;
        }
        /* else: busy-wait forever */
    }

    if (wc.status != IBV_WC_SUCCESS) {
        set_err(ep, "recv WC error: %s", ibv_wc_status_str(wc.status));
        return -EIO;
    }
    if (WR_ID_IS_SEND(wc.wr_id)) {
        /* Unexpected — ignore */
        set_err(ep, "recv CQ got send WC?");
        return -EPROTO;
    }

    uint32_t slot = WR_ID_SLOT(wc.wr_id);
    uint32_t byte_len = wc.byte_len;
    uint32_t copy = byte_len < buf_cap ? byte_len : buf_cap;
    memcpy(buf, slot_addr(ep, slot), copy);
    *out_len = copy;

    /* Re-post this recv */
    int rc = post_one_recv(ep, slot);
    if (rc) return rc;

    return 0;
}

/* ---------------------------------------------------------------------- */
/* Misc                                                                   */
/* ---------------------------------------------------------------------- */

bool rdma_endpoint_is_connected(const rdma_endpoint_t* ep)
{
    return ep && ep->connected;
}

const char* rdma_endpoint_last_error(const rdma_endpoint_t* ep)
{
    return ep ? ep->last_err : "(null endpoint)";
}

void rdma_endpoint_destroy(rdma_endpoint_t* ep)
{
    if (!ep) return;

    if (ep->id && ep->connected) {
        rdma_disconnect(ep->id);
    }
    if (ep->qp) {
        rdma_destroy_qp(ep->id);
        ep->qp = NULL;
    }
    if (ep->mr) { ibv_dereg_mr(ep->mr); ep->mr = NULL; }
    if (ep->recv_cq) { ibv_destroy_cq(ep->recv_cq); ep->recv_cq = NULL; }
    if (ep->send_cq) { ibv_destroy_cq(ep->send_cq); ep->send_cq = NULL; }
    if (ep->pd) { ibv_dealloc_pd(ep->pd); ep->pd = NULL; }
    if (ep->buf) { free(ep->buf); ep->buf = NULL; }
    if (ep->id) { rdma_destroy_id(ep->id); ep->id = NULL; }
    if (ep->listen_id) { rdma_destroy_id(ep->listen_id); ep->listen_id = NULL; }
    if (ep->ec) { rdma_destroy_event_channel(ep->ec); ep->ec = NULL; }

    free(ep);
}
