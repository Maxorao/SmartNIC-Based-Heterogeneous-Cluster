/*
 * comch_host.h — Public interface for the host-side DOCA Comch endpoint.
 *
 * Include this header in programs that run on the x86 host and need to
 * communicate with the BlueField-3 ARM via the PCIe Comch tunnel.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <doca_comm_channel.h>
#include <doca_dev.h>
#include <doca_error.h>

/* Tunable parameters */
#define COMCH_MAX_MSG_SIZE   65536   /* bytes — must match NIC side */
#define COMCH_QUEUE_DEPTH    64      /* entries in send/recv queues */
#define COMCH_SEND_RETRY_MAX 10000   /* spin retries before giving up */

/* Context handle — one per connection to the SmartNIC */
typedef struct {
    struct doca_dev               *dev;        /* BlueField-3 PF on host */
    struct doca_dev_rep           *dev_rep;    /* NET representor */
    struct doca_comm_channel_ep_t *ep;         /* Comch endpoint */
    struct doca_comm_channel_addr_t *peer_addr;/* NIC-side peer */
} comch_host_ctx_t;

/*
 * comch_host_init — open device, representor, create and connect endpoint.
 * @pci_addr: BDF of the BlueField-3 PF as seen from host, e.g. "03:00.0"
 */
doca_error_t comch_host_init(comch_host_ctx_t *ctx, const char *pci_addr);

/* Send len bytes from msg.  Retries up to COMCH_SEND_RETRY_MAX on AGAIN. */
doca_error_t comch_host_send(comch_host_ctx_t *ctx,
                              const void *msg, size_t len);

/* Non-blocking receive.  Returns DOCA_ERROR_AGAIN if nothing ready. */
doca_error_t comch_host_recv(comch_host_ctx_t *ctx,
                              void *msg, size_t *len);

/* Blocking receive with millisecond timeout. */
doca_error_t comch_host_recv_blocking(comch_host_ctx_t *ctx,
                                       void *msg, size_t *len,
                                       uint32_t timeout_ms);

/* Disconnect and free all resources. */
void comch_host_destroy(comch_host_ctx_t *ctx);
