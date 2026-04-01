/*
 * db.cc — TimescaleDB interface implementation via libpq (C++ build).
 *
 * This file is a superset of db.c: it contains the original v1
 * implementation plus extended v2 functions for the gRPC-based
 * cluster_master (separate host/BF2 metrics tables, node registry,
 * and event log).
 *
 * Compiled as C++ but all public functions have C linkage (extern "C"
 * via db.h) so they can be called from both C and C++ code.
 *
 * DDL created by db_init_schema_v2:
 *   node_registry   — Current state of each node (upsert on heartbeat)
 *   host_metrics    — TimescaleDB hypertable for x86 host metrics
 *   bf2_metrics     — TimescaleDB hypertable for BF2 ARM metrics
 *   cluster_events  — TimescaleDB hypertable for state-change events
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#include <libpq-fe.h>

#include "db.h"
#include "../../common/protocol.h"

/* ------------------------------------------------------------------ */
/* Internal structure                                                   */
/* ------------------------------------------------------------------ */

struct db_ctx {
    PGconn *conn;
    char   *connstr;   /* saved for reconnect */
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static void log_pq_error(const char *op, PGconn *conn)
{
    fprintf(stderr, "[db] %s failed: %s\n", op, PQerrorMessage(conn));
}

static int ensure_connected(db_ctx_t *ctx)
{
    if (PQstatus(ctx->conn) == CONNECTION_OK) return 0;

    fprintf(stderr, "[db] connection lost, reconnecting...\n");
    PQfinish(ctx->conn);
    ctx->conn = PQconnectdb(ctx->connstr);
    if (PQstatus(ctx->conn) != CONNECTION_OK) {
        fprintf(stderr, "[db] reconnect failed: %s\n",
                PQerrorMessage(ctx->conn));
        return -1;
    }
    fprintf(stderr, "[db] reconnected\n");
    return 0;
}

static int exec_simple(db_ctx_t *ctx, const char *sql)
{
    PGresult *res = PQexec(ctx->conn, sql);
    ExecStatusType status = PQresultStatus(res);
    int ok = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);
    if (!ok) fprintf(stderr, "[db] exec_simple error: %s\n%s\n",
                     PQerrorMessage(ctx->conn), sql);
    PQclear(res);
    return ok ? 0 : -1;
}

/* Convert nanosecond unix timestamp to ISO-8601 string for PostgreSQL */
static void ns_to_iso8601(uint64_t ts_ns, char *buf, size_t buf_size)
{
    time_t sec = (time_t)(ts_ns / 1000000000ULL);
    long   ns  = (long)(ts_ns % 1000000000ULL);
    struct tm tm_utc;
    gmtime_r(&sec, &tm_utc);
    snprintf(buf, buf_size,
             "%04d-%02d-%02dT%02d:%02d:%02d.%09ldZ",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, ns);
}

/* ------------------------------------------------------------------ */
/* Public API — v1 (original from db.c)                                */
/* ------------------------------------------------------------------ */

db_ctx_t *db_connect(const char *connstr)
{
    db_ctx_t *ctx = (db_ctx_t *)calloc(1, sizeof(db_ctx_t));
    if (!ctx) { perror("calloc"); return NULL; }

    ctx->connstr = strdup(connstr);
    ctx->conn    = PQconnectdb(connstr);

    if (PQstatus(ctx->conn) != CONNECTION_OK) {
        fprintf(stderr, "[db] connect failed: %s\n",
                PQerrorMessage(ctx->conn));
        PQfinish(ctx->conn);
        free(ctx->connstr);
        free(ctx);
        return NULL;
    }

    fprintf(stderr, "[db] connected to: %s\n", connstr);
    return ctx;
}

void db_disconnect(db_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->conn)    PQfinish(ctx->conn);
    if (ctx->connstr) free(ctx->connstr);
    free(ctx);
}

int db_init_schema(db_ctx_t *ctx)
{
    if (ensure_connected(ctx) < 0) return -1;

    /* node_metrics hypertable */
    const char *create_metrics =
        "CREATE TABLE IF NOT EXISTS node_metrics ("
        "    time         TIMESTAMPTZ NOT NULL,"
        "    node_id      TEXT NOT NULL,"
        "    cpu_pct      FLOAT4,"
        "    mem_used_kb  BIGINT,"
        "    mem_total_kb BIGINT,"
        "    net_rx_bps   BIGINT,"
        "    net_tx_bps   BIGINT"
        ");";

    const char *create_hypertable =
        "SELECT create_hypertable('node_metrics', 'time',"
        "    if_not_exists => TRUE);";

    /* node_status table */
    const char *create_status =
        "CREATE TABLE IF NOT EXISTS node_status ("
        "    node_id       TEXT PRIMARY KEY,"
        "    ip_addr       TEXT,"
        "    online        BOOLEAN DEFAULT TRUE,"
        "    last_seen     TIMESTAMPTZ,"
        "    registered_at TIMESTAMPTZ DEFAULT NOW()"
        ");";

    if (exec_simple(ctx, create_metrics)    < 0) return -1;
    if (exec_simple(ctx, create_hypertable) < 0) return -1;  /* may warn if exists */
    if (exec_simple(ctx, create_status)     < 0) return -1;

    fprintf(stderr, "[db] schema v1 initialised\n");
    return 0;
}

int db_insert_resource(db_ctx_t *ctx, const resource_report_t *r)
{
    if (ensure_connected(ctx) < 0) return -1;

    char ts_buf[64];
    ns_to_iso8601(r->timestamp_ns, ts_buf, sizeof(ts_buf));

    uint64_t mem_used_kb = r->mem_total_kb > r->mem_avail_kb
                           ? r->mem_total_kb - r->mem_avail_kb : 0;

    /* Build string representations of numeric values */
    char cpu_str[32], used_str[32], total_str[32], rx_str[32], tx_str[32];
    snprintf(cpu_str,   sizeof(cpu_str),   "%.4f", (double)r->cpu_usage_pct);
    snprintf(used_str,  sizeof(used_str),  "%" PRIu64, mem_used_kb);
    snprintf(total_str, sizeof(total_str), "%" PRIu64, r->mem_total_kb);
    snprintf(rx_str,    sizeof(rx_str),    "%" PRIu64, r->net_rx_bytes);
    snprintf(tx_str,    sizeof(tx_str),    "%" PRIu64, r->net_tx_bytes);

    const char *params[7] = {
        ts_buf,
        r->node_id,
        cpu_str,
        used_str,
        total_str,
        rx_str,
        tx_str,
    };

    const char *sql =
        "INSERT INTO node_metrics "
        "(time, node_id, cpu_pct, mem_used_kb, mem_total_kb, net_rx_bps, net_tx_bps)"
        " VALUES ($1, $2, $3, $4, $5, $6, $7)";

    PGresult *res = PQexecParams(ctx->conn, sql, 7, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        log_pq_error("db_insert_resource", ctx->conn);
        PQclear(res);
        /* Try reconnect once */
        if (ensure_connected(ctx) == 0) {
            res = PQexecParams(ctx->conn, sql, 7, NULL, params, NULL, NULL, 0);
            ok  = (PQresultStatus(res) == PGRES_COMMAND_OK);
            if (!ok) log_pq_error("db_insert_resource (retry)", ctx->conn);
        }
    }
    PQclear(res);
    return ok ? 0 : -1;
}

int db_update_node_status(db_ctx_t *ctx, const char *node_id, int online)
{
    if (ensure_connected(ctx) < 0) return -1;

    const char *online_str = online ? "true" : "false";
    const char *params[2]  = { node_id, online_str };

    const char *sql =
        "INSERT INTO node_status (node_id, online, last_seen)"
        " VALUES ($1, $2, NOW())"
        " ON CONFLICT (node_id) DO UPDATE"
        "   SET online = EXCLUDED.online, last_seen = NOW()";

    PGresult *res = PQexecParams(ctx->conn, sql, 2, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) log_pq_error("db_update_node_status", ctx->conn);
    PQclear(res);
    return ok ? 0 : -1;
}

int db_register_node(db_ctx_t *ctx, const register_payload_t *reg,
                     const char *ip_str)
{
    if (ensure_connected(ctx) < 0) return -1;

    const char *params[2] = { reg->node_id, ip_str ? ip_str : "" };

    const char *sql =
        "INSERT INTO node_status (node_id, ip_addr, online, last_seen, registered_at)"
        " VALUES ($1, $2, TRUE, NOW(), NOW())"
        " ON CONFLICT (node_id) DO UPDATE"
        "   SET ip_addr = EXCLUDED.ip_addr, online = TRUE,"
        "       last_seen = NOW()";

    PGresult *res = PQexecParams(ctx->conn, sql, 2, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) log_pq_error("db_register_node", ctx->conn);
    PQclear(res);
    return ok ? 0 : -1;
}

/* ================================================================== */
/* Public API — v2 (extended schema for gRPC cluster_master)           */
/* ================================================================== */

int db_init_schema_v2(db_ctx_t *ctx)
{
    if (ensure_connected(ctx) < 0) return -1;

    /* Also init v1 schema for backward compatibility */
    if (db_init_schema(ctx) < 0) return -1;

    /* ---- node_registry ---- */
    const char *create_registry =
        "CREATE TABLE IF NOT EXISTS node_registry ("
        "    node_uuid     TEXT PRIMARY KEY,"
        "    hostname      TEXT,"
        "    pci_bus_id    TEXT,"
        "    state         TEXT DEFAULT 'unknown',"
        "    host_status   TEXT DEFAULT 'unknown',"
        "    bf2_status    TEXT DEFAULT 'unknown',"
        "    last_seen     TIMESTAMPTZ DEFAULT NOW(),"
        "    registered_at TIMESTAMPTZ DEFAULT NOW()"
        ");";

    /* ---- host_metrics hypertable ---- */
    const char *create_host_metrics =
        "CREATE TABLE IF NOT EXISTS host_metrics ("
        "    time          TIMESTAMPTZ NOT NULL,"
        "    node_uuid     TEXT NOT NULL,"
        "    cpu_pct       FLOAT4,"
        "    mem_total_kb  BIGINT,"
        "    mem_avail_kb  BIGINT,"
        "    net_rx_bytes  BIGINT,"
        "    net_tx_bytes  BIGINT"
        ");";

    const char *ht_host =
        "SELECT create_hypertable('host_metrics', 'time',"
        "    if_not_exists => TRUE);";

    /* ---- bf2_metrics hypertable ---- */
    const char *create_bf2_metrics =
        "CREATE TABLE IF NOT EXISTS bf2_metrics ("
        "    time              TIMESTAMPTZ NOT NULL,"
        "    node_uuid         TEXT NOT NULL,"
        "    arm_cpu_pct       FLOAT4,"
        "    arm_mem_total_kb  BIGINT,"
        "    arm_mem_avail_kb  BIGINT,"
        "    temperature_c     FLOAT4,"
        "    port_rx_bytes     BIGINT,"
        "    port_tx_bytes     BIGINT,"
        "    port_rx_drops     BIGINT,"
        "    ovs_flow_count    INTEGER"
        ");";

    const char *ht_bf2 =
        "SELECT create_hypertable('bf2_metrics', 'time',"
        "    if_not_exists => TRUE);";

    /* ---- cluster_events hypertable ---- */
    const char *create_events =
        "CREATE TABLE IF NOT EXISTS cluster_events ("
        "    time         TIMESTAMPTZ NOT NULL DEFAULT NOW(),"
        "    node_uuid    TEXT,"
        "    event_type   TEXT NOT NULL,"
        "    detail       TEXT"
        ");";

    const char *ht_events =
        "SELECT create_hypertable('cluster_events', 'time',"
        "    if_not_exists => TRUE);";

    /* Execute DDL */
    if (exec_simple(ctx, create_registry)    < 0) return -1;
    if (exec_simple(ctx, create_host_metrics) < 0) return -1;
    if (exec_simple(ctx, ht_host)             < 0) return -1;
    if (exec_simple(ctx, create_bf2_metrics)  < 0) return -1;
    if (exec_simple(ctx, ht_bf2)              < 0) return -1;
    if (exec_simple(ctx, create_events)       < 0) return -1;
    if (exec_simple(ctx, ht_events)           < 0) return -1;

    /* Compression policies (best-effort — may fail if not supported) */
    exec_simple(ctx,
        "ALTER TABLE host_metrics SET ("
        "    timescaledb.compress,"
        "    timescaledb.compress_segmentby = 'node_uuid'"
        ");");
    exec_simple(ctx,
        "SELECT add_compression_policy('host_metrics', INTERVAL '7 days',"
        "    if_not_exists => TRUE);");

    exec_simple(ctx,
        "ALTER TABLE bf2_metrics SET ("
        "    timescaledb.compress,"
        "    timescaledb.compress_segmentby = 'node_uuid'"
        ");");
    exec_simple(ctx,
        "SELECT add_compression_policy('bf2_metrics', INTERVAL '7 days',"
        "    if_not_exists => TRUE);");

    exec_simple(ctx,
        "ALTER TABLE cluster_events SET ("
        "    timescaledb.compress,"
        "    timescaledb.compress_segmentby = 'node_uuid'"
        ");");
    exec_simple(ctx,
        "SELECT add_compression_policy('cluster_events', INTERVAL '30 days',"
        "    if_not_exists => TRUE);");

    fprintf(stderr, "[db] schema v2 initialised\n");
    return 0;
}

int db_insert_host_metrics(db_ctx_t *ctx, const char *node_uuid, uint64_t ts_ns,
                           float cpu_pct, uint64_t mem_total_kb, uint64_t mem_avail_kb,
                           uint64_t net_rx, uint64_t net_tx)
{
    if (ensure_connected(ctx) < 0) return -1;

    char ts_buf[64];
    ns_to_iso8601(ts_ns, ts_buf, sizeof(ts_buf));

    char cpu_str[32], mt_str[32], ma_str[32], rx_str[32], tx_str[32];
    snprintf(cpu_str, sizeof(cpu_str), "%.4f", (double)cpu_pct);
    snprintf(mt_str,  sizeof(mt_str),  "%" PRIu64, mem_total_kb);
    snprintf(ma_str,  sizeof(ma_str),  "%" PRIu64, mem_avail_kb);
    snprintf(rx_str,  sizeof(rx_str),  "%" PRIu64, net_rx);
    snprintf(tx_str,  sizeof(tx_str),  "%" PRIu64, net_tx);

    const char *params[7] = { ts_buf, node_uuid, cpu_str, mt_str, ma_str, rx_str, tx_str };

    const char *sql =
        "INSERT INTO host_metrics "
        "(time, node_uuid, cpu_pct, mem_total_kb, mem_avail_kb, net_rx_bytes, net_tx_bytes)"
        " VALUES ($1, $2, $3, $4, $5, $6, $7)";

    PGresult *res = PQexecParams(ctx->conn, sql, 7, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        log_pq_error("db_insert_host_metrics", ctx->conn);
        PQclear(res);
        if (ensure_connected(ctx) == 0) {
            res = PQexecParams(ctx->conn, sql, 7, NULL, params, NULL, NULL, 0);
            ok  = (PQresultStatus(res) == PGRES_COMMAND_OK);
            if (!ok) log_pq_error("db_insert_host_metrics (retry)", ctx->conn);
        } else {
            return -1;
        }
    }
    PQclear(res);
    return ok ? 0 : -1;
}

int db_insert_bf2_metrics(db_ctx_t *ctx, const char *node_uuid, uint64_t ts_ns,
                          float arm_cpu_pct, uint64_t arm_mem_total_kb, uint64_t arm_mem_avail_kb,
                          float temp_c, uint64_t port_rx, uint64_t port_tx,
                          uint64_t port_drops, uint32_t ovs_flows)
{
    if (ensure_connected(ctx) < 0) return -1;

    char ts_buf[64];
    ns_to_iso8601(ts_ns, ts_buf, sizeof(ts_buf));

    char cpu_str[32], mt_str[32], ma_str[32], temp_str[32];
    char rx_str[32], tx_str[32], drops_str[32], flows_str[32];
    snprintf(cpu_str,   sizeof(cpu_str),   "%.4f", (double)arm_cpu_pct);
    snprintf(mt_str,    sizeof(mt_str),    "%" PRIu64, arm_mem_total_kb);
    snprintf(ma_str,    sizeof(ma_str),    "%" PRIu64, arm_mem_avail_kb);
    snprintf(temp_str,  sizeof(temp_str),  "%.2f", (double)temp_c);
    snprintf(rx_str,    sizeof(rx_str),    "%" PRIu64, port_rx);
    snprintf(tx_str,    sizeof(tx_str),    "%" PRIu64, port_tx);
    snprintf(drops_str, sizeof(drops_str), "%" PRIu64, port_drops);
    snprintf(flows_str, sizeof(flows_str), "%" PRIu32, ovs_flows);

    const char *params[10] = {
        ts_buf, node_uuid, cpu_str, mt_str, ma_str,
        temp_str, rx_str, tx_str, drops_str, flows_str
    };

    const char *sql =
        "INSERT INTO bf2_metrics "
        "(time, node_uuid, arm_cpu_pct, arm_mem_total_kb, arm_mem_avail_kb,"
        " temperature_c, port_rx_bytes, port_tx_bytes, port_rx_drops, ovs_flow_count)"
        " VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)";

    PGresult *res = PQexecParams(ctx->conn, sql, 10, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) {
        log_pq_error("db_insert_bf2_metrics", ctx->conn);
        PQclear(res);
        if (ensure_connected(ctx) == 0) {
            res = PQexecParams(ctx->conn, sql, 10, NULL, params, NULL, NULL, 0);
            ok  = (PQresultStatus(res) == PGRES_COMMAND_OK);
            if (!ok) log_pq_error("db_insert_bf2_metrics (retry)", ctx->conn);
        } else {
            return -1;
        }
    }
    PQclear(res);
    return ok ? 0 : -1;
}

int db_insert_event(db_ctx_t *ctx, const char *node_uuid, const char *event_type,
                    const char *detail)
{
    if (ensure_connected(ctx) < 0) return -1;

    const char *params[3] = {
        node_uuid ? node_uuid : "",
        event_type,
        detail ? detail : ""
    };

    const char *sql =
        "INSERT INTO cluster_events (time, node_uuid, event_type, detail)"
        " VALUES (NOW(), $1, $2, $3)";

    PGresult *res = PQexecParams(ctx->conn, sql, 3, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) log_pq_error("db_insert_event", ctx->conn);
    PQclear(res);
    return ok ? 0 : -1;
}

int db_upsert_node_registry(db_ctx_t *ctx, const char *node_uuid, const char *hostname,
                            const char *pci_bus_id, const char *state,
                            const char *host_status, const char *bf2_status)
{
    if (ensure_connected(ctx) < 0) return -1;

    const char *params[6] = {
        node_uuid,
        hostname     ? hostname     : "",
        pci_bus_id   ? pci_bus_id   : "",
        state        ? state        : "unknown",
        host_status  ? host_status  : "unknown",
        bf2_status   ? bf2_status   : "unknown"
    };

    const char *sql =
        "INSERT INTO node_registry "
        "(node_uuid, hostname, pci_bus_id, state, host_status, bf2_status, last_seen, registered_at)"
        " VALUES ($1, $2, $3, $4, $5, $6, NOW(), NOW())"
        " ON CONFLICT (node_uuid) DO UPDATE SET"
        "   hostname    = EXCLUDED.hostname,"
        "   pci_bus_id  = EXCLUDED.pci_bus_id,"
        "   state       = EXCLUDED.state,"
        "   host_status = EXCLUDED.host_status,"
        "   bf2_status  = EXCLUDED.bf2_status,"
        "   last_seen   = NOW()";

    PGresult *res = PQexecParams(ctx->conn, sql, 6, NULL, params, NULL, NULL, 0);
    int ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
    if (!ok) log_pq_error("db_upsert_node_registry", ctx->conn);
    PQclear(res);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Raw SQL execution (for batch inserts from DbWriter)                 */
/* ------------------------------------------------------------------ */

int db_exec_sql(db_ctx_t *ctx, const char *sql)
{
    if (ensure_connected(ctx) < 0) return -1;

    PGresult *res = PQexec(ctx->conn, sql);
    ExecStatusType status = PQresultStatus(res);
    int ok = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);
    if (!ok) {
        fprintf(stderr, "[db] batch exec failed: %s\n", PQerrorMessage(ctx->conn));
    }
    PQclear(res);
    return ok ? 0 : -1;
}
