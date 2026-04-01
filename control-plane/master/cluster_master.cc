/*
 * cluster_master.cc — Main entry point for the gRPC-based cluster master.
 *
 * Responsibilities:
 *   - Parse command-line arguments
 *   - Connect to TimescaleDB, initialise v2 schema
 *   - Start gRPC server (ClusterControl + MasterHealth)
 *   - Start HTTP JSON status endpoint
 *   - Run watchdog thread: state transitions, DB upserts, event logging
 *   - Graceful shutdown on SIGINT / SIGTERM
 *
 * Usage:
 *   cluster_master [--grpc-port PORT] [--http-port PORT] [--db-connstr CONNSTR]
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "grpc_service.h"
#include "http_status.h"
#include "node_registry.h"

extern "C" {
#include "db.h"
#include "../../common/node_state.h"
#include "../../common/timing.h"
}

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int /*sig*/)
{
    g_shutdown.store(true);
}

/* ------------------------------------------------------------------ */
/* Watchdog thread                                                     */
/* ------------------------------------------------------------------ */

static void watchdog_thread(NodeRegistry& registry, db_ctx_t* db)
{
    fprintf(stderr, "[watchdog] started (1s interval)\n");

    while (!g_shutdown.load()) {
        uint64_t now = unix_ns();

        /* Run state transitions and log changes */
        registry.runStateTransitions(now,
            [&](const std::string& uuid,
                node_state_t old_state,
                node_state_t new_state) {
                fprintf(stderr, "[watchdog] %s: %s -> %s\n",
                        uuid.c_str(),
                        node_state_str(old_state),
                        node_state_str(new_state));

                if (db) {
                    /* Log event */
                    std::string detail = std::string(node_state_str(old_state)) +
                                         " -> " + node_state_str(new_state);
                    db_insert_event(db, uuid.c_str(),
                                   "state_transition", detail.c_str());
                }
            });

        /* Update DB registry for all nodes */
        if (db) {
            auto nodes = registry.snapshot();
            for (const auto& e : nodes) {
                db_upsert_node_registry(db,
                    e.node_uuid.c_str(),
                    e.hostname.c_str(),
                    e.pci_bus_id.c_str(),
                    node_state_str(e.state),
                    domain_status_str(e.host_status),
                    domain_status_str(e.bf2_status));
            }
        }

        /* Sleep 1 second */
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    fprintf(stderr, "[watchdog] stopped\n");
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

struct MasterConfig {
    uint16_t    grpc_port  = 50051;
    uint16_t    http_port  = 8080;
    std::string db_connstr = "host=localhost dbname=cluster_metrics "
                             "user=cluster password=cluster";
};

static MasterConfig parse_args(int argc, char* argv[])
{
    MasterConfig cfg;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--grpc-port") == 0 && i + 1 < argc) {
            cfg.grpc_port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--http-port") == 0 && i + 1 < argc) {
            cfg.http_port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--db-connstr") == 0 && i + 1 < argc) {
            cfg.db_connstr = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: %s [OPTIONS]\n"
                "  --grpc-port PORT     gRPC listen port (default: 50051)\n"
                "  --http-port PORT     HTTP status port (default: 8080)\n"
                "  --db-connstr CONNSTR PostgreSQL connection string\n",
                argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s (use --help)\n", argv[i]);
            exit(1);
        }
    }

    return cfg;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char* argv[])
{
    MasterConfig cfg = parse_args(argc, argv);

    /* ---- Signal handling ---- */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- Database ---- */
    db_ctx_t* db = db_connect(cfg.db_connstr.c_str());
    if (!db) {
        fprintf(stderr, "[master] WARNING: database connection failed, "
                "running without persistence\n");
    } else {
        if (db_init_schema_v2(db) < 0) {
            fprintf(stderr, "[master] WARNING: schema v2 init failed\n");
        }
    }

    /* ---- Node registry ---- */
    NodeRegistry registry;

    /* ---- gRPC server ---- */
    ClusterControlServiceImpl control_service(registry, db);
    MasterHealthServiceImpl   health_service(registry);

    std::string listen_addr = "0.0.0.0:" + std::to_string(cfg.grpc_port);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(listen_addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&control_service);
    builder.RegisterService(&health_service);

    /* Keepalive configuration */
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS,       10000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS,    5000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    builder.AddChannelArgument(
        GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);

    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        fprintf(stderr, "[master] FATAL: failed to start gRPC server on %s\n",
                listen_addr.c_str());
        if (db) db_disconnect(db);
        return 1;
    }

    fprintf(stderr, "[master] gRPC server listening on %s\n",
            listen_addr.c_str());

    /* ---- HTTP status server ---- */
    if (http_status_start(cfg.http_port, registry) < 0) {
        fprintf(stderr, "[master] WARNING: HTTP status server failed to start\n");
    }

    /* ---- Watchdog thread ---- */
    std::thread wd(watchdog_thread, std::ref(registry), db);

    /* ---- Log startup event ---- */
    if (db) {
        db_insert_event(db, nullptr, "master_start",
                        ("grpc=" + std::to_string(cfg.grpc_port) +
                         " http=" + std::to_string(cfg.http_port)).c_str());
    }

    fprintf(stderr, "[master] cluster_master running "
            "(grpc=%u, http=%u, db=%s)\n",
            (unsigned)cfg.grpc_port,
            (unsigned)cfg.http_port,
            db ? "connected" : "none");

    /* ---- Wait for shutdown ---- */
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    fprintf(stderr, "[master] shutting down...\n");

    /* ---- Cleanup ---- */
    server->Shutdown();
    http_status_stop();

    g_shutdown.store(true);  /* ensure watchdog exits */
    if (wd.joinable()) wd.join();

    if (db) {
        db_insert_event(db, nullptr, "master_stop", "graceful shutdown");
        db_disconnect(db);
    }

    fprintf(stderr, "[master] shutdown complete\n");
    return 0;
}
