/*
 * grpc_service.h — gRPC service implementations for the cluster master.
 *
 * ClusterControlServiceImpl handles the bidirectional NodeSession stream
 * and the unary DirectPush fallback.  MasterHealthServiceImpl responds
 * to watchdog pings.
 */

#pragma once

#include <mutex>
#include <grpcpp/grpcpp.h>
#include "cluster.grpc.pb.h"

#include "node_registry.h"

extern "C" {
#include "db.h"
}

/* ------------------------------------------------------------------ */
/* ClusterControl service                                              */
/* ------------------------------------------------------------------ */

class ClusterControlServiceImpl final : public cluster::ClusterControl::Service {
public:
    ClusterControlServiceImpl(NodeRegistry& registry, db_ctx_t* db)
        : registry_(registry), db_(db) {}

    /* Expose db mutex for watchdog thread (shares same PGconn) */
    std::mutex& dbMutex() { return db_mu_; }

    /*
     * Bidirectional stream: slave_agent registers, then sends heartbeats
     * and metric reports.  Master sends ACKs and commands.
     */
    grpc::Status NodeSession(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<cluster::MasterMessage,
                                  cluster::NodeMessage>* stream) override;

    /*
     * Unary RPC: host metric_push connects directly when BF2/Comch is down.
     */
    grpc::Status DirectPush(
        grpc::ServerContext* context,
        const cluster::DirectPushRequest* request,
        cluster::DirectPushResponse* response) override;

    // Thread-safe DB write: libpq PGconn is not thread-safe, so all
    // DB calls from gRPC handler threads must go through this wrapper.
    void dbInsertHostMetrics(const cluster::ResourceReport& r);
    void dbInsertBF2Metrics(const cluster::BF2MetricsReport& r);
    void dbInsertEvent(const char* node_uuid, const char* event_type, const char* detail);

private:
    NodeRegistry& registry_;
    db_ctx_t*     db_;
    std::mutex    db_mu_;   // protects all db_ calls (PGconn is not thread-safe)
};

/* ------------------------------------------------------------------ */
/* MasterHealth service                                                */
/* ------------------------------------------------------------------ */

class MasterHealthServiceImpl final : public cluster::MasterHealth::Service {
public:
    explicit MasterHealthServiceImpl(NodeRegistry& registry)
        : registry_(registry) {}

    grpc::Status Ping(
        grpc::ServerContext* context,
        const cluster::HealthPingRequest* request,
        cluster::HealthPingResponse* response) override;

private:
    NodeRegistry& registry_;
};
