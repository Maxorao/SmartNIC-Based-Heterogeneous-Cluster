/*
 * db.h — TimescaleDB interface for the cluster master monitor.
 *
 * Uses libpq (PostgreSQL C client) to write resource metrics and node
 * status into a TimescaleDB hypertable.
 *
 * Schema is created automatically by db_init_schema().
 *
 * v2: Extended schema for gRPC-based cluster_master with separate host
 *     and BF2 metrics tables, node registry, and event log.
 */

#pragma once

#include <stdint.h>
#include "../../common/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque database context */
typedef struct db_ctx db_ctx_t;

/*
 * Connect to the database.
 * @connstr: libpq connection string, e.g.
 *   "host=localhost dbname=cluster_metrics user=cluster password=cluster"
 * Returns NULL on failure (error printed to stderr).
 */
db_ctx_t *db_connect(const char *connstr);

/*
 * Disconnect and free all resources.
 */
void db_disconnect(db_ctx_t *ctx);

/*
 * Create tables if they do not exist (v1 schema).
 * Safe to call on every startup.
 * Returns 0 on success, -1 on failure.
 */
int db_init_schema(db_ctx_t *ctx);

/*
 * Insert one resource report into the node_metrics hypertable.
 * Returns 0 on success, -1 on failure (will attempt reconnect once).
 */
int db_insert_resource(db_ctx_t *ctx, const resource_report_t *r);

/*
 * Upsert node online/offline status.
 * Returns 0 on success, -1 on failure.
 */
int db_update_node_status(db_ctx_t *ctx, const char *node_id, int online);

/*
 * Upsert node registration info.
 * Returns 0 on success, -1 on failure.
 */
int db_register_node(db_ctx_t *ctx, const register_payload_t *reg,
                     const char *ip_str);

/* ------------------------------------------------------------------ */
/* Extended schema v2 — used by gRPC-based cluster_master             */
/* ------------------------------------------------------------------ */

/*
 * Create v2 tables: node_registry, host_metrics, bf2_metrics,
 * cluster_events (all with TimescaleDB hypertables + compression).
 * Safe to call on every startup (IF NOT EXISTS).
 * Returns 0 on success, -1 on failure.
 */
int db_init_schema_v2(db_ctx_t *ctx);

/*
 * Insert host-side resource metrics.
 * Returns 0 on success, -1 on failure (auto-reconnect once).
 */
int db_insert_host_metrics(db_ctx_t *ctx, const char *node_uuid, uint64_t ts_ns,
                           float cpu_pct, uint64_t mem_total_kb, uint64_t mem_avail_kb,
                           uint64_t net_rx, uint64_t net_tx);

/*
 * Insert BF2-side metrics (ARM CPU, temperature, OVS flows, etc.).
 * Returns 0 on success, -1 on failure (auto-reconnect once).
 */
int db_insert_bf2_metrics(db_ctx_t *ctx, const char *node_uuid, uint64_t ts_ns,
                          float arm_cpu_pct, uint64_t arm_mem_total_kb, uint64_t arm_mem_avail_kb,
                          float temp_c, uint64_t port_rx, uint64_t port_tx,
                          uint64_t port_drops, uint32_t ovs_flows);

/*
 * Insert a cluster event (state transition, registration, error, etc.).
 * Returns 0 on success, -1 on failure.
 */
int db_insert_event(db_ctx_t *ctx, const char *node_uuid, const char *event_type,
                    const char *detail);

/*
 * Upsert node registry entry with current state and domain status.
 * Returns 0 on success, -1 on failure.
 */
int db_upsert_node_registry(db_ctx_t *ctx, const char *node_uuid, const char *hostname,
                            const char *pci_bus_id, const char *state,
                            const char *host_status, const char *bf2_status);

#ifdef __cplusplus
}
#endif
