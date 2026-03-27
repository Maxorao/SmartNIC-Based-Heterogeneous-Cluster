// Shared control-plane message protocol
// Binary format, no external dependencies
// Used by slave_monitor, master_monitor, forward_routine, and bench programs.

#pragma once
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#define PROTO_MAGIC       0xBEEF1234U
#define PROTO_MAX_PAYLOAD 4096
#define SERVICE_NAME      "cluster-control"   /* DOCA Comch service name */

/* Message types */
typedef enum {
    MSG_REGISTER        = 1,
    MSG_REGISTER_ACK    = 2,
    MSG_HEARTBEAT       = 3,
    MSG_HEARTBEAT_ACK   = 4,
    MSG_RESOURCE_REPORT = 5,
    MSG_COMMAND         = 6,
    MSG_COMMAND_ACK     = 7,
    MSG_DEREGISTER      = 8,
    /* Benchmark-only */
    MSG_BENCH_PING      = 100,
    MSG_BENCH_PONG      = 101,
} msg_type_t;

/* Fixed-size message header (16 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;       /* PROTO_MAGIC */
    uint16_t type;        /* msg_type_t */
    uint16_t flags;       /* reserved, set to 0 */
    uint32_t seq;         /* sequence number */
    uint32_t payload_len; /* length of following payload */
} msg_header_t;

/* Resource report payload */
typedef struct __attribute__((packed)) {
    char     node_id[64];       /* hostname */
    uint64_t timestamp_ns;      /* unix nanoseconds */
    float    cpu_usage_pct;     /* 0.0 - 100.0 */
    uint64_t mem_total_kb;
    uint64_t mem_avail_kb;
    uint64_t net_rx_bytes;
    uint64_t net_tx_bytes;
    uint64_t pcie_rx_bytes;
    uint64_t pcie_tx_bytes;
} resource_report_t;

/* Registration payload */
typedef struct __attribute__((packed)) {
    char     node_id[64];
    char     smartnic_id[64];   /* SmartNIC hardware identifier */
    uint32_t ip_addr;           /* network byte order */
    uint16_t port;
    uint16_t padding;
    char     version[16];
} register_payload_t;

/* Bench ping/pong payload */
typedef struct __attribute__((packed)) {
    uint64_t send_ts_ns;   /* sender timestamp (ns) */
    uint32_t seq;
    uint32_t size;         /* requested payload size */
    uint8_t  data[64];     /* padding to reach desired size */
} bench_ping_t;

/* Build a complete message into buf (header + payload).
 * Returns total bytes written, or -1 on error. */
static inline int proto_build(void *buf, size_t buf_size,
                               msg_type_t type, uint32_t seq,
                               const void *payload, uint32_t payload_len)
{
    size_t total = sizeof(msg_header_t) + payload_len;
    if (total > buf_size) return -1;

    msg_header_t *hdr = (msg_header_t *)buf;
    hdr->magic       = PROTO_MAGIC;
    hdr->type        = (uint16_t)type;
    hdr->flags       = 0;
    hdr->seq         = seq;
    hdr->payload_len = payload_len;

    if (payload_len > 0 && payload)
        memcpy((char *)buf + sizeof(msg_header_t), payload, payload_len);

    return (int)total;
}

/* Validate message header. Returns 0 on success, -1 on error. */
static inline int proto_validate(const msg_header_t *hdr, size_t recv_len)
{
    if (recv_len < sizeof(msg_header_t)) return -1;
    if (hdr->magic != PROTO_MAGIC)       return -1;
    if (recv_len < sizeof(msg_header_t) + hdr->payload_len) return -1;
    return 0;
}

/* Send a full message over a TCP socket (handles partial sends).
 * Returns 0 on success, -1 on error. */
static inline int proto_tcp_send(int fd, msg_type_t type, uint32_t seq,
                                  const void *payload, uint32_t payload_len)
{
    char buf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];
    int total = proto_build(buf, sizeof(buf), type, seq, payload, payload_len);
    if (total < 0) return -1;

    ssize_t sent = 0;
    while (sent < total) {
        ssize_t n = send(fd, buf + sent, (size_t)(total - sent), MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* Receive a full message from a TCP socket.
 * Reads header first, then payload. payload_buf must be at least PROTO_MAX_PAYLOAD.
 * Returns 0 on success, -1 on error/disconnect. */
static inline int proto_tcp_recv(int fd, msg_header_t *hdr_out,
                                  void *payload_buf, size_t payload_buf_size)
{
    /* Read header */
    ssize_t got = 0;
    char *p = (char *)hdr_out;
    while (got < (ssize_t)sizeof(msg_header_t)) {
        ssize_t n = recv(fd, p + got, sizeof(msg_header_t) - (size_t)got, MSG_WAITALL);
        if (n <= 0) return -1;
        got += n;
    }

    if (hdr_out->magic != PROTO_MAGIC) return -1;
    if (hdr_out->payload_len > payload_buf_size) return -1;

    /* Read payload */
    got = 0;
    p = (char *)payload_buf;
    while (got < (ssize_t)hdr_out->payload_len) {
        ssize_t n = recv(fd, p + got, hdr_out->payload_len - (size_t)got, MSG_WAITALL);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* Return human-readable message type name */
static inline const char *msg_type_str(uint16_t type)
{
    switch ((msg_type_t)type) {
        case MSG_REGISTER:        return "REGISTER";
        case MSG_REGISTER_ACK:    return "REGISTER_ACK";
        case MSG_HEARTBEAT:       return "HEARTBEAT";
        case MSG_HEARTBEAT_ACK:   return "HEARTBEAT_ACK";
        case MSG_RESOURCE_REPORT: return "RESOURCE_REPORT";
        case MSG_COMMAND:         return "COMMAND";
        case MSG_COMMAND_ACK:     return "COMMAND_ACK";
        case MSG_DEREGISTER:      return "DEREGISTER";
        case MSG_BENCH_PING:      return "BENCH_PING";
        case MSG_BENCH_PONG:      return "BENCH_PONG";
        default:                  return "UNKNOWN";
    }
}
