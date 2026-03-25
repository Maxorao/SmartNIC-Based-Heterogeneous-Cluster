/*
 * comch_nic.h — Public interface for the NIC-side DOCA Comch endpoint.
 *
 * Include this header in programs that run on the BlueField-3 ARM and
 * need to receive/send messages from/to the host x86 via PCIe Comch.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <doca_comm_channel.h>
#include <doca_dev.h>
#include <doca_error.h>

/* Must match host-side values */
#define COMCH_MAX_MSG_SIZE   65536
#define COMCH_QUEUE_DEPTH    64
#define COMCH_SEND_RETRY_MAX 10000

/* Context handle for the NIC-side endpoint */
typedef struct {
    struct doca_dev               *dev;        /* NIC's own PF */
    struct doca_comm_channel_ep_t *ep;         /* Comch endpoint (listening) */
    struct doca_comm_channel_addr_t *peer_addr;/* set after first recv() */
} comch_nic_ctx_t;

/*
 * comch_nic_init — open NIC device, create and start listening endpoint.
 * @pci_addr: BDF of the NIC PF as seen from the ARM OS, e.g. "03:00.0"
 */
doca_error_t comch_nic_init(comch_nic_ctx_t *ctx, const char *pci_addr);

/* Send len bytes from msg.  peer_addr must be known (after first recv). */
doca_error_t comch_nic_send(comch_nic_ctx_t *ctx,
                             const void *msg, size_t len);

/* Non-blocking receive.  Returns DOCA_ERROR_AGAIN if no message. */
doca_error_t comch_nic_recv(comch_nic_ctx_t *ctx,
                             void *msg, size_t *len);

/* Blocking receive with millisecond timeout. */
doca_error_t comch_nic_recv_blocking(comch_nic_ctx_t *ctx,
                                      void *msg, size_t *len,
                                      uint32_t timeout_ms);

/* Disconnect and free all resources. */
void comch_nic_destroy(comch_nic_ctx_t *ctx);
