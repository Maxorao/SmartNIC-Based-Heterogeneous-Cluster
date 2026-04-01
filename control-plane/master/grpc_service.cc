/*
 * grpc_service.cc — gRPC service implementations for the cluster master.
 *
 * NodeSession protocol:
 *   1. Client sends RegisterRequest as the first message.
 *   2. Master validates, registers node, sends RegisterAck.
 *   3. Loop: read NodeMessage, dispatch by payload case.
 *   4. On stream close (client disconnect), mark node suspect.
 *
 * DirectPush: unary insertion of host metrics (degradation fallback).
 */

#include "grpc_service.h"

#include <cinttypes>
#include <cstdio>
#include <string>

extern "C" {
#include "../../common/timing.h"
#include "../../common/node_state.h"
}

/* ------------------------------------------------------------------ */
/* Helper: map proto DomainStatus to C enum                            */
/* ------------------------------------------------------------------ */

static domain_status_t proto_to_domain(cluster::DomainStatus ds)
{
    switch (ds) {
    case cluster::DOMAIN_OK:          return DOMAIN_OK;
    case cluster::DOMAIN_DEGRADED:    return DOMAIN_DEGRADED;
    case cluster::DOMAIN_UNREACHABLE: return DOMAIN_UNREACHABLE;
    default:                          return DOMAIN_UNKNOWN;
    }
}

/* ================================================================== */
/* ClusterControlServiceImpl                                           */
/* ================================================================== */

grpc::Status ClusterControlServiceImpl::NodeSession(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<cluster::MasterMessage,
                              cluster::NodeMessage>* stream)
{
    std::string node_uuid;

    /* ---- Phase 1: registration ---- */
    {
        cluster::NodeMessage first_msg;
        if (!stream->Read(&first_msg)) {
            return grpc::Status(grpc::CANCELLED, "stream closed before register");
        }

        if (!first_msg.has_register_req()) {
            return grpc::Status(grpc::INVALID_ARGUMENT,
                                "first message must be RegisterRequest");
        }

        const auto& reg = first_msg.register_req();
        node_uuid = reg.node_uuid();

        uint64_t now = unix_ns();

        registry_.registerNode(
            reg.node_uuid(),
            reg.hostname(),
            reg.host_hostname(),
            reg.ip_addr(),
            reg.pci_bus_id(),
            now);

        /* Persist to DB (thread-safe) */
        {
            std::lock_guard<std::mutex> lk(db_mu_);
            if (db_) {
                db_upsert_node_registry(db_, reg.node_uuid().c_str(),
                                        reg.hostname().c_str(),
                                        reg.pci_bus_id().c_str(),
                                        "online", "ok", "ok");
                db_insert_event(db_, reg.node_uuid().c_str(),
                                "register", reg.hostname().c_str());
            }
        }

        fprintf(stderr, "[grpc] node registered: %s (%s) from %s\n",
                reg.node_uuid().c_str(),
                reg.hostname().c_str(),
                reg.ip_addr().c_str());

        /* Send RegisterAck */
        cluster::MasterMessage ack_msg;
        auto* ack = ack_msg.mutable_register_ack();
        ack->set_accepted(true);
        ack->set_assigned_id(node_uuid);
        ack->set_heartbeat_interval_ms(3000);
        ack->set_report_interval_ms(5000);

        if (!stream->Write(ack_msg)) {
            return grpc::Status(grpc::INTERNAL, "failed to send RegisterAck");
        }
    }

    /* ---- Phase 2: message loop ---- */
    cluster::NodeMessage msg;
    while (stream->Read(&msg)) {
        switch (msg.payload_case()) {

        case cluster::NodeMessage::kHeartbeat: {
            const auto& hb = msg.heartbeat();
            uint64_t now = unix_ns();

            domain_status_t host_st = proto_to_domain(hb.host_status());
            domain_status_t bf2_st  = proto_to_domain(hb.bf2_status());

            registry_.updateHeartbeat(node_uuid, host_st, bf2_st, now);

            /* Send HeartbeatAck */
            cluster::MasterMessage reply;
            auto* ha = reply.mutable_heartbeat_ack();
            ha->set_seq(hb.seq());
            ha->set_master_time_ns(now);
            stream->Write(reply);
            break;
        }

        case cluster::NodeMessage::kResourceReport: {
            const auto& rr = msg.resource_report();

            {
                std::lock_guard<std::mutex> lk(db_mu_);
                if (db_) {
                    db_insert_host_metrics(db_,
                        rr.node_uuid().c_str(),
                        rr.timestamp_ns(),
                        rr.cpu_usage_pct(),
                        rr.mem_total_kb(),
                        rr.mem_avail_kb(),
                        rr.net_rx_bytes(),
                        rr.net_tx_bytes());
                }
            }

            /* Send ReportAck */
            cluster::MasterMessage reply;
            auto* ra = reply.mutable_report_ack();
            ra->set_seq(rr.timestamp_ns());  /* use timestamp as seq */
            stream->Write(reply);
            break;
        }

        case cluster::NodeMessage::kBf2Report: {
            const auto& br = msg.bf2_report();

            {
                std::lock_guard<std::mutex> lk(db_mu_);
                if (db_) {
                    db_insert_bf2_metrics(db_,
                        br.node_uuid().c_str(),
                        br.timestamp_ns(),
                        br.arm_cpu_pct(),
                        br.arm_mem_total_kb(),
                        br.arm_mem_avail_kb(),
                        br.temperature_c(),
                        br.port_rx_bytes(),
                        br.port_tx_bytes(),
                        br.port_rx_drops(),
                        br.ovs_flow_count());
                }
            }
            break;
        }

        case cluster::NodeMessage::kStatusChange: {
            const auto& sc = msg.status_change();

            fprintf(stderr, "[grpc] status change from %s: domain=%s %s->%s reason=%s\n",
                    node_uuid.c_str(),
                    sc.domain().c_str(),
                    cluster::DomainStatus_Name(sc.old_status()).c_str(),
                    cluster::DomainStatus_Name(sc.new_status()).c_str(),
                    sc.reason().c_str());

            {
                std::string detail = sc.domain() + ": " +
                    cluster::DomainStatus_Name(sc.old_status()) + " -> " +
                    cluster::DomainStatus_Name(sc.new_status()) +
                    " (" + sc.reason() + ")";
                std::lock_guard<std::mutex> lk(db_mu_);
                if (db_) {
                    db_insert_event(db_, node_uuid.c_str(),
                                    "status_change", detail.c_str());
                }
            }
            break;
        }

        case cluster::NodeMessage::kDeregister: {
            const auto& dr = msg.deregister();

            fprintf(stderr, "[grpc] node deregistered: %s reason=%s\n",
                    dr.node_uuid().c_str(), dr.reason().c_str());

            {
                std::lock_guard<std::mutex> lk(db_mu_);
                if (db_) {
                    db_insert_event(db_, node_uuid.c_str(),
                                    "deregister", dr.reason().c_str());
                    db_upsert_node_registry(db_, node_uuid.c_str(),
                                            "", "", "offline", "unknown", "unknown");
                }
            }

            registry_.removeNode(node_uuid);
            return grpc::Status::OK;
        }

        case cluster::NodeMessage::kCmdResult: {
            const auto& cr = msg.cmd_result();
            fprintf(stderr, "[grpc] command result from %s: cmd=%s exit=%d\n",
                    node_uuid.c_str(),
                    cr.command_id().c_str(),
                    cr.exit_code());
            break;
        }

        default:
            fprintf(stderr, "[grpc] unknown payload case %d from %s\n",
                    static_cast<int>(msg.payload_case()),
                    node_uuid.c_str());
            break;
        }
    }

    /* Stream closed — mark node as suspect (watchdog will transition to
     * OFFLINE if heartbeat timeout expires). */
    fprintf(stderr, "[grpc] stream closed for node %s, marking suspect\n",
            node_uuid.c_str());

    {
        std::lock_guard<std::mutex> lk(db_mu_);
        if (db_) {
            db_insert_event(db_, node_uuid.c_str(),
                            "stream_closed", "bidirectional stream ended");
        }
    }

    /* Don't remove from registry — let the watchdog handle the transition
     * from SUSPECT to OFFLINE based on heartbeat timestamps. */

    return grpc::Status::OK;
}

/* ================================================================== */
/* DirectPush (host metric_push fallback)                              */
/* ================================================================== */

grpc::Status ClusterControlServiceImpl::DirectPush(
    grpc::ServerContext* /*context*/,
    const cluster::DirectPushRequest* request,
    cluster::DirectPushResponse* response)
{
    fprintf(stderr, "[grpc] DirectPush received from %s (cpu=%.1f%%)\n",
            request->node_id().c_str(), request->cpu_usage_pct());

    if (db_) {
        std::lock_guard<std::mutex> lk(db_mu_);
        int rc = db_insert_host_metrics(db_,
            request->node_id().c_str(),
            request->timestamp_ns(),
            request->cpu_usage_pct(),
            request->mem_total_kb(),
            request->mem_avail_kb(),
            request->net_rx_bytes(),
            request->net_tx_bytes());

        response->set_accepted(rc == 0);

        if (rc != 0) {
            fprintf(stderr, "[grpc] DirectPush: DB insert failed for %s\n",
                    request->node_id().c_str());
        }
    } else {
        response->set_accepted(false);
    }

    return grpc::Status::OK;
}

/* ================================================================== */
/* MasterHealth (watchdog health check)                                */
/* ================================================================== */

grpc::Status MasterHealthServiceImpl::Ping(
    grpc::ServerContext* /*context*/,
    const cluster::HealthPingRequest* /*request*/,
    cluster::HealthPingResponse* response)
{
    uint64_t now = unix_ns();
    auto summary = registry_.getSummary();

    response->set_timestamp_ns(now);
    response->set_healthy(true);
    response->set_node_count(summary.total);

    return grpc::Status::OK;
}

/* ================================================================== */
/* Thread-safe DB wrappers                                             */
/* ================================================================== */

void ClusterControlServiceImpl::dbInsertHostMetrics(const cluster::ResourceReport& r)
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (db_) {
        db_insert_host_metrics(db_, r.node_uuid().c_str(), r.timestamp_ns(),
                               r.cpu_usage_pct(), r.mem_total_kb(), r.mem_avail_kb(),
                               r.net_rx_bytes(), r.net_tx_bytes());
    }
}

void ClusterControlServiceImpl::dbInsertBF2Metrics(const cluster::BF2MetricsReport& r)
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (db_) {
        db_insert_bf2_metrics(db_, r.node_uuid().c_str(), r.timestamp_ns(),
                              r.arm_cpu_pct(), r.arm_mem_total_kb(), r.arm_mem_avail_kb(),
                              r.temperature_c(), r.port_rx_bytes(), r.port_tx_bytes(),
                              r.port_rx_drops(), r.ovs_flow_count());
    }
}

void ClusterControlServiceImpl::dbInsertEvent(const char* node_uuid,
                                               const char* event_type,
                                               const char* detail)
{
    std::lock_guard<std::mutex> lk(db_mu_);
    if (db_) {
        db_insert_event(db_, node_uuid, event_type, detail);
    }
}
