/*
 * slave_agent.cc — Intelligent management agent for BF2 SmartNIC.
 *
 * Runs on the BF2 ARM SoC (aarch64, Ubuntu 20.04).  Responsibilities:
 *
 *   1. Receive host metrics from metric_push via DOCA Comch (PCIe bypass)
 *   2. Collect BF2 local metrics (ARM CPU, memory, temperature, ports, OVS)
 *   3. Register with cluster_master via gRPC
 *   4. Send heartbeats and metrics over a gRPC bidirectional stream
 *   5. Detect host failures (Comch timeout) and report status changes
 *
 * Usage:
 *   slave_agent [OPTIONS]
 *
 * Options:
 *   --node-uuid=UUID     BF2 hardware UUID (default: read from sysfs)
 *   --hostname=NAME      Human-readable node name (default: system hostname)
 *   --master-addr=H:P    cluster_master gRPC endpoint (default: 192.168.1.1:50051)
 *   --dev-pci=ADDR       BF2 device PCI from ARM view (default: 03:00.0)
 *   --rep-pci=ADDR       Host representor PCI (default: auto)
 *   --heartbeat-ms=N     Heartbeat interval in ms (default: 3000)
 *   --report-ms=N        Host resource report interval (default: 3000)
 *   --bf2-report-ms=N    BF2 metrics report interval (default: 5000)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cstdarg>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <getopt.h>
#include <unistd.h>

#include <grpcpp/grpcpp.h>
#include "cluster.grpc.pb.h"

#include "bf2_collector.h"
#include "host_collector.h"

extern "C" {
#include "../../common/timing.h"
#include "../../common/node_state.h"
}

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using cluster::ClusterControl;
using cluster::NodeMessage;
using cluster::MasterMessage;

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static std::atomic<bool> g_running{true};

/* ------------------------------------------------------------------ */
/* Signal handling                                                     */
/* ------------------------------------------------------------------ */

static void signal_handler(int sig)
{
    (void)sig;
    g_running.store(false);
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static void sa_log(const char* fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm_info);
    fprintf(stderr, "[%s.%03ld] slave_agent: ", tbuf,
            ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

struct Config {
    std::string node_uuid;
    std::string hostname;
    std::string master_addr   = "192.168.1.1:50051";
    std::string dev_pci       = "03:00.0";
    std::string rep_pci       = "auto";
    uint32_t    heartbeat_ms  = 3000;
    uint32_t    report_ms     = 3000;
    uint32_t    bf2_report_ms = 5000;
};

/* ------------------------------------------------------------------ */
/* UUID helpers                                                        */
/* ------------------------------------------------------------------ */

/**
 * Try to read a hardware UUID from sysfs for the given PCI device.
 * Falls back to the system hostname if unavailable.
 */
static std::string read_bf2_uuid(const std::string& dev_pci)
{
    // Try /sys/bus/pci/devices/<pci>/device
    std::string path = "/sys/bus/pci/devices/0000:" + dev_pci + "/device";
    FILE* f = fopen(path.c_str(), "r");
    if (f) {
        char buf[128]{};
        if (fgets(buf, sizeof(buf), f)) {
            fclose(f);
            // Strip trailing newline
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                buf[--len] = '\0';
            if (len > 0)
                return std::string(buf);
        } else {
            fclose(f);
        }
    }

    // Try /etc/machine-id as fallback
    f = fopen("/etc/machine-id", "r");
    if (f) {
        char buf[64]{};
        if (fgets(buf, sizeof(buf), f)) {
            fclose(f);
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                buf[--len] = '\0';
            if (len > 0)
                return std::string(buf);
        } else {
            fclose(f);
        }
    }

    // Last resort
    char host[64]{};
    gethostname(host, sizeof(host) - 1);
    return std::string(host) + "-bf2";
}

static std::string get_hostname()
{
    char buf[64]{};
    gethostname(buf, sizeof(buf) - 1);
    return std::string(buf);
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

static Config parse_args(int argc, char* argv[])
{
    Config cfg;

    static struct option long_opts[] = {
        {"node-uuid",     required_argument, nullptr, 'u'},
        {"hostname",      required_argument, nullptr, 'n'},
        {"master-addr",   required_argument, nullptr, 'm'},
        {"dev-pci",       required_argument, nullptr, 'd'},
        {"rep-pci",       required_argument, nullptr, 'r'},
        {"heartbeat-ms",  required_argument, nullptr, 'h'},
        {"report-ms",     required_argument, nullptr, 'R'},
        {"bf2-report-ms", required_argument, nullptr, 'b'},
        {"help",          no_argument,       nullptr, 'H'},
        {nullptr, 0, nullptr, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'u': cfg.node_uuid    = optarg; break;
        case 'n': cfg.hostname     = optarg; break;
        case 'm': cfg.master_addr  = optarg; break;
        case 'd': cfg.dev_pci      = optarg; break;
        case 'r': cfg.rep_pci      = optarg; break;
        case 'h': cfg.heartbeat_ms = static_cast<uint32_t>(atoi(optarg)); break;
        case 'R': cfg.report_ms    = static_cast<uint32_t>(atoi(optarg)); break;
        case 'b': cfg.bf2_report_ms = static_cast<uint32_t>(atoi(optarg)); break;
        case 'H':
        default:
            fprintf(stderr,
                "Usage: %s [OPTIONS]\n"
                "  --node-uuid=UUID        BF2 hardware UUID\n"
                "  --hostname=NAME         Human-readable name\n"
                "  --master-addr=HOST:PORT gRPC endpoint (default: 192.168.1.1:50051)\n"
                "  --dev-pci=ADDR          BF2 device PCI (default: 03:00.0)\n"
                "  --rep-pci=ADDR          Host representor PCI (default: auto)\n"
                "  --heartbeat-ms=N        Heartbeat interval (default: 3000)\n"
                "  --report-ms=N           Host report interval (default: 3000)\n"
                "  --bf2-report-ms=N       BF2 report interval (default: 5000)\n",
                argv[0]);
            exit(c == 'H' ? 0 : 1);
        }
    }

    if (cfg.node_uuid.empty())
        cfg.node_uuid = read_bf2_uuid(cfg.dev_pci);
    if (cfg.hostname.empty())
        cfg.hostname = get_hostname();

    return cfg;
}

/* ------------------------------------------------------------------ */
/* gRPC channel with keepalive                                         */
/* ------------------------------------------------------------------ */

static std::shared_ptr<Channel> make_channel(const std::string& addr)
{
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS,         10000);  // 10s
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,       5000);  // 5s
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    args.SetInt(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
    args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);

    return grpc::CreateCustomChannel(addr,
                                     grpc::InsecureChannelCredentials(),
                                     args);
}

/* ------------------------------------------------------------------ */
/* Master message reader thread                                        */
/* ------------------------------------------------------------------ */

static void reader_thread_fn(
    grpc::ClientReaderWriter<NodeMessage, MasterMessage>* stream,
    std::atomic<bool>* stream_ok)
{
    MasterMessage msg;
    while (stream->Read(&msg)) {
        if (msg.has_heartbeat_ack()) {
            // Nothing to do; heartbeat acknowledged.
        } else if (msg.has_register_ack()) {
            const auto& ack = msg.register_ack();
            if (ack.accepted()) {
                sa_log("registration accepted, assigned_id=%s "
                       "hb_interval=%ums report_interval=%ums",
                       ack.assigned_id().c_str(),
                       ack.heartbeat_interval_ms(),
                       ack.report_interval_ms());
            } else {
                sa_log("registration REJECTED");
            }
        } else if (msg.has_report_ack()) {
            // Report acknowledged.
        } else if (msg.has_command()) {
            const auto& cmd = msg.command();
            sa_log("received command: id=%s action=%s",
                   cmd.command_id().c_str(), cmd.action().c_str());
            // TODO: execute command and send result
        } else if (msg.has_cluster_view()) {
            const auto& cv = msg.cluster_view();
            sa_log("cluster view: total=%u online=%u suspect=%u offline=%u",
                   cv.total_nodes(), cv.online_nodes(),
                   cv.suspect_nodes(), cv.offline_nodes());
        }
    }

    // Stream ended (server closed or network failure).
    sa_log("reader: stream ended");
    stream_ok->store(false);
}

/* ------------------------------------------------------------------ */
/* Build protobuf messages                                             */
/* ------------------------------------------------------------------ */

static NodeMessage make_register_request(const Config& cfg)
{
    NodeMessage nm;
    auto* req = nm.mutable_register_req();
    req->set_node_uuid(cfg.node_uuid);
    req->set_hostname(cfg.hostname);
    req->set_host_hostname("");  // filled when host registers via Comch
    req->set_pci_bus_id(cfg.dev_pci);
    req->set_version("1.0.0");

    auto* caps = req->mutable_caps();
    caps->set_arm_cores(8);       // BF2 has 8 ARM A72 cores
    caps->set_mem_mb(16384);      // 16 GB
    caps->set_port_speed_gbps(100);
    caps->set_doca_version("1.5");

    return nm;
}

static NodeMessage make_heartbeat(uint64_t seq,
                                   bool host_alive,
                                   bool bf2_ok)
{
    NodeMessage nm;
    auto* hb = nm.mutable_heartbeat();
    hb->set_seq(seq);
    hb->set_timestamp_ns(unix_ns());
    hb->set_host_status(host_alive ? cluster::DOMAIN_OK
                                   : cluster::DOMAIN_UNREACHABLE);
    hb->set_bf2_status(bf2_ok ? cluster::DOMAIN_OK
                              : cluster::DOMAIN_DEGRADED);
    return nm;
}

static NodeMessage make_resource_report(const std::string& uuid,
                                         const HostCollector::HostMetrics& hm)
{
    NodeMessage nm;
    auto* rr = nm.mutable_resource_report();
    rr->set_node_uuid(uuid);
    rr->set_timestamp_ns(hm.timestamp_ns);
    rr->set_cpu_usage_pct(hm.cpu_pct);
    rr->set_mem_total_kb(hm.mem_total_kb);
    rr->set_mem_avail_kb(hm.mem_avail_kb);
    rr->set_net_rx_bytes(hm.net_rx_bytes);
    rr->set_net_tx_bytes(hm.net_tx_bytes);
    return nm;
}

static NodeMessage make_bf2_report(const std::string& uuid,
                                    const BF2Collector::BF2Metrics& bm)
{
    NodeMessage nm;
    auto* br = nm.mutable_bf2_report();
    br->set_node_uuid(uuid);
    br->set_timestamp_ns(bm.timestamp_ns);
    br->set_arm_cpu_pct(bm.arm_cpu_pct);
    br->set_arm_mem_total_kb(bm.arm_mem_total_kb);
    br->set_arm_mem_avail_kb(bm.arm_mem_avail_kb);
    br->set_temperature_c(bm.temperature_c);
    br->set_port_rx_bytes(bm.port_rx_bytes);
    br->set_port_tx_bytes(bm.port_tx_bytes);
    br->set_port_rx_drops(bm.port_rx_drops);
    br->set_ovs_flow_count(bm.ovs_flow_count);
    return nm;
}

static NodeMessage make_status_change(const std::string& domain,
                                       cluster::DomainStatus old_s,
                                       cluster::DomainStatus new_s,
                                       const std::string& reason)
{
    NodeMessage nm;
    auto* sc = nm.mutable_status_change();
    sc->set_domain(domain);
    sc->set_old_status(old_s);
    sc->set_new_status(new_s);
    sc->set_reason(reason);
    return nm;
}

static NodeMessage make_deregister(const std::string& uuid,
                                    const std::string& reason)
{
    NodeMessage nm;
    auto* dr = nm.mutable_deregister();
    dr->set_node_uuid(uuid);
    dr->set_reason(reason);
    return nm;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char* argv[])
{
    Config cfg = parse_args(argc, argv);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    sa_log("starting: uuid=%s hostname=%s master=%s",
           cfg.node_uuid.c_str(), cfg.hostname.c_str(),
           cfg.master_addr.c_str());

    // ---- Start HostCollector (Comch NIC side) ----

    HostCollector host_collector(cfg.dev_pci.c_str(),
                                 cfg.rep_pci.c_str(),
                                 "cluster-control");
    if (!host_collector.start()) {
        sa_log("WARNING: HostCollector failed to start; "
               "host metrics will be unavailable");
    }

    // ---- Init BF2 local metrics collector ----

    BF2Collector bf2_collector;
    bf2_collector.init();

    // ---- Main reconnection loop ----

    uint32_t backoff_s = 3;
    static constexpr uint32_t MAX_BACKOFF_S = 30;

    while (g_running.load()) {
        sa_log("connecting to master at %s ...", cfg.master_addr.c_str());

        auto channel = make_channel(cfg.master_addr);

        // Wait for the channel to become ready (up to 10s).
        auto deadline = std::chrono::system_clock::now()
                      + std::chrono::seconds(10);
        if (!channel->WaitForConnected(deadline)) {
            sa_log("channel not ready after 10s, retrying in %us...",
                   backoff_s);
            for (uint32_t i = 0; i < backoff_s && g_running.load(); ++i)
                sleep(1);
            backoff_s = std::min(backoff_s * 2, MAX_BACKOFF_S);
            continue;
        }

        auto stub = ClusterControl::NewStub(channel);

        ClientContext ctx;
        auto stream = stub->NodeSession(&ctx);
        if (!stream) {
            sa_log("failed to open NodeSession stream");
            for (uint32_t i = 0; i < backoff_s && g_running.load(); ++i)
                sleep(1);
            backoff_s = std::min(backoff_s * 2, MAX_BACKOFF_S);
            continue;
        }

        // ---- Send RegisterRequest ----
        {
            NodeMessage reg_msg = make_register_request(cfg);
            if (!stream->Write(reg_msg)) {
                sa_log("failed to send RegisterRequest");
                stream->WritesDone();
                stream->Finish();
                continue;
            }
            sa_log("RegisterRequest sent");
        }

        // ---- Spawn reader thread ----
        std::atomic<bool> stream_ok{true};
        std::thread reader(reader_thread_fn, stream.get(), &stream_ok);

        // Reset backoff on successful connection.
        backoff_s = 3;

        // ---- Reporting loop ----
        uint64_t hb_seq = 0;
        uint64_t last_hb_ns        = 0;
        uint64_t last_report_ns    = 0;
        uint64_t last_bf2_ns       = 0;

        // Track host reachability for status change notifications.
        bool prev_host_alive = true;  // optimistic initial state

        while (g_running.load() && stream_ok.load()) {

            uint64_t now = now_ns();
            bool host_alive = host_collector.isAlive();
            bool bf2_ok = true;  // BF2 is always "ok" if we're running

            // Detect host reachability change.
            if (host_alive != prev_host_alive) {
                cluster::DomainStatus old_s = prev_host_alive
                    ? cluster::DOMAIN_OK : cluster::DOMAIN_UNREACHABLE;
                cluster::DomainStatus new_s = host_alive
                    ? cluster::DOMAIN_OK : cluster::DOMAIN_UNREACHABLE;
                std::string reason = host_alive
                    ? "Comch connection restored"
                    : "Comch timeout — host unreachable";

                NodeMessage sc = make_status_change("host", old_s, new_s, reason);
                if (!stream->Write(sc)) {
                    sa_log("failed to write StatusChangeNotice");
                    stream_ok.store(false);
                    break;
                }
                sa_log("host status changed: %s -> %s",
                       prev_host_alive ? "OK" : "UNREACHABLE",
                       host_alive ? "OK" : "UNREACHABLE");
                prev_host_alive = host_alive;
            }

            // ---- Heartbeat ----
            uint64_t hb_interval = static_cast<uint64_t>(cfg.heartbeat_ms)
                                   * 1000000ULL;
            if (now - last_hb_ns >= hb_interval) {
                NodeMessage hb = make_heartbeat(hb_seq++, host_alive, bf2_ok);
                if (!stream->Write(hb)) {
                    sa_log("failed to write heartbeat");
                    stream_ok.store(false);
                    break;
                }
                last_hb_ns = now;
            }

            // ---- Host resource report ----
            uint64_t report_interval = static_cast<uint64_t>(cfg.report_ms)
                                       * 1000000ULL;
            if (now - last_report_ns >= report_interval) {
                if (host_alive) {
                    auto hm = host_collector.getLatest();
                    if (hm.valid) {
                        NodeMessage rr = make_resource_report(cfg.node_uuid, hm);
                        if (!stream->Write(rr)) {
                            sa_log("failed to write ResourceReport");
                            stream_ok.store(false);
                            break;
                        }
                    }
                }
                last_report_ns = now;
            }

            // ---- BF2 metrics report ----
            uint64_t bf2_interval = static_cast<uint64_t>(cfg.bf2_report_ms)
                                    * 1000000ULL;
            if (now - last_bf2_ns >= bf2_interval) {
                auto bm = bf2_collector.collect();
                NodeMessage br = make_bf2_report(cfg.node_uuid, bm);
                if (!stream->Write(br)) {
                    sa_log("failed to write BF2MetricsReport");
                    stream_ok.store(false);
                    break;
                }
                last_bf2_ns = now;
            }

            // Poll interval — balance responsiveness with CPU usage.
            sleep_ms(100);
        }

        // ---- Stream broke or shutting down ----

        // Send deregister if we're shutting down gracefully.
        if (!g_running.load() && stream_ok.load()) {
            NodeMessage dr = make_deregister(cfg.node_uuid, "graceful shutdown");
            stream->Write(dr);
        }

        stream->WritesDone();
        Status status = stream->Finish();
        if (!status.ok()) {
            sa_log("stream finished with error: %s (code %d)",
                   status.error_message().c_str(),
                   static_cast<int>(status.error_code()));
        }

        // Wait for reader thread to exit (it will see stream closed).
        if (reader.joinable())
            reader.join();

        if (g_running.load()) {
            sa_log("connection lost, reconnecting in %us...", backoff_s);
            for (uint32_t i = 0; i < backoff_s && g_running.load(); ++i)
                sleep(1);
            backoff_s = std::min(backoff_s * 2, MAX_BACKOFF_S);
        }
    }

    // ---- Cleanup ----

    host_collector.stop();
    sa_log("exiting");
    return 0;
}
