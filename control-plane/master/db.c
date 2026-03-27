/*
 * db.c — TimescaleDB interface implementation via libpq.
 *
 * Provides a single-connection wrapper with automatic reconnect on error.
 * All queries use parameterized statements to prevent SQL injection and
 * to let the server cache query plans.
 *
 * DDL (created by db_init_schema):
 *
 *   node_metrics  — TimescaleDB hypertable partitioned by time
 *   node_status   — Current status of each registered node
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

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

db_ctx_t *db_connect(const char *connstr)
{
    db_ctx_t *ctx = calloc(1, sizeof(db_ctx_t));
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

    fprintf(stderr, "[db] schema initialised\n");
    return 0;
}

int db_insert_resource(db_ctx_t *ctx, const resource_report_t *r)
{
    if (ensure_connected(ctx) < 0) return -1;

    /*
     * Convert timestamp_ns (unix nanoseconds) to an ISO-8601 string that
     * PostgreSQL can parse as TIMESTAMPTZ.
     */
    time_t sec = (time_t)(r->timestamp_ns / 1000000000ULL);
    long   ns  = (long)(r->timestamp_ns % 1000000000ULL);
    struct tm tm_utc;
    gmtime_r(&sec, &tm_utc);
    char ts_buf[64];
    snprintf(ts_buf, sizeof(ts_buf),
             "%04d-%02d-%02dT%02d:%02d:%02d.%09ldZ",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, ns);

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
