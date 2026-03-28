/*
 * metric_push.c — Lightweight host metric shipper for BF2 offload scenario
 *
 * Runs on the x86 host. Reads /proc/stat and /proc/meminfo every interval,
 * packs a MSG_RESOURCE_REPORT, and Comch-sends it to the local BF2.
 *
 * This is intentionally minimal:  no registration handshake, no heartbeat
 * state machine, no change detection, no TCP code.  All protocol logic
 * (heartbeat, registration, connection management) lives on the BF2 ARM
 * side in the offloaded slave_monitor / forward_routine.
 *
 * Compared to slave_monitor --mode=offload, this binary has:
 *   - No register/deregister lifecycle
 *   - No heartbeat vs report decision
 *   - No change-detection threshold
 *   - No TCP transport code path
 *   → Significantly shorter per-iteration CPU footprint
 *
 * Usage:
 *   metric_push --pci=0000:5e:00.0 --interval=10 [--node-id=NAME]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>

#include "../../common/protocol.h"
#include "../../common/timing.h"
#include "../../tunnel/comch_api.h"

static volatile int g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

/* ── /proc readers (same logic as slave_monitor, inlined for simplicity) ── */

typedef struct {
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
} cpu_stat_t;

static int read_cpu(cpu_stat_t *s)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    int rc = fscanf(f, "cpu  %lu %lu %lu %lu %lu %lu %lu %lu",
                    &s->user, &s->nice, &s->system, &s->idle,
                    &s->iowait, &s->irq, &s->softirq, &s->steal);
    fclose(f);
    return (rc == 8) ? 0 : -1;
}

static float cpu_pct(const cpu_stat_t *a, const cpu_stat_t *b)
{
    uint64_t idle_a = a->idle + a->iowait;
    uint64_t idle_b = b->idle + b->iowait;
    uint64_t tot_a  = a->user + a->nice + a->system + idle_a
                    + a->irq + a->softirq + a->steal;
    uint64_t tot_b  = b->user + b->nice + b->system + idle_b
                    + b->irq + b->softirq + b->steal;
    uint64_t dt = tot_b - tot_a, di = idle_b - idle_a;
    return dt ? (float)(dt - di) * 100.0f / (float)dt : 0.0f;
}

static void read_mem(uint64_t *total, uint64_t *avail)
{
    *total = *avail = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char key[64]; uint64_t val; char unit[8];
    while (fscanf(f, "%63s %lu %7s", key, &val, unit) >= 2) {
        if (strcmp(key, "MemTotal:") == 0)     *total = val;
        if (strcmp(key, "MemAvailable:") == 0) *avail = val;
    }
    fclose(f);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    char pci[32]     = "0000:5e:00.0";
    char node_id[64] = "";
    uint32_t interval_ms = 10;

    static struct option opts[] = {
        {"pci",      required_argument, 0, 'p'},
        {"interval", required_argument, 0, 't'},
        {"node-id",  required_argument, 0, 'n'},
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'p': strncpy(pci, optarg, sizeof(pci)-1); break;
        case 't': interval_ms = (uint32_t)atoi(optarg); break;
        case 'n': strncpy(node_id, optarg, sizeof(node_id)-1); break;
        default:
            fprintf(stderr, "Usage: %s --pci=ADDR [--interval=MS] [--node-id=NAME]\n", argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }
    if (node_id[0] == '\0')
        gethostname(node_id, sizeof(node_id) - 1);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Init Comch */
    comch_host_ctx_t *ctx = NULL;
    doca_error_t ret = comch_host_init(&ctx, pci, COMCH_SERVICE_NAME);
    if (ret != DOCA_SUCCESS) {
        fprintf(stderr, "metric_push: comch_host_init failed: %s\n",
                doca_error_get_name(ret));
        return 1;
    }
    fprintf(stderr, "metric_push: started (pci=%s interval=%ums node=%s)\n",
            pci, interval_ms, node_id);

    cpu_stat_t prev_cpu;
    read_cpu(&prev_cpu);
    uint32_t seq = 0;
    char buf[sizeof(msg_header_t) + sizeof(resource_report_t)];

    while (g_running) {
        sleep_ms(interval_ms);
        if (!g_running) break;

        /* Collect */
        cpu_stat_t cur_cpu;
        read_cpu(&cur_cpu);

        resource_report_t rpt = {0};
        strncpy(rpt.node_id, node_id, sizeof(rpt.node_id) - 1);
        rpt.timestamp_ns  = unix_ns();
        rpt.cpu_usage_pct = cpu_pct(&prev_cpu, &cur_cpu);
        read_mem(&rpt.mem_total_kb, &rpt.mem_avail_kb);
        prev_cpu = cur_cpu;

        /* Build and send */
        int len = proto_build(buf, sizeof(buf), MSG_RESOURCE_REPORT,
                              seq++, &rpt, sizeof(rpt));
        if (len > 0)
            comch_host_send(ctx, buf, (size_t)len);
    }

    comch_host_destroy(ctx);
    fprintf(stderr, "metric_push: exiting after %u reports\n", seq);
    return 0;
}
