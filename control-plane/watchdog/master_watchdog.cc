/*
 * master_watchdog.cc — Master health watchdog running on master's BF2 ARM.
 *
 * Monitors cluster_master health via two independent mechanisms:
 *   1. Comch health ping: periodic MSG_HEARTBEAT to cluster_master on host.
 *      If no response for 5 consecutive checks (~15s), marks master unhealthy.
 *   2. gRPC health check: calls MasterHealth::Ping() on cluster_master.
 *      If fails for 30s continuously, marks master failed.
 *
 * On master failure:
 *   - Attempt systemctl restart cluster_master (up to 3 retries)
 *   - If all retries fail, send FailoverControl::TriggerFailover() to standby
 *
 * Usage:
 *   master_watchdog --dev-pci=03:00.0 --rep-pci=03:00.1 \
 *       --master-grpc-addr=localhost:50051 --standby-addr=10.0.0.2:50052 \
 *       --check-interval-ms=3000
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
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
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

struct Config {
    std::string dev_pci       = "03:00.0";
    std::string rep_pci       = "auto";
    std::string master_addr   = "localhost:50051";
    std::string standby_addr;               // empty = no standby
    uint32_t    check_interval_ms = 3000;
};

/* ------------------------------------------------------------------ */
/* Comch health monitor                                                 */
/* ------------------------------------------------------------------ */

static bool comch_health_check(comch_nic_ctx_t *ctx, uint32_t seq,
                               uint32_t timeout_ms)
{
    char buf[sizeof(msg_header_t) + sizeof(bench_ping_t)];
    bench_ping_t ping = {};
    ping.send_ts_ns = unix_ns();
    ping.seq = seq;

    int len = proto_build(buf, sizeof(buf), MSG_HEARTBEAT, seq,
                          &ping, sizeof(ping));
    if (len <= 0) return false;

    doca_error_t ret = comch_nic_send(ctx, buf, (size_t)len);
    if (ret != DOCA_SUCCESS) return false;

    char recv_buf[COMCH_MAX_MSG_SIZE];
    size_t recv_len = sizeof(recv_buf);
    ret = comch_nic_recv_blocking(ctx, recv_buf, &recv_len, timeout_ms);
    if (ret != DOCA_SUCCESS) return false;

    msg_header_t *hdr = (msg_header_t *)recv_buf;
    if (proto_validate(hdr, recv_len) != 0) return false;

    return (hdr->type == MSG_HEARTBEAT_ACK);
}

/* ------------------------------------------------------------------ */
/* gRPC health monitor                                                  */
/* ------------------------------------------------------------------ */

static bool grpc_health_check(const std::string &addr)
{
    auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    auto stub = cluster::MasterHealth::NewStub(channel);

    cluster::HealthPingRequest req;
    req.set_timestamp_ns(unix_ns());

    cluster::HealthPingResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(5));

    grpc::Status status = stub->Ping(&ctx, req, &resp);
    return status.ok() && resp.healthy();
}

/* ------------------------------------------------------------------ */
/* Recovery actions                                                     */
/* ------------------------------------------------------------------ */

static bool attempt_restart(int max_retries)
{
    for (int i = 0; i < max_retries; i++) {
        fprintf(stderr, "[watchdog] restart attempt %d/%d: "
                "systemctl restart cluster_master\n", i + 1, max_retries);
        int rc = system("systemctl restart cluster_master");
        if (rc != 0) {
            fprintf(stderr, "[watchdog] systemctl returned %d\n", rc);
        }

        /* Wait for service to come up */
        sleep_ms(5000);

        /* Verify via gRPC */
        /* Note: the caller will re-check; just return whether systemctl
         * reported success */
        if (rc == 0) return true;
    }
    return false;
}

static void trigger_failover(const std::string &standby_addr,
                             const std::string &reason)
{
    fprintf(stderr, "[watchdog] CRITICAL: triggering failover to %s\n",
            standby_addr.c_str());

    auto channel = grpc::CreateChannel(standby_addr,
                                       grpc::InsecureChannelCredentials());
    auto stub = cluster::FailoverControl::NewStub(channel);

    cluster::FailoverRequest req;
    req.set_reason(reason);
    req.set_timestamp_ns(unix_ns());

    cluster::FailoverResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(10));

    grpc::Status status = stub->TriggerFailover(&ctx, req, &resp);
    if (status.ok() && resp.accepted()) {
        fprintf(stderr, "[watchdog] failover accepted by standby: %s\n",
                resp.new_master_addr().c_str());
    } else {
        fprintf(stderr, "[watchdog] failover request FAILED: %s\n",
                status.error_message().c_str());
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                            */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    Config cfg;

    static struct option opts[] = {
        {"dev-pci",           required_argument, nullptr, 'd'},
        {"rep-pci",           required_argument, nullptr, 'r'},
        {"master-grpc-addr",  required_argument, nullptr, 'm'},
        {"standby-addr",      required_argument, nullptr, 's'},
        {"check-interval-ms", required_argument, nullptr, 'i'},
        {"help",              no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", opts, nullptr)) != -1) {
        switch (c) {
        case 'd': cfg.dev_pci       = optarg; break;
        case 'r': cfg.rep_pci       = optarg; break;
        case 'm': cfg.master_addr   = optarg; break;
        case 's': cfg.standby_addr  = optarg; break;
        case 'i': cfg.check_interval_ms = (uint32_t)atoi(optarg); break;
        default:
            fprintf(stderr,
                "Usage: %s --dev-pci=PCI --rep-pci=PCI "
                "--master-grpc-addr=ADDR --standby-addr=ADDR "
                "[--check-interval-ms=MS]\n", argv[0]);
            return (c == 'h') ? 0 : 1;
        }
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize Comch NIC-side */
    comch_nic_ctx_t *comch_ctx = nullptr;
    const char *rep = cfg.rep_pci.c_str();
    if (cfg.rep_pci == "auto") rep = nullptr;

    doca_error_t ret = comch_nic_init(&comch_ctx, cfg.dev_pci.c_str(),
                                      rep, COMCH_SERVICE_NAME);
    if (ret != DOCA_SUCCESS) {
        fprintf(stderr, "[watchdog] comch_nic_init failed (continuing with "
                "gRPC-only monitoring)\n");
        comch_ctx = nullptr;
    }

    fprintf(stderr, "[watchdog] started: master=%s standby=%s interval=%ums\n",
            cfg.master_addr.c_str(),
            cfg.standby_addr.empty() ? "(none)" : cfg.standby_addr.c_str(),
            cfg.check_interval_ms);

    const int COMCH_FAIL_LIMIT = 5;         /* 5 missed = ~15s at 3s interval */
    const int GRPC_FAIL_LIMIT  = 10;        /* 10 missed = ~30s at 3s interval */
    const int RESTART_RETRIES  = 3;

    int comch_consecutive_fails = 0;
    int grpc_consecutive_fails  = 0;
    uint32_t seq = 0;
    bool master_failed = false;

    while (g_running.load()) {
        bool comch_ok = false;
        bool grpc_ok  = false;

        /* Comch health ping */
        if (comch_ctx) {
            comch_ok = comch_health_check(comch_ctx, seq++,
                                          cfg.check_interval_ms);
            if (comch_ok) {
                comch_consecutive_fails = 0;
            } else {
                comch_consecutive_fails++;
                fprintf(stderr, "[watchdog] comch check failed (%d/%d)\n",
                        comch_consecutive_fails, COMCH_FAIL_LIMIT);
            }
        }

        /* gRPC health check */
        grpc_ok = grpc_health_check(cfg.master_addr);
        if (grpc_ok) {
            grpc_consecutive_fails = 0;
        } else {
            grpc_consecutive_fails++;
            fprintf(stderr, "[watchdog] grpc check failed (%d/%d)\n",
                    grpc_consecutive_fails, GRPC_FAIL_LIMIT);
        }

        /* Evaluate health */
        bool comch_unhealthy = (comch_ctx != nullptr) &&
                               (comch_consecutive_fails >= COMCH_FAIL_LIMIT);
        bool grpc_unhealthy  = (grpc_consecutive_fails >= GRPC_FAIL_LIMIT);

        if ((comch_unhealthy || grpc_unhealthy) && !master_failed) {
            fprintf(stderr, "[watchdog] CRITICAL: master considered unhealthy "
                    "(comch_fails=%d grpc_fails=%d)\n",
                    comch_consecutive_fails, grpc_consecutive_fails);

            /* Attempt restart */
            bool restarted = attempt_restart(RESTART_RETRIES);

            if (restarted) {
                /* Verify recovery via gRPC */
                sleep_ms(3000);
                if (grpc_health_check(cfg.master_addr)) {
                    fprintf(stderr, "[watchdog] master recovered after restart\n");
                    comch_consecutive_fails = 0;
                    grpc_consecutive_fails  = 0;
                    continue;
                }
            }

            /* Restart failed — trigger failover */
            master_failed = true;
            if (!cfg.standby_addr.empty()) {
                trigger_failover(cfg.standby_addr,
                                 "master unresponsive after restart attempts");
            } else {
                fprintf(stderr, "[watchdog] no standby configured, "
                        "cannot failover\n");
            }
        }

        sleep_ms(cfg.check_interval_ms);
    }

    if (comch_ctx) comch_nic_destroy(comch_ctx);
    fprintf(stderr, "[watchdog] shutting down\n");
    return 0;
}
