/*
 * orchestrator_agent.cc — Local migration executor for BF2.
 *
 * Listens on a gRPC port and executes Docker/ip/perf commands locally,
 * replacing the two-hop SSH path used by orchestrator.py. This removes
 * the 3–5 second SSH connect overhead per migration step, dropping total
 * migration latency from ~8 s (SSH-based) to sub-second.
 *
 * Runs as root on each BF2 (needs ip / docker / perf privileges).
 * Listens on all interfaces by default (restricts via firewall if needed).
 *
 * Usage:
 *   orchestrator_agent [--port=50052] [--bind=0.0.0.0]
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <getopt.h>
#include <sys/wait.h>
#include <time.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include "orchestrator_agent.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using namespace orchestrator_agent;

static std::atomic<bool> g_shutdown{false};

/* --------------------------------------------------------------------- */
/* Helper: execute a shell command, return (rc, stdout+stderr, ms)         */
/* --------------------------------------------------------------------- */

struct CmdResult {
    int         rc;
    std::string output;
    uint64_t    duration_ms;
};

static CmdResult run_cmd(const std::string& cmd, int timeout_sec = 60)
{
    auto t0 = std::chrono::steady_clock::now();
    CmdResult r = {-1, "", 0};

    /* Limit with timeout */
    std::string full = "timeout " + std::to_string(timeout_sec) + " " + cmd + " 2>&1";
    FILE* f = popen(full.c_str(), "r");
    if (!f) { r.output = "popen failed"; return r; }
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        r.output += buf;
        if (r.output.size() > 65536) {
            r.output.resize(65536);
            break;
        }
    }
    int status = pclose(f);
    if (WIFEXITED(status)) {
        r.rc = WEXITSTATUS(status);
    } else {
        r.rc = -1;
    }
    auto t1 = std::chrono::steady_clock::now();
    r.duration_ms = (uint64_t)std::chrono::duration_cast<
        std::chrono::milliseconds>(t1 - t0).count();
    return r;
}

/* --------------------------------------------------------------------- */
/* Service implementation                                                  */
/* --------------------------------------------------------------------- */

class AgentImpl final : public OrchestratorAgent::Service {
public:
    Status StartContainer(ServerContext* /*ctx*/,
                           const StartContainerRequest* req,
                           ContainerResponse* resp) override
    {
        /* docker rm any existing container with same name first */
        run_cmd("docker rm -f " + req->name() + " 2>/dev/null", 10);

        /* Assemble docker run */
        std::string cmd = "docker run -d --name " + req->name();
        std::string net = req->network().empty() ? "host" : req->network();
        cmd += " --network=" + net;
        if (!req->cpuset().empty()) {
            cmd += " --cpuset-cpus=" + req->cpuset();
        }
        for (const auto& arg : req->extra_args()) {
            cmd += " " + arg;
        }
        cmd += " " + req->image();

        CmdResult r = run_cmd(cmd, 60);
        resp->set_success(r.rc == 0);
        resp->set_duration_ms(r.duration_ms);
        if (r.rc == 0) {
            /* Extract container ID (first word of output) */
            std::string id = r.output.substr(0, r.output.find('\n'));
            resp->set_container_id(id);
        } else {
            resp->set_error(r.output);
        }
        return Status::OK;
    }

    Status StopContainer(ServerContext*, const StopContainerRequest* req,
                          ContainerResponse* resp) override
    {
        std::string cmd = "docker rm";
        if (req->force()) cmd += " -f";
        cmd += " " + req->name();
        CmdResult r = run_cmd(cmd, 30);
        resp->set_success(r.rc == 0);
        resp->set_duration_ms(r.duration_ms);
        if (r.rc != 0) resp->set_error(r.output);
        return Status::OK;
    }

    Status HealthCheck(ServerContext*, const HealthCheckRequest* req,
                        HealthCheckResponse* resp) override
    {
        uint32_t max = req->max_attempts() ? req->max_attempts() : 30;
        uint32_t interval_ms = req->interval_ms() ? req->interval_ms() : 1000;

        auto t0 = std::chrono::steady_clock::now();
        for (uint32_t attempt = 1; attempt <= max; attempt++) {
            std::string cmd = "curl -sf -m 2 -o /dev/null " + req->target_url();
            CmdResult r = run_cmd(cmd, 3);
            if (r.rc == 0) {
                auto t1 = std::chrono::steady_clock::now();
                resp->set_healthy(true);
                resp->set_attempts(attempt);
                resp->set_duration_ms((uint64_t)std::chrono::
                    duration_cast<std::chrono::milliseconds>(t1 - t0).count());
                return Status::OK;
            }
            usleep(interval_ms * 1000);
        }
        auto t1 = std::chrono::steady_clock::now();
        resp->set_healthy(false);
        resp->set_attempts(max);
        resp->set_duration_ms((uint64_t)std::chrono::
            duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        resp->set_error("max attempts exceeded");
        return Status::OK;
    }

    Status SwitchVip(ServerContext*, const SwitchVipRequest* req,
                      SwitchVipResponse* resp) override
    {
        uint32_t prefix = req->prefix_len() ? req->prefix_len() : 24;
        std::string action = req->action();
        if (action != "add" && action != "del") {
            resp->set_success(false);
            resp->set_error("action must be 'add' or 'del'");
            return Status::OK;
        }
        std::string cmd = "ip addr " + action + " " + req->vip() + "/" +
                           std::to_string(prefix) + " dev " + req->interface() +
                           " 2>&1 || true";
        auto t0 = std::chrono::steady_clock::now();
        CmdResult r = run_cmd(cmd, 5);

        if (action == "add" && req->send_arp()) {
            std::string arp_cmd = "arping -c 3 -A -I " + req->interface() +
                                    " " + req->vip() + " &>/dev/null";
            run_cmd(arp_cmd, 5);
        }
        auto t1 = std::chrono::steady_clock::now();
        resp->set_success(r.rc == 0);
        resp->set_duration_ms((uint64_t)std::chrono::
            duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        if (r.rc != 0) resp->set_error(r.output);
        return Status::OK;
    }

    Status RunPerf(ServerContext*, const RunPerfRequest* req,
                    RunPerfResponse* resp) override
    {
        std::string events = req->events().empty()
                              ? "LLC-load-misses,LLC-loads" : req->events();
        double dur = req->duration_s() > 0 ? req->duration_s() : 2.0;
        std::string target = req->target_pid().empty()
                              ? "-a" : ("-p " + req->target_pid());
        std::string cmd = "perf stat -e " + events + " " + target +
                           " sleep " + std::to_string(dur);
        CmdResult r = run_cmd(cmd, (int)dur + 10);
        resp->set_success(r.rc == 0);
        resp->set_raw_output(r.output);

        /* Parse "<count> <event_name>" lines */
        std::istringstream iss(r.output);
        std::string line;
        while (std::getline(iss, line)) {
            /* Strip commas in counts */
            std::string clean;
            for (char c : line) if (c != ',') clean += c;
            std::istringstream ls(clean);
            uint64_t n = 0;
            std::string ev;
            if (ls >> n >> ev) {
                (*resp->mutable_counters())[ev] = n;
            }
        }
        return Status::OK;
    }

    Status Ping(ServerContext*, const PingRequest*,
                 PingResponse* resp) override
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        resp->set_server_timestamp_ns((uint64_t)ts.tv_sec * 1000000000ULL
                                       + ts.tv_nsec);
        char hn[128];
        gethostname(hn, sizeof(hn));
        resp->set_hostname(hn);
        resp->set_version("1.0.0");
        return Status::OK;
    }
};

/* --------------------------------------------------------------------- */
/* Main                                                                    */
/* --------------------------------------------------------------------- */

static void signal_handler(int) { g_shutdown.store(true); }

int main(int argc, char** argv)
{
    int port = 50052;
    std::string bind_ip = "0.0.0.0";

    static struct option longopts[] = {
        {"port",  required_argument, 0, 'p'},
        {"bind",  required_argument, 0, 'b'},
        {"help",  no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "p:b:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); break;
        case 'b': bind_ip = optarg; break;
        case 'h':
            fprintf(stderr, "Usage: %s [--port=N] [--bind=IP]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    AgentImpl service;
    ServerBuilder builder;
    std::string addr = bind_ip + ":" + std::to_string(port);
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    if (!server) {
        fprintf(stderr, "Failed to start gRPC server on %s\n", addr.c_str());
        return 1;
    }
    fprintf(stderr, "[orch_agent] listening on %s\n", addr.c_str());

    /* Poll for shutdown */
    while (!g_shutdown.load()) sleep(1);
    server->Shutdown();
    server->Wait();
    return 0;
}
