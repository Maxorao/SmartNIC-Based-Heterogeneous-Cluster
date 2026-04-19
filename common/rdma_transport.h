/*
 * rdma_transport.h — RDMA Send/Recv transport for BF2↔BF2 control-plane.
 *
 * Design:
 *   - Uses rdma_cm for connection setup (client/server over RoCE v2).
 *   - Uses ibv_post_send / ibv_post_recv with IBV_WR_SEND (two-sided).
 *   - Pre-posts a batch of receive WRs on the server side.
 *   - Single QP per connection, single CQ, busy-poll for latency-critical path.
 *   - Uses inline data when message fits inline (≤ 256 B typical), else DMA.
 *
 * Public API (C, dual-target: host x86_64 and BF2 aarch64):
 *   rdma_endpoint_create_server(ip, port, msg_size, batch) → endpoint
 *   rdma_endpoint_create_client(server_ip, port, msg_size) → endpoint
 *   rdma_endpoint_send(ep, data, len) → 0/−errno
 *   rdma_endpoint_recv(ep, buf, cap, out_len, timeout_us) → 0/−errno
 *   rdma_endpoint_destroy(ep)
 *
 * Link requirements: -libverbs -lrdmacm
 */

#ifndef RDMA_TRANSPORT_H
#define RDMA_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdma_endpoint rdma_endpoint_t;

/* Timeouts / poll limits */
#define RDMA_POLL_BUSY_WAIT  (-1)   /* Pass as timeout_us to busy-wait forever */
#define RDMA_POLL_NONBLOCK   (0)    /* Pass as timeout_us for non-blocking poll */

/*
 * Create a server endpoint that listens on ip:port and accepts a single
 * incoming connection. This call blocks until a client connects (or fails).
 *
 * Parameters:
 *   bind_ip     - local IP to bind (IPv4 string, e.g. "192.168.56.102")
 *                 If NULL, binds INADDR_ANY.
 *   port        - TCP port (rdma_cm uses TCP semantics for connection setup)
 *   msg_size    - maximum message size in bytes (≤ 4096 recommended)
 *   recv_depth  - number of pre-posted receive WRs (≥ 32 recommended)
 *
 * Returns: endpoint or NULL on error.
 */
rdma_endpoint_t* rdma_endpoint_create_server(const char* bind_ip,
                                              uint16_t port,
                                              uint32_t msg_size,
                                              uint32_t recv_depth);

/*
 * Create a client endpoint connecting to server at server_ip:port.
 *
 * Parameters:
 *   server_ip   - IPv4 string of server
 *   port        - server port
 *   msg_size    - maximum message size (must match server)
 *
 * Returns: endpoint or NULL on error.
 */
rdma_endpoint_t* rdma_endpoint_create_client(const char* server_ip,
                                              uint16_t port,
                                              uint32_t msg_size);

/*
 * Send a message over the endpoint. Blocking with busy-poll on the CQ.
 *
 * Parameters:
 *   ep          - endpoint
 *   data        - payload pointer
 *   len         - payload length (≤ msg_size)
 *
 * Returns: 0 on success, negative errno on failure.
 */
int rdma_endpoint_send(rdma_endpoint_t* ep, const void* data, uint32_t len);

/*
 * Receive one message. Copies up to buf_cap bytes into buf.
 *
 * Parameters:
 *   ep          - endpoint
 *   buf         - destination buffer
 *   buf_cap     - capacity of buf (should be ≥ msg_size)
 *   out_len     - actual message length written (out)
 *   timeout_us  - RDMA_POLL_BUSY_WAIT | RDMA_POLL_NONBLOCK | >0
 *
 * Returns: 0 on success, -ETIMEDOUT if non-blocking and no message,
 *          negative errno on failure.
 */
int rdma_endpoint_recv(rdma_endpoint_t* ep, void* buf, uint32_t buf_cap,
                       uint32_t* out_len, int64_t timeout_us);

/*
 * Destroy the endpoint. Drains outstanding WRs where possible.
 */
void rdma_endpoint_destroy(rdma_endpoint_t* ep);

/*
 * Check if the underlying QP is still connected (no peer disconnect seen).
 */
bool rdma_endpoint_is_connected(const rdma_endpoint_t* ep);

/*
 * Get human-readable description of the last error on this endpoint.
 */
const char* rdma_endpoint_last_error(const rdma_endpoint_t* ep);

#ifdef __cplusplus
}
#endif

#endif /* RDMA_TRANSPORT_H */
