/*
 * e2e_host.c — End-to-end latency bench, host endpoint.
 *
 * Topology:
 *   host-A (this bin) ↔ Comch ↔ BF2-A (e2e_nic, --mode=client) ↔ RDMA ↔
 *                                 BF2-B (e2e_nic, --mode=server) ↔ Comch ↔ host-B (this bin)
 *
 * There are two host binaries running simultaneously:
 *   host-A (pinger):  sends MSG_BENCH_PING with send_ts, waits for MSG_BENCH_PONG,
 *                     records RTT, reports statistics.
 *   host-B (ponger):  receives PING, echoes PONG with same payload.
 *
 * Usage:
 *   host-A (tianjin):
 *     ./e2e_host --mode=pinger --pci=<host_pci> --size=128 --iters=10000 \
 *                --output=/tmp/e2e.csv
 *   host-B (fujian):
 *     ./e2e_host --mode=ponger --pci=<host_pci>
 *
 * Each ping traverses FOUR hops: Comch(A→BF2-A) + RDMA(BF2-A→BF2-B) + Comch(BF2-B→B),
 * and the pong traverses the same four hops in reverse → RTT is 8 hops. Divide by 2
 * for one-way latency.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <time.h>

#include "../../common/protocol.h"
#include "../../common/timing.h"
#include "../../tunnel/comch_api.h"

#define DEFAULT_SERVICE  "e2e-latency"
#define WARMUP_ITERS     200

static volatile int g_running = 1;
static void sig_handler(int sig) { (void)sig; g_running = 0; }

typedef enum { MODE_PINGER, MODE_PONGER } e2e_mode_t;

static void print_stats(uint64_t* latencies_ns, uint32_t n, uint32_t msg_size,
                         const char* out_path)
{
    if (n == 0) { fprintf(stderr, "no samples\n"); return; }
    /* Sort to get percentiles */
    for (uint32_t i = 1; i < n; i++) {
        uint64_t k = latencies_ns[i];
        int j = (int)i - 1;
        while (j >= 0 && latencies_ns[j] > k) {
            latencies_ns[j+1] = latencies_ns[j]; j--;
        }
        latencies_ns[j+1] = k;
    }
    uint64_t min_ns = latencies_ns[0];
    uint64_t p50 = latencies_ns[n/2];
    uint64_t p95 = latencies_ns[(uint32_t)((double)n * 0.95)];
    uint64_t p99 = latencies_ns[(uint32_t)((double)n * 0.99)];
    uint64_t p999 = latencies_ns[(uint32_t)((double)n * 0.999)];
    uint64_t max_ns = latencies_ns[n-1];
    double sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += latencies_ns[i];
    double mean = sum / n;

    printf("\n=== E2E Latency (RTT, Comch→RDMA→Comch→...→Comch) ===\n");
    printf("  msg_size = %u bytes, n = %u\n", msg_size, n);
    printf("  RTT  min   = %.2f us\n", min_ns / 1000.0);
    printf("  RTT  mean  = %.2f us\n", mean / 1000.0);
    printf("  RTT  p50   = %.2f us\n", p50 / 1000.0);
    printf("  RTT  p95   = %.2f us\n", p95 / 1000.0);
    printf("  RTT  p99   = %.2f us\n", p99 / 1000.0);
    printf("  RTT  p99.9 = %.2f us\n", p999 / 1000.0);
    printf("  RTT  max   = %.2f us\n", max_ns / 1000.0);
    printf("  One-way (RTT/2): mean=%.2f us, p50=%.2f us, p99=%.2f us\n",
           mean / 2000.0, p50 / 2000.0, p99 / 2000.0);

    if (out_path) {
        FILE* f = fopen(out_path, "w");
        if (f) {
            fprintf(f, "msg_size,iteration,rtt_ns\n");
            for (uint32_t i = 0; i < n; i++) {
                fprintf(f, "%u,%u,%" PRIu64 "\n", msg_size, i, latencies_ns[i]);
            }
            fclose(f);
            fprintf(stderr, "Wrote %u samples to %s\n", n, out_path);
        }
    }
}

static int run_pinger(const char* pci_addr, uint32_t msg_size, uint32_t iters,
                       const char* out_path, const char* service)
{
    comch_host_ctx_t* ctx = NULL;
    if (comch_host_init(&ctx, pci_addr, service) != DOCA_SUCCESS) {
        fprintf(stderr, "comch_host_init failed\n");
        return 1;
    }
    fprintf(stderr, "[pinger] comch connected (pci=%s, service=%s)\n",
            pci_addr, service);

    size_t payload_size = sizeof(bench_ping_t);
    if (msg_size > sizeof(bench_ping_t)) payload_size = msg_size;
    if (payload_size > PROTO_MAX_PAYLOAD) payload_size = PROTO_MAX_PAYLOAD;

    char sbuf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];
    char rbuf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];

    /* Warmup */
    for (uint32_t i = 0; i < WARMUP_ITERS && g_running; i++) {
        bench_ping_t ping = { .send_ts_ns = now_ns(), .seq = i, .size = msg_size };
        int total = proto_build(sbuf, sizeof(sbuf), MSG_BENCH_PING, i,
                                &ping, (uint32_t)payload_size);
        if (comch_host_send(ctx, sbuf, total) != DOCA_SUCCESS) {
            fprintf(stderr, "[warmup] send failed\n"); goto cleanup;
        }
        size_t rlen = sizeof(rbuf);
        if (comch_host_recv_blocking(ctx, rbuf, &rlen, 5000) != DOCA_SUCCESS) {
            fprintf(stderr, "[warmup] recv failed\n"); goto cleanup;
        }
    }
    fprintf(stderr, "[pinger] warmup (%u iters) done, starting %u measurements\n",
            WARMUP_ITERS, iters);

    uint64_t* lat = calloc(iters, sizeof(uint64_t));
    if (!lat) { fprintf(stderr, "calloc failed\n"); goto cleanup; }

    uint32_t ok = 0;
    for (uint32_t i = 0; i < iters && g_running; i++) {
        bench_ping_t ping = { .send_ts_ns = now_ns(), .seq = i, .size = msg_size };
        int total = proto_build(sbuf, sizeof(sbuf), MSG_BENCH_PING, i,
                                &ping, (uint32_t)payload_size);
        uint64_t t0 = now_ns();
        if (comch_host_send(ctx, sbuf, total) != DOCA_SUCCESS) continue;

        size_t rlen = sizeof(rbuf);
        if (comch_host_recv_blocking(ctx, rbuf, &rlen, 2000) != DOCA_SUCCESS) {
            fprintf(stderr, "[pinger] recv timeout at i=%u\n", i);
            continue;
        }
        uint64_t t1 = now_ns();
        lat[ok++] = t1 - t0;

        /* Progress */
        if ((i & 0xFFF) == 0) {
            fprintf(stderr, "[pinger] %u / %u\r", i, iters);
        }
    }
    fprintf(stderr, "\n");

    print_stats(lat, ok, msg_size, out_path);
    free(lat);

cleanup:
    comch_host_destroy(ctx);
    return 0;
}

static int run_ponger(const char* pci_addr, const char* service)
{
    comch_host_ctx_t* ctx = NULL;
    if (comch_host_init(&ctx, pci_addr, service) != DOCA_SUCCESS) {
        fprintf(stderr, "comch_host_init failed\n");
        return 1;
    }
    fprintf(stderr, "[ponger] comch connected (pci=%s, service=%s)\n",
            pci_addr, service);

    char buf[sizeof(msg_header_t) + PROTO_MAX_PAYLOAD];
    uint64_t count = 0;
    while (g_running) {
        size_t len = sizeof(buf);
        doca_error_t rc = comch_host_recv_blocking(ctx, buf, &len, 1000);
        if (rc != DOCA_SUCCESS) continue;

        msg_header_t* hdr = (msg_header_t*)buf;
        if (proto_validate(hdr, len) < 0) continue;
        if (hdr->type != MSG_BENCH_PING) continue;

        /* Echo as PONG: keep same payload, only change type/seq */
        hdr->type = MSG_BENCH_PONG;
        (void)comch_host_send(ctx, buf, len);
        count++;

        if ((count & 0xFFF) == 0) {
            fprintf(stderr, "[ponger] echoed %" PRIu64 "\r", count);
        }
    }
    fprintf(stderr, "\n[ponger] exiting (%" PRIu64 " echoes)\n", count);
    comch_host_destroy(ctx);
    return 0;
}

int main(int argc, char** argv)
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    mode_t mode = MODE_PINGER;
    const char* pci_addr = "auto";
    const char* service = DEFAULT_SERVICE;
    const char* out_path = NULL;
    uint32_t msg_size = 128;
    uint32_t iters = 10000;

    static struct option longopts[] = {
        {"mode",    required_argument, 0, 'm'},
        {"pci",     required_argument, 0, 'p'},
        {"service", required_argument, 0, 's'},
        {"size",    required_argument, 0, 'z'},
        {"iters",   required_argument, 0, 'i'},
        {"output",  required_argument, 0, 'o'},
        {"help",    no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "m:p:s:z:i:o:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'm':
            if (strcmp(optarg, "pinger") == 0) mode = MODE_PINGER;
            else if (strcmp(optarg, "ponger") == 0) mode = MODE_PONGER;
            else { fprintf(stderr, "unknown mode\n"); return 1; }
            break;
        case 'p': pci_addr = optarg; break;
        case 's': service = optarg; break;
        case 'z': msg_size = atoi(optarg); break;
        case 'i': iters = atoi(optarg); break;
        case 'o': out_path = optarg; break;
        case 'h':
            fprintf(stderr,
                "Usage: %s --mode=pinger|ponger --pci=ADDR [--size=N]\n"
                "          [--iters=N] [--output=FILE] [--service=NAME]\n",
                argv[0]);
            return 0;
        }
    }

    return mode == MODE_PINGER ?
        run_pinger(pci_addr, msg_size, iters, out_path, service) :
        run_ponger(pci_addr, service);
}
