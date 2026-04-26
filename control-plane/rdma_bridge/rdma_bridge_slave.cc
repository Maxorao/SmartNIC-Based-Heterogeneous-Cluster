/*
 * rdma_bridge_slave.cc — Worker-BF2 side of the RDMA metric hot-path.
 *
 * Role:
 *   1. Connect to master BF2 via RDMA Send/Recv
 *   2. Listen on a Unix domain socket (UDS) for metric reports from slave_agent
 *   3. Forward each UDS message over RDMA
 *
 * Connection lifecycle:
 *   - Retries RDMA connect on failure with exponential backoff (1s → 30s)
 *   - Reconnects on RDMA disconnect
 *   - Graceful shutdown on SIGINT/SIGTERM
 *
 * Latency-critical: busy-poll RDMA send CQ on the hot path.
 *
 * Usage:
 *   rdma_bridge_slave --master-ip=192.168.56.102 [--port=7889]
 *                     [--uds=/var/run/rdma_bridge.sock]
 *                     [--msg-size=4096]
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

#include <vector>

#include <getopt.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

extern "C" {
#include "../../common/rdma_transport.h"
#include "rdma_bridge_common.h"
}

static std::atomic<bool> g_running{true};
static void signal_handler(int) { g_running.store(false); }

struct Config {
    std::string master_ip;
    uint16_t    port      = BRIDGE_DEFAULT_PORT;
    std::string uds_path  = BRIDGE_UDS_DEFAULT_PATH;
    uint32_t    msg_size  = BRIDGE_MAX_PAYLOAD + BRIDGE_HDR_SIZE;
};

/* --------------------------------------------------------------------- */
/* UDS server                                                              */
/* --------------------------------------------------------------------- */

static int uds_server_setup(const std::string& path)
{
    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    /* Remove stale socket */
    ::unlink(path.c_str());

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); ::close(fd); return -1;
    }
    ::chmod(path.c_str(), 0666);

    /* Receive buffer size bump to avoid drops */
    int bufsize = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    /* Set 1s recv timeout so we can check g_running */
    struct timeval tv = { 1, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    fprintf(stderr, "[bridge_slave] UDS listening at %s\n", path.c_str());
    return fd;
}

/* --------------------------------------------------------------------- */
/* RDMA client with reconnect                                              */
/* --------------------------------------------------------------------- */

class RdmaClient {
public:
    RdmaClient(const std::string& ip, uint16_t port, uint32_t msg_size)
        : ip_(ip), port_(port), msg_size_(msg_size) {}

    ~RdmaClient() { disconnect(); }

    bool ensure_connected() {
        if (ep_ && rdma_endpoint_is_connected(ep_)) return true;
        disconnect();

        uint32_t backoff_s = 1;
        while (g_running.load()) {
            fprintf(stderr, "[bridge_slave] connecting RDMA to %s:%u...\n",
                    ip_.c_str(), port_);
            ep_ = rdma_endpoint_create_client(ip_.c_str(), port_, msg_size_);
            if (ep_) {
                fprintf(stderr, "[bridge_slave] RDMA connected\n");
                return true;
            }
            fprintf(stderr, "[bridge_slave] connect failed, retry in %us\n",
                    backoff_s);
            for (uint32_t i = 0; i < backoff_s && g_running.load(); i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            backoff_s = std::min(30u, backoff_s * 2);
        }
        return false;
    }

    int send(const void* buf, uint32_t len) {
        if (!ep_) return -ENOTCONN;
        int rc = rdma_endpoint_send(ep_, buf, len);
        if (rc < 0) {
            fprintf(stderr, "[bridge_slave] rdma send err: %s\n",
                    rdma_endpoint_last_error(ep_));
            disconnect();
        }
        return rc;
    }

private:
    void disconnect() {
        if (ep_) { rdma_endpoint_destroy(ep_); ep_ = nullptr; }
    }

    std::string     ip_;
    uint16_t        port_;
    uint32_t        msg_size_;
    rdma_endpoint_t* ep_ = nullptr;
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
        {"master-ip", required_argument, 0, 'M'},
        {"port",      required_argument, 0, 'p'},
        {"uds",       required_argument, 0, 'u'},
        {"msg-size",  required_argument, 0, 's'},
        {"help",      no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "M:p:u:s:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'M': cfg.master_ip = optarg; break;
        case 'p': cfg.port = (uint16_t)atoi(optarg); break;
        case 'u': cfg.uds_path = optarg; break;
        case 's': cfg.msg_size = (uint32_t)atoi(optarg); break;
        case 'h':
            fprintf(stderr, "Usage: %s --master-ip=IP [--port=N] [--uds=PATH] "
                    "[--msg-size=N]\n", argv[0]);
            return 0;
        }
    }
    if (cfg.master_ip.empty()) {
        fprintf(stderr, "--master-ip is required\n"); return 1;
    }

    /* Bring up UDS first so slave_agent can connect early */
    int uds_fd = uds_server_setup(cfg.uds_path);
    if (uds_fd < 0) return 1;

    RdmaClient rc(cfg.master_ip, cfg.port, cfg.msg_size);
    if (!rc.ensure_connected()) { ::close(uds_fd); ::unlink(cfg.uds_path.c_str()); return 1; }

    /* Receive + forward loop */
    std::vector<uint8_t> buf(cfg.msg_size);
    uint64_t sent = 0, drops = 0, errors = 0;

    auto last_log = std::chrono::steady_clock::now();

    while (g_running.load()) {
        ssize_t n = ::recv(uds_fd, buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            perror("recv"); break;
        }
        if (n == 0) continue;
        if ((size_t)n < sizeof(bridge_hdr_t)) {
            drops++; continue;
        }
        /* Validate header */
        bridge_hdr_t* h = (bridge_hdr_t*)buf.data();
        if (h->magic != BRIDGE_MAGIC) { drops++; continue; }

        if (!rc.ensure_connected()) break;
        int rv = rc.send(buf.data(), (uint32_t)n);
        if (rv == 0) {
            sent++;
        } else {
            errors++;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log).count() >= 10) {
            fprintf(stderr, "[bridge_slave] sent=%lu drops=%lu errors=%lu\n",
                    (unsigned long)sent, (unsigned long)drops, (unsigned long)errors);
            last_log = now;
        }
    }

    fprintf(stderr, "[bridge_slave] exiting (sent=%lu drops=%lu errors=%lu)\n",
            (unsigned long)sent, (unsigned long)drops, (unsigned long)errors);
    ::close(uds_fd);
    ::unlink(cfg.uds_path.c_str());
    return 0;
}
