/*
 * mock_slave.cc — gRPC scalability test for cluster_master.
 *
 * Simulates N slave nodes as threads.  Each thread opens a
 * ClusterControl::NodeSession bidirectional stream, sends a RegisterRequest,
 * then loops sending HeartbeatPing + ResourceReport at the configured
 * interval for the test duration.
 *
 * Measures: registration latency, report-to-ACK latency, error counts.
 * Prints a summary table matching the existing mock_slave.c format.
 *
 * Usage:
 *   mock_slave --master-addr=localhost:50051 --nodes=4 \
 *       --duration=60 --interval=1000
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <getopt.h>
#include <inttypes.h>

#include <grpcpp/grpcpp.h>
#include "cluster.grpc.pb.h"
#include "cluster.pb.h"

extern "C" {
#include "../../common/timing.h"
}

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */

static std::atomic<bool> g_stop{false};

static void sig_handler(int) { g_stop.store(true); }

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

static std::string g_master_addr = "localhost:50051";
static int         g_nodes       = 4;
static int         g_duration    = 60;      /* seconds */
static uint32_t    g_interval_ms = 1000;

/* ------------------------------------------------------------------ */
/* Per-node statistics                                                  */
/* ------------------------------------------------------------------ */

struct NodeStats {
    uint64_t msgs_sent       = 0;
    uint64_t msgs_acked      = 0;
    uint64_t send_errors     = 0;
    uint64_t connect_errors  = 0;
    uint64_t latency_sum_us  = 0;   /* sum of report->ACK latency */
    uint64_t reg_latency_us  = 0;   /* registration round-trip */
};

static std::vector<NodeStats> g_stats;

/* ------------------------------------------------------------------ */
/* Reader thread: drains MasterMessage from bidi stream                 */
/* ------------------------------------------------------------------ */

struct ReaderState {
    std::atomic<uint64_t> acks_received{0};
    std::atomic<uint64_t> last_ack_ts_ns{0};
    std::atomic<bool>     registered{false};
    std::atomic<uint64_t> reg_ack_ts_ns{0};
    std::atomic<bool>     done{false};
};

static void reader_func(
    grpc::ClientReaderWriter<cluster::NodeMessage, cluster::MasterMessage> *stream,
    ReaderState *rs)
{
    cluster::MasterMessage msg;
    while (stream->Read(&msg)) {
        uint64_t now = now_ns();
        if (msg.has_register_ack()) {
            rs->registered.store(true);
            rs->reg_ack_ts_ns.store(now);
        } else if (msg.has_heartbeat_ack() || msg.has_report_ack()) {
            rs->acks_received.fetch_add(1);
            rs->last_ack_ts_ns.store(now);
        }
        /* Ignore other message types (commands, cluster_view, etc.) */
    }
    rs->done.store(true);
}

/* ------------------------------------------------------------------ */
/* Per-node thread                                                      */
/* ------------------------------------------------------------------ */

static void node_thread(int idx)
{
    char node_id[32];
    snprintf(node_id, sizeof(node_id), "mock-node-%04d", idx);

    NodeStats &st = g_stats[idx];

    /* Create gRPC channel and stub */
    auto channel = grpc::CreateChannel(g_master_addr,
                                       grpc::InsecureChannelCredentials());

    /* Wait for channel to be ready (up to 5s) */
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
    if (!channel->WaitForConnected(deadline)) {
        fprintf(stderr, "[%s] failed to connect to %s\n",
                node_id, g_master_addr.c_str());
        st.connect_errors++;
        return;
    }

    auto stub = cluster::ClusterControl::NewStub(channel);

    /* Open bidi stream */
    grpc::ClientContext ctx;
    auto stream = stub->NodeSession(&ctx);
    if (!stream) {
        fprintf(stderr, "[%s] failed to open NodeSession stream\n", node_id);
        st.connect_errors++;
        return;
    }

    /* Start reader thread */
    ReaderState rs;
    std::thread reader(reader_func, stream.get(), &rs);

    /* Send RegisterRequest */
    {
        cluster::NodeMessage reg_msg;
        auto *reg = reg_msg.mutable_register_req();
        reg->set_node_uuid(node_id);
        reg->set_hostname(node_id);
        reg->set_host_hostname(std::string("host-of-") + node_id);
        reg->set_version("mock-grpc-1.0");

        auto *caps = reg->mutable_caps();
        caps->set_arm_cores(8);
        caps->set_mem_mb(16384);
        caps->set_port_speed_gbps(100);
        caps->set_doca_version("1.5");

        uint64_t reg_t0 = now_ns();
        if (!stream->Write(reg_msg)) {
            fprintf(stderr, "[%s] failed to send RegisterRequest\n", node_id);
            st.connect_errors++;
            stream->WritesDone();
            reader.join();
            return;
        }

        /* Wait for RegisterAck (up to 3s) */
        for (int w = 0; w < 30 && !rs.registered.load() && !g_stop.load(); w++)
            sleep_ms(100);

        if (rs.registered.load()) {
            st.reg_latency_us = (rs.reg_ack_ts_ns.load() - reg_t0) / 1000;
        } else {
            fprintf(stderr, "[%s] register timeout\n", node_id);
            st.connect_errors++;
        }
    }

    /* Main reporting loop */
    uint64_t end_ns = now_ns() + (uint64_t)g_duration * 1000000000ULL;
    uint64_t hb_seq = 0;

    while (!g_stop.load() && now_ns() < end_ns) {
        /* HeartbeatPing */
        {
            cluster::NodeMessage hb_msg;
            auto *hb = hb_msg.mutable_heartbeat();
            hb->set_seq(hb_seq);
            hb->set_timestamp_ns(unix_ns());
            hb->set_host_status(cluster::DOMAIN_OK);
            hb->set_bf2_status(cluster::DOMAIN_OK);

            if (!stream->Write(hb_msg)) {
                st.send_errors++;
                break;
            }
            st.msgs_sent++;
        }

        /* ResourceReport with synthetic data (same pattern as mock_slave.c) */
        {
            cluster::NodeMessage rpt_msg;
            auto *rpt = rpt_msg.mutable_resource_report();
            rpt->set_node_uuid(node_id);
            rpt->set_timestamp_ns(unix_ns());
            rpt->set_cpu_usage_pct(20.0f + (float)(idx % 60));
            rpt->set_mem_total_kb(16ULL * 1024 * 1024);   /* 16 GB */
            rpt->set_mem_avail_kb(8ULL  * 1024 * 1024);
            rpt->set_net_rx_bytes((uint64_t)idx * 1000000 + st.msgs_sent);
            rpt->set_net_tx_bytes((uint64_t)idx * 500000  + st.msgs_sent);

            uint64_t t0 = now_ns();
            uint64_t acks_before = rs.acks_received.load();

            if (!stream->Write(rpt_msg)) {
                st.send_errors++;
                break;
            }
            st.msgs_sent++;

            /* Wait briefly for ACK (non-blocking style: up to 500ms) */
            for (int w = 0; w < 50; w++) {
                if (rs.acks_received.load() > acks_before) {
                    uint64_t lat_us = (now_ns() - t0) / 1000;
                    st.latency_sum_us += lat_us;
                    st.msgs_acked++;
                    break;
                }
                sleep_ms(10);
            }
        }

        hb_seq++;
        sleep_ms(g_interval_ms);
    }

    /* Clean shutdown */
    stream->WritesDone();

    /* Wait for reader to finish (stream will be closed by server or timeout) */
    if (reader.joinable()) {
        /* Give reader up to 2s to drain */
        for (int w = 0; w < 20 && !rs.done.load(); w++)
            sleep_ms(100);
        ctx.TryCancel();
        reader.join();
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    static struct option long_opts[] = {
        {"master-addr", required_argument, nullptr, 'a'},
        {"nodes",       required_argument, nullptr, 'n'},
        {"duration",    required_argument, nullptr, 'd'},
        {"interval",    required_argument, nullptr, 't'},
        {"help",        no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'a': g_master_addr = optarg; break;
        case 'n': g_nodes       = atoi(optarg); break;
        case 'd': g_duration    = atoi(optarg); break;
        case 't': g_interval_ms = (uint32_t)atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr,
                "Usage: %s --master-addr=ADDR --nodes=N "
                "--duration=SECS [--interval=MS]\n", argv[0]);
            return (c == 'h') ? 0 : 1;
        }
    }

    if (g_nodes <= 0 || g_nodes > 65536) {
        fprintf(stderr, "nodes must be 1..65536\n");
        return 1;
    }

    struct sigaction sa_sig{};
    sa_sig.sa_handler = sig_handler;
    sigemptyset(&sa_sig.sa_mask);
    sa_sig.sa_flags = 0;
    sigaction(SIGTERM, &sa_sig, nullptr);
    sigaction(SIGINT,  &sa_sig, nullptr);
    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr,
            "mock_slave: master=%s  nodes=%d  duration=%ds  interval=%ums\n",
            g_master_addr.c_str(), g_nodes, g_duration, g_interval_ms);

    g_stats.resize(g_nodes);

    /* Launch all node threads with small stagger */
    std::vector<std::thread> threads;
    threads.reserve(g_nodes);

    for (int i = 0; i < g_nodes; i++) {
        threads.emplace_back(node_thread, i);

        /* Stagger: 5ms per node, brief pause every 100 */
        if (g_nodes > 1 && i % 100 == 99)
            sleep_ms(50);
        else
            sleep_ms(5);
    }

    /* Wait for all threads */
    for (auto &t : threads) {
        if (t.joinable()) t.join();
    }

    /* Print summary (same format as mock_slave.c) */
    uint64_t total_sent   = 0, total_acked  = 0, total_errors = 0;
    uint64_t total_lat    = 0, lat_count    = 0;

    printf("\n=== mock_slave results (%d nodes) ===\n", g_nodes);
    printf("%-16s  %8s  %8s  %8s  %12s  %12s\n",
           "node_id", "sent", "acked", "errors", "avg_lat_ms", "reg_lat_ms");

    int print_limit = (g_nodes > 20) ? 20 : g_nodes;
    for (int i = 0; i < print_limit; i++) {
        NodeStats &s = g_stats[i];
        double avg_lat_ms = s.msgs_acked > 0
                          ? (double)s.latency_sum_us / (double)s.msgs_acked / 1000.0
                          : 0.0;
        double reg_lat_ms = (double)s.reg_latency_us / 1000.0;
        printf("mock-node-%04d  %8" PRIu64 "  %8" PRIu64 "  %8" PRIu64
               "  %12.3f  %12.3f\n",
               i, s.msgs_sent, s.msgs_acked, s.send_errors,
               avg_lat_ms, reg_lat_ms);
        total_sent   += s.msgs_sent;
        total_acked  += s.msgs_acked;
        total_errors += s.send_errors;
        if (s.msgs_acked > 0) {
            total_lat  += s.latency_sum_us;
            lat_count  += s.msgs_acked;
        }
    }

    if (g_nodes > 20) {
        for (int i = 20; i < g_nodes; i++) {
            NodeStats &s = g_stats[i];
            total_sent   += s.msgs_sent;
            total_acked  += s.msgs_acked;
            total_errors += s.send_errors;
            if (s.msgs_acked > 0) {
                total_lat += s.latency_sum_us;
                lat_count += s.msgs_acked;
            }
        }
        printf("... (%d more nodes not shown individually)\n", g_nodes - 20);
    }

    double overall_avg_lat_ms = lat_count > 0
                              ? (double)total_lat / (double)lat_count / 1000.0
                              : 0.0;
    printf("\nTOTAL: sent=%" PRIu64 "  acked=%" PRIu64
           "  errors=%" PRIu64 "  avg_lat=%.3f ms\n",
           total_sent, total_acked, total_errors, overall_avg_lat_ms);

    if (total_sent > 0) {
        printf("Error rate: %.2f%%\n",
               (double)total_errors / (double)total_sent * 100.0);
    }

    return 0;
}
