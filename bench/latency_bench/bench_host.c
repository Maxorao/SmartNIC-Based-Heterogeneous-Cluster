/*
 * bench_host.c — Experiment A: Tunnel latency measurement (host side)
 *
 * Measures one-way latency of two paths:
 *   comch  — DOCA Communications Channel (PCIe kernel-bypass)
 *   tcp    — kernel TCP socket to the NIC's IP port 12345
 *
 * Protocol: send MSG_BENCH_PING with a nanosecond timestamp embedded in
 * the payload; wait for MSG_BENCH_PONG echo; one-way latency = RTT / 2.
 *
 * Usage:
 *   bench_host --pci=ADDR [--mode=comch|tcp|all] [--size=BYTES|all]
 *              [--iters=N] [--nic-ip=IP] [--output-dir=DIR]
 *
 * Defaults: mode=all, size=all, iters=10000, nic-ip=192.168.1.10, output-dir=.
 *
 * Output: per-run CSV files + summary table printed to stdout.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../../common/protocol.h"
#include "../../common/timing.h"
#include "../../tunnel/host/comch_host.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

#define WARMUP_ITERS   200
#define TCP_NIC_PORT   12345

static char     g_pci_addr[32]    = "03:00.0";
static char     g_nic_ip[64]      = "192.168.1.10";
static uint32_t g_iters           = 10000;
static char     g_output_dir[256] = ".";

typedef enum { MODE_COMCH, MODE_TCP, MODE_ALL } run_mode_t;
static run_mode_t g_mode = MODE_ALL;

static int g_test_all_sizes = 1;
static uint32_t g_single_size = 64;

static const uint32_t SIZES[] = { 64, 256, 1024, 4096, 65536 };
#define N_SIZES  (sizeof(SIZES) / sizeof(SIZES[0]))

/* ------------------------------------------------------------------ */
/* Comch benchmark                                                      */
/* ------------------------------------------------------------------ */

static int run_comch(comch_host_ctx_t *ctx, uint32_t size,
                     uint32_t iters, uint64_t *latencies)
{
    /* Message buffer: header + bench_ping_t padded to `size` */
    size_t payload_size = sizeof(bench_ping_t);
    if (size > sizeof(bench_ping_t)) payload_size = size;
    if (payload_size > PROTO_MAX_PAYLOAD) payload_size = PROTO_MAX_PAYLOAD;

    char send_buf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];
    char recv_buf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];

    /* Warmup */
    for (uint32_t i = 0; i < WARMUP_ITERS; i++) {
        bench_ping_t ping = { .send_ts_ns = now_ns(), .seq = i, .size = size };
        size_t total = (size_t)proto_build(send_buf, sizeof(send_buf),
                                            MSG_BENCH_PING, i, &ping,
                                            (uint32_t)payload_size);
        doca_error_t ret = comch_host_send(ctx, send_buf, total);
        if (ret != DOCA_SUCCESS) return -1;
        size_t rlen = sizeof(recv_buf);
        ret = comch_host_recv_blocking(ctx, recv_buf, &rlen, 2000);
        if (ret != DOCA_SUCCESS) {
            fprintf(stderr, "warmup recv timeout (comch)\n");
            return -1;
        }
    }

    /* Measurement */
    for (uint32_t i = 0; i < iters; i++) {
        bench_ping_t ping = { .send_ts_ns = now_ns(), .seq = i, .size = size };
        size_t total = (size_t)proto_build(send_buf, sizeof(send_buf),
                                            MSG_BENCH_PING, i, &ping,
                                            (uint32_t)payload_size);
        uint64_t t0 = now_ns();
        doca_error_t ret = comch_host_send(ctx, send_buf, total);
        if (ret != DOCA_SUCCESS) { latencies[i] = 0; continue; }

        size_t rlen = sizeof(recv_buf);
        ret = comch_host_recv_blocking(ctx, recv_buf, &rlen, 2000);
        uint64_t rtt = now_ns() - t0;

        if (ret != DOCA_SUCCESS) { latencies[i] = 0; continue; }
        latencies[i] = rtt / 2;  /* one-way estimate */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* TCP benchmark                                                        */
/* ------------------------------------------------------------------ */

static int tcp_connect_nic(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(TCP_NIC_PORT),
    };
    if (inet_pton(AF_INET, g_nic_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid NIC IP: %s\n", g_nic_ip);
        close(fd); return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect to NIC TCP"); close(fd); return -1;
    }
    return fd;
}

static int run_tcp(int fd, uint32_t size, uint32_t iters, uint64_t *latencies)
{
    size_t payload_size = sizeof(bench_ping_t);
    if (size > sizeof(bench_ping_t)) payload_size = size;
    if (payload_size > PROTO_MAX_PAYLOAD) payload_size = PROTO_MAX_PAYLOAD;

    char send_buf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];

    /* Warmup */
    for (uint32_t i = 0; i < WARMUP_ITERS; i++) {
        bench_ping_t ping = { .send_ts_ns = now_ns(), .seq = i, .size = size };
        size_t total = (size_t)proto_build(send_buf, sizeof(send_buf),
                                            MSG_BENCH_PING, i, &ping,
                                            (uint32_t)payload_size);
        ssize_t n = send(fd, send_buf, total, MSG_NOSIGNAL);
        if (n != (ssize_t)total) return -1;

        /* Echo: receive back exactly `total` bytes */
        char rbuf[sizeof(send_buf)];
        ssize_t got = 0;
        while (got < (ssize_t)total) {
            ssize_t r = recv(fd, rbuf + got, (size_t)(total - got), MSG_WAITALL);
            if (r <= 0) return -1;
            got += r;
        }
    }

    /* Measurement */
    for (uint32_t i = 0; i < iters; i++) {
        bench_ping_t ping = { .send_ts_ns = now_ns(), .seq = i, .size = size };
        size_t total = (size_t)proto_build(send_buf, sizeof(send_buf),
                                            MSG_BENCH_PING, i, &ping,
                                            (uint32_t)payload_size);
        uint64_t t0 = now_ns();
        ssize_t n = send(fd, send_buf, total, MSG_NOSIGNAL);
        if (n != (ssize_t)total) { latencies[i] = 0; continue; }

        char rbuf[sizeof(send_buf)];
        ssize_t got = 0;
        while (got < (ssize_t)total) {
            ssize_t r = recv(fd, rbuf + got, (size_t)(total - got), MSG_WAITALL);
            if (r <= 0) { latencies[i] = 0; goto next; }
            got += r;
        }
        latencies[i] = (now_ns() - t0) / 2;
next:;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Run one combination: protocol × size                                */
/* ------------------------------------------------------------------ */

static void run_one(const char *proto, uint32_t size,
                    comch_host_ctx_t *comch_ctx, int tcp_fd)
{
    uint64_t *latencies = calloc(g_iters, sizeof(uint64_t));
    if (!latencies) { perror("calloc"); return; }

    printf("Running %s  size=%-6u  iters=%u ...", proto, size, g_iters);
    fflush(stdout);

    int rc = -1;
    if (strcmp(proto, "comch") == 0)
        rc = run_comch(comch_ctx, size, g_iters, latencies);
    else
        rc = run_tcp(tcp_fd, size, g_iters, latencies);

    if (rc < 0) {
        printf("  FAILED\n");
        free(latencies);
        return;
    }
    printf("  done\n");

    /* Stats */
    char label[64];
    snprintf(label, sizeof(label), "%s_%uB", proto, size);
    print_latency_stats(label, latencies, g_iters);

    /* Export CSV */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%uB.csv", g_output_dir, proto, size);
    if (export_csv(path, latencies, g_iters) == 0)
        printf("  -> CSV: %s\n", path);

    free(latencies);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"pci",        required_argument, 0, 'P'},
        {"mode",       required_argument, 0, 'm'},
        {"size",       required_argument, 0, 's'},
        {"iters",      required_argument, 0, 'n'},
        {"nic-ip",     required_argument, 0, 'i'},
        {"output-dir", required_argument, 0, 'o'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
        case 'P': strncpy(g_pci_addr, optarg, sizeof(g_pci_addr)-1); break;
        case 'm':
            if      (strcmp(optarg, "comch") == 0) g_mode = MODE_COMCH;
            else if (strcmp(optarg, "tcp")   == 0) g_mode = MODE_TCP;
            else if (strcmp(optarg, "all")   == 0) g_mode = MODE_ALL;
            else { fprintf(stderr, "Unknown mode: %s\n", optarg); exit(1); }
            break;
        case 's':
            if (strcmp(optarg, "all") == 0) {
                g_test_all_sizes = 1;
            } else {
                g_test_all_sizes = 0;
                g_single_size = (uint32_t)atoi(optarg);
            }
            break;
        case 'n': g_iters = (uint32_t)atoi(optarg); break;
        case 'i': strncpy(g_nic_ip, optarg, sizeof(g_nic_ip)-1); break;
        case 'o': strncpy(g_output_dir, optarg, sizeof(g_output_dir)-1); break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s --pci=ADDR [--mode=comch|tcp|all]\n"
                "          [--size=BYTES|all] [--iters=N]\n"
                "          [--nic-ip=IP] [--output-dir=DIR]\n",
                argv[0]);
            exit(c == 'h' ? 0 : 1);
        }
    }

    /* Decide which sizes to test */
    uint32_t sizes_to_test[N_SIZES];
    size_t   n_sizes;
    if (g_test_all_sizes) {
        memcpy(sizes_to_test, SIZES, sizeof(SIZES));
        n_sizes = N_SIZES;
    } else {
        sizes_to_test[0] = g_single_size;
        n_sizes = 1;
    }

    /* Initialise transports */
    comch_host_ctx_t comch_ctx;
    int comch_ok = 0;
    if (g_mode == MODE_COMCH || g_mode == MODE_ALL) {
        doca_error_t ret = comch_host_init(&comch_ctx, g_pci_addr);
        if (ret != DOCA_SUCCESS) {
            fprintf(stderr, "comch_host_init failed: %s\n",
                    doca_error_get_descr(ret));
            if (g_mode == MODE_COMCH) return 1;
            fprintf(stderr, "Skipping comch tests.\n");
        } else {
            comch_ok = 1;
        }
    }

    int tcp_fd = -1;
    if (g_mode == MODE_TCP || g_mode == MODE_ALL) {
        tcp_fd = tcp_connect_nic();
        if (tcp_fd < 0) {
            fprintf(stderr, "TCP connect to NIC failed\n");
            if (g_mode == MODE_TCP) { if (comch_ok) comch_host_destroy(&comch_ctx); return 1; }
            fprintf(stderr, "Skipping TCP tests.\n");
        }
    }

    printf("\n=== Tunnel Latency Benchmark ===\n");
    printf("PCI: %s   NIC-IP: %s   iters: %u\n\n",
           g_pci_addr, g_nic_ip, g_iters);

    /* Run benchmarks */
    for (size_t s = 0; s < n_sizes; s++) {
        uint32_t sz = sizes_to_test[s];
        if (comch_ok)
            run_one("comch", sz, &comch_ctx, -1);
        if (tcp_fd >= 0)
            run_one("tcp", sz, NULL, tcp_fd);
    }

    printf("\n=== Final Summary ===\n");

    /* Cleanup */
    if (comch_ok) comch_host_destroy(&comch_ctx);
    if (tcp_fd >= 0) close(tcp_fd);

    return 0;
}
