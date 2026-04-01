/*
 * metric_push.cc — Enhanced host metric shipper with gRPC fallback.
 *
 * Normal mode:  Same as metric_push.c — reads /proc/stat + /proc/meminfo,
 *               builds a MSG_RESOURCE_REPORT, sends via Comch to local BF2.
 *
 * Fallback mode: After 5 consecutive Comch failures, switches to gRPC
 *                DirectPush to cluster_master.  Periodically (30s) tries
 *                to re-init Comch and switch back when recovered.
 *
 * Usage:
 *   metric_push --pci=0000:5e:00.0 --interval=1000 \
 *       --node-id=fujian --master-addr=localhost:50051
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <atomic>
#include <chrono>
#include <memory>
#include <getopt.h>
#include <unistd.h>

#include <grpcpp/grpcpp.h>
#include "cluster.grpc.pb.h"
#include "cluster.pb.h"

extern "C" {
#include "../../common/protocol.h"
#include "../../common/timing.h"
#ifndef NO_DOCA
#include "../../tunnel/comch_api.h"
#endif
}

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */

static std::atomic<bool> g_running{true};

static void on_signal(int) { g_running.store(false); }

/* ------------------------------------------------------------------ */
/* Transport mode                                                       */
/* ------------------------------------------------------------------ */

enum class Mode { COMCH, GRPC_DIRECT };

/* ------------------------------------------------------------------ */
/* /proc readers (same logic as metric_push.c)                          */
/* ------------------------------------------------------------------ */

struct CpuStat {
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
};

static int read_cpu(CpuStat *s)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    int rc = fscanf(f, "cpu  %lu %lu %lu %lu %lu %lu %lu %lu",
                    &s->user, &s->nice, &s->system, &s->idle,
                    &s->iowait, &s->irq, &s->softirq, &s->steal);
    fclose(f);
    return (rc == 8) ? 0 : -1;
}

static float cpu_pct(const CpuStat *a, const CpuStat *b)
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

/* ------------------------------------------------------------------ */
/* gRPC direct push                                                     */
/* ------------------------------------------------------------------ */

static bool grpc_push(const std::shared_ptr<grpc::Channel> &channel,
                      const char *node_id, float cpu,
                      uint64_t mem_total, uint64_t mem_avail,
                      uint64_t net_rx, uint64_t net_tx)
{
    auto stub = cluster::ClusterControl::NewStub(channel);

    cluster::DirectPushRequest req;
    req.set_node_id(node_id);
    req.set_timestamp_ns(unix_ns());
    req.set_cpu_usage_pct(cpu);
    req.set_mem_total_kb(mem_total);
    req.set_mem_avail_kb(mem_avail);
    req.set_net_rx_bytes(net_rx);
    req.set_net_tx_bytes(net_tx);

    cluster::DirectPushResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(5));

    grpc::Status status = stub->DirectPush(&ctx, req, &resp);
    return status.ok() && resp.accepted();
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    char pci[32]        = "0000:5e:00.0";
    char node_id[64]    = "";
    uint32_t interval_ms = 1000;
    std::string master_addr = "localhost:50051";

    static struct option opts[] = {
        {"pci",         required_argument, nullptr, 'p'},
        {"interval",    required_argument, nullptr, 't'},
        {"node-id",     required_argument, nullptr, 'n'},
        {"master-addr", required_argument, nullptr, 'm'},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 'p': strncpy(pci, optarg, sizeof(pci) - 1); break;
        case 't': interval_ms = (uint32_t)atoi(optarg); break;
        case 'n': strncpy(node_id, optarg, sizeof(node_id) - 1); break;
        case 'm': master_addr = optarg; break;
        default:
            fprintf(stderr,
                "Usage: %s --pci=ADDR [--interval=MS] [--node-id=NAME] "
                "[--master-addr=ADDR]\n", argv[0]);
            return (c == 'h') ? 0 : 1;
        }
    }

    if (node_id[0] == '\0')
        gethostname(node_id, sizeof(node_id) - 1);

    struct sigaction sa_sig{};
    sa_sig.sa_handler = on_signal;
    sigemptyset(&sa_sig.sa_mask);
    sa_sig.sa_flags = 0;
    sigaction(SIGTERM, &sa_sig, nullptr);
    sigaction(SIGINT,  &sa_sig, nullptr);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize Comch */
    comch_host_ctx_t *comch_ctx = nullptr;
    doca_error_t ret = comch_host_init(&comch_ctx, pci, COMCH_SERVICE_NAME);
    if (ret != DOCA_SUCCESS) {
        fprintf(stderr, "[metric_push] comch_host_init failed: will start in "
                "gRPC mode\n");
        comch_ctx = nullptr;
    }

    Mode current_mode = (comch_ctx != nullptr) ? Mode::COMCH : Mode::GRPC_DIRECT;
    int comch_failures = 0;
    const int FAILOVER_THRESHOLD = 5;
    const uint64_t COMCH_RETRY_INTERVAL_NS = 30ULL * 1000000000ULL;  /* 30s */
    uint64_t last_comch_retry_ns = 0;

    /* gRPC channel (lazy, created on first use in GRPC_DIRECT mode) */
    std::shared_ptr<grpc::Channel> grpc_channel;

    fprintf(stderr, "[metric_push] started (pci=%s interval=%ums node=%s "
            "mode=%s master=%s)\n",
            pci, interval_ms, node_id,
            (current_mode == Mode::COMCH) ? "comch" : "grpc",
            master_addr.c_str());

    CpuStat prev_cpu = {};
    read_cpu(&prev_cpu);
    uint32_t seq = 0;
    uint64_t total_comch_sent = 0;
    uint64_t total_grpc_sent  = 0;

    while (g_running.load()) {
        sleep_ms(interval_ms);
        if (!g_running.load()) break;

        /* Collect metrics */
        CpuStat cur_cpu = {};
        read_cpu(&cur_cpu);

        float cpu = cpu_pct(&prev_cpu, &cur_cpu);
        uint64_t mem_total = 0, mem_avail = 0;
        read_mem(&mem_total, &mem_avail);
        prev_cpu = cur_cpu;

        if (current_mode == Mode::COMCH) {
            /* Build binary message and send via Comch */
            resource_report_t rpt = {};
            strncpy(rpt.node_id, node_id, sizeof(rpt.node_id) - 1);
            rpt.timestamp_ns  = unix_ns();
            rpt.cpu_usage_pct = cpu;
            rpt.mem_total_kb  = mem_total;
            rpt.mem_avail_kb  = mem_avail;

            char buf[sizeof(msg_header_t) + sizeof(resource_report_t)];
            int len = proto_build(buf, sizeof(buf), MSG_RESOURCE_REPORT,
                                  seq++, &rpt, sizeof(rpt));
            if (len <= 0) {
                comch_failures++;
            } else {
                ret = comch_host_send(comch_ctx, buf, (size_t)len);
                if (ret == DOCA_SUCCESS) {
                    comch_failures = 0;
                    total_comch_sent++;
                } else {
                    comch_failures++;
                    fprintf(stderr, "[metric_push] comch_host_send failed "
                            "(%d/%d)\n", comch_failures, FAILOVER_THRESHOLD);
                }
            }

            /* Switch to gRPC if threshold exceeded */
            if (comch_failures >= FAILOVER_THRESHOLD) {
                fprintf(stderr, "[metric_push] switching to gRPC fallback "
                        "after %d Comch failures\n", comch_failures);
                current_mode = Mode::GRPC_DIRECT;
                last_comch_retry_ns = now_ns();

                /* Destroy failed Comch context */
                if (comch_ctx) {
                    comch_host_destroy(comch_ctx);
                    comch_ctx = nullptr;
                }
            }

        } else {
            /* gRPC direct push mode */
            if (!grpc_channel) {
                grpc_channel = grpc::CreateChannel(
                    master_addr, grpc::InsecureChannelCredentials());
            }

            bool ok = grpc_push(grpc_channel, node_id, cpu,
                                mem_total, mem_avail, 0, 0);
            if (ok) {
                total_grpc_sent++;
                if (total_grpc_sent <= 3 || total_grpc_sent % 10 == 0)
                    fprintf(stderr, "[metric_push] gRPC push OK (#%" PRIu64 ")\n",
                            total_grpc_sent);
            } else {
                fprintf(stderr, "[metric_push] grpc DirectPush failed\n");
            }

            /* Periodically attempt Comch recovery */
            uint64_t elapsed = now_ns() - last_comch_retry_ns;
            if (elapsed >= COMCH_RETRY_INTERVAL_NS) {
                fprintf(stderr, "[metric_push] attempting Comch recovery...\n");
                ret = comch_host_init(&comch_ctx, pci, COMCH_SERVICE_NAME);
                if (ret == DOCA_SUCCESS) {
                    fprintf(stderr, "[metric_push] Comch recovered, "
                            "switching back\n");
                    current_mode = Mode::COMCH;
                    comch_failures = 0;
                    grpc_channel.reset();
                } else {
                    fprintf(stderr, "[metric_push] Comch still unavailable\n");
                    comch_ctx = nullptr;
                }
                last_comch_retry_ns = now_ns();
            }
        }
    }

    if (comch_ctx) comch_host_destroy(comch_ctx);

    fprintf(stderr, "[metric_push] exiting: comch_sent=%" PRIu64
            " grpc_sent=%" PRIu64 " total_seq=%u\n",
            total_comch_sent, total_grpc_sent, seq);
    return 0;
}
