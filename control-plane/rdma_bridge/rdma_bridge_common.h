/*
 * rdma_bridge_common.h — Shared definitions for RDMA bridge slave/master.
 *
 * Wire protocol: fixed 16-byte header + variable payload.
 *   magic(4) = 0xBF21DMA1
 *   type(1)  = BRIDGE_MSG_*
 *   flags(1) = reserved
 *   len(2)   = payload length (network-byte order)
 *   seq(4)   = monotonic sequence (for dedup / debug)
 *   ts_ns(4) = low 32 bits of send timestamp (wrap-around ok for logging)
 *
 * Types:
 *   1 = ResourceReport (host metrics; ~128 B payload)
 *   2 = BF2MetricsReport (BF2 metrics; ~128 B payload)
 *   3 = StatusChange (domain status transition; ~64 B)
 */

#ifndef RDMA_BRIDGE_COMMON_H
#define RDMA_BRIDGE_COMMON_H

#include <stdint.h>

/* "BF2RDMA1" — all valid hex digits: BF2RDMA1 → 0xBF2FDA1A (ASCII-like) */
#define BRIDGE_MAGIC        0xBF2DDA1Au
#define BRIDGE_MAX_PAYLOAD  4080
#define BRIDGE_HDR_SIZE     16

#define BRIDGE_MSG_RESOURCE_REPORT  1
#define BRIDGE_MSG_BF2_REPORT       2
#define BRIDGE_MSG_STATUS_CHANGE    3

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  type;
    uint8_t  flags;
    uint16_t len;       /* network byte order */
    uint32_t seq;
    uint32_t ts_ns_lo;
} bridge_hdr_t;

/* Default UDS path for slave_agent → rdma_bridge_slave hand-off */
#define BRIDGE_UDS_DEFAULT_PATH  "/var/run/rdma_bridge.sock"

/* Default TCP port for BF2 RDMA cm endpoint */
#define BRIDGE_DEFAULT_PORT  7889

#endif /* RDMA_BRIDGE_COMMON_H */
