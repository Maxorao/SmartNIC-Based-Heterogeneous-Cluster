/*
 * rdma_bridge_master.cc — Master-BF2 side of the RDMA metric hot-path.
 *
 * Role:
 *   1. Listen for RDMA connections from worker BF2 bridges.
 *   2. For each received bridge message, forward to cluster_master via
 *      gRPC DirectPush on the tmfifo link (192.168.100.1:50051 by default).
 *
 * Concurrency model:
 *   - Single RDMA server endpoint per worker instance. Run one process per
 *     worker BF2 (bind to different ports 7889/7890/7891 etc.)
 *     OR launch this binary with --port=N for each worker.
 *   - gRPC client uses a single channel with keepalive; reports are
 *     submitted via async stub to avoid head-of-line blocking.
 *
 * Usage:
 *   rdma_bridge_master --port=7889
 *                      [--bind-ip=192.168.56.102]
 *                      [--master-grpc=192.168.100.1:50051]
 *                      [--msg-size=4096]
 */

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>

#include <getopt.h>
#include <unistd.h>

#include <grpcpp/grpcpp.h>
#include "cluster.grpc.pb.h"

extern "C" {
#include "../../common/rdma_transport.h"
#include "rdma_bridge_common.h"
#include "../../common/protocol.h"
}

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using cluster::ClusterControl;
using cluster::DirectPushRequest;
using cluster::DirectPushResponse;

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running.store(false); }

struct Config {
    std::string bind_ip;
    uint16_t    port         = BRIDGE_DEFAULT_PORT;
    std::string master_grpc  = "192.168.100.1:50051";   /* tmfifo to host */
    uint32_t    msg_size     = BRIDGE_MAX_PAYLOAD + BRIDGE_HDR_SIZE;
};

/* --------------------------------------------------------------------- */
/* gRPC forwarder                                                          */
/* --------------------------------------------------------------------- */

class GrpcForwarder {
public:
    explicit GrpcForwarder(const std::string& addr) : addr_(addr) {
        connect();
    }

    void connect() {
        grpc::ChannelArguments args;
        args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
        args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
        args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        channel_ = grpc::CreateCustomChannel(
            addr_, grpc::InsecureChannelCredentials(), args);
        stub_ = ClusterControl::NewStub(channel_);
        fprintf(stderr, "[bridge_master] gRPC channel opened to %s\n",
                addr_.c_str());
    }

    bool forward_resource_report(const void* payload, uint32_t len) {
        if (len < sizeof(resource_report_t)) return false;
        const resource_report_t* rr = (const resource_report_t*)payload;

        DirectPushRequest req;
        req.set_node_id(std::string(rr->node_id, strnlen(rr->node_id, sizeof(rr->node_id))));
        req.set_timestamp_ns(rr->timestamp_ns);
        req.set_cpu_usage_pct(rr->cpu_usage_pct);
        req.set_mem_total_kb(rr->mem_total_kb);
        req.set_mem_avail_kb(rr->mem_avail_kb);
        req.set_net_rx_bytes(rr->net_rx_bytes);
        req.set_net_tx_bytes(rr->net_tx_bytes);

        DirectPushResponse resp;
        ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(500));
        Status st = stub_->DirectPush(&ctx, req, &resp);
        if (!st.ok()) {
            fprintf(stderr, "[bridge_master] DirectPush failed: %s\n",
                    st.error_message().c_str());
            return false;
        }
        return resp.accepted();
    }

private:
    std::string             addr_;
    std::shared_ptr<Channel> channel_;
    std::unique_ptr<ClusterControl::Stub> stub_;
};

/* --------------------------------------------------------------------- */
/* Main loop                                                               */
/* --------------------------------------------------------------------- */

int main(int argc, char** argv)
{
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    Config cfg;

    static struct option longopts[] = {
        {"bind-ip",     required_argument, 0, 'B'},
        {"port",        required_argument, 0, 'p'},
        {"master-grpc", required_argument, 0, 'G'},
        {"msg-size",    required_argument, 0, 's'},
        {"help",        no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "B:p:G:s:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'B': cfg.bind_ip = optarg; break;
        case 'p': cfg.port = (uint16_t)atoi(optarg); break;
        case 'G': cfg.master_grpc = optarg; break;
        case 's': cfg.msg_size = (uint32_t)atoi(optarg); break;
        case 'h':
            fprintf(stderr, "Usage: %s [--bind-ip=IP] [--port=N] "
                    "[--master-grpc=HOST:PORT] [--msg-size=N]\n", argv[0]);
            return 0;
        }
    }

    GrpcForwarder fwd(cfg.master_grpc);

    /* Outer loop: accept a worker connection, handle it until disconnect */
    while (g_running.load()) {
        fprintf(stderr, "[bridge_master] listening RDMA on %s:%u ...\n",
                cfg.bind_ip.empty() ? "*" : cfg.bind_ip.c_str(), cfg.port);

        rdma_endpoint_t* ep = rdma_endpoint_create_server(
            cfg.bind_ip.empty() ? nullptr : cfg.bind_ip.c_str(),
            cfg.port, cfg.msg_size, 256);
        if (!ep) {
            fprintf(stderr, "[bridge_master] accept failed, retry in 2s\n");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        fprintf(stderr, "[bridge_master] worker connected\n");

        std::vector<uint8_t> buf(cfg.msg_size);
        uint64_t recvd = 0, forwarded = 0, bad = 0;

        while (g_running.load() && rdma_endpoint_is_connected(ep)) {
            uint32_t rlen = 0;
            int rc = rdma_endpoint_recv(ep, buf.data(), buf.size(),
                                         &rlen, 1000000);
            if (rc == -ETIMEDOUT || rc == -EAGAIN) continue;
            if (rc < 0) {
                fprintf(stderr, "[bridge_master] recv err: %s\n",
                        rdma_endpoint_last_error(ep));
                break;
            }
            recvd++;

            if (rlen < sizeof(bridge_hdr_t)) { bad++; continue; }
            bridge_hdr_t* h = (bridge_hdr_t*)buf.data();
            if (h->magic != BRIDGE_MAGIC) { bad++; continue; }

            const void* payload = buf.data() + sizeof(bridge_hdr_t);
            uint32_t plen = rlen - (uint32_t)sizeof(bridge_hdr_t);

            switch (h->type) {
            case BRIDGE_MSG_RESOURCE_REPORT:
                if (fwd.forward_resource_report(payload, plen)) forwarded++;
                break;
            case BRIDGE_MSG_BF2_REPORT:
            case BRIDGE_MSG_STATUS_CHANGE:
                /* TODO: add BF2Report and StatusChange forwarders.
                 * For now, count as received but skip. */
                forwarded++;
                break;
            default:
                bad++;
                break;
            }

            if ((recvd % 5000) == 0) {
                fprintf(stderr, "[bridge_master] recvd=%lu fwd=%lu bad=%lu\n",
                        (unsigned long)recvd, (unsigned long)forwarded,
                        (unsigned long)bad);
            }
        }

        fprintf(stderr, "[bridge_master] worker disconnected "
                "(recvd=%lu fwd=%lu bad=%lu)\n",
                (unsigned long)recvd, (unsigned long)forwarded,
                (unsigned long)bad);
        rdma_endpoint_destroy(ep);
    }

    return 0;
}
