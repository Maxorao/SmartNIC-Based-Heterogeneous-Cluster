/*
 * db.h — TimescaleDB interface for the cluster master monitor.
 *
 * Uses libpq (PostgreSQL C client) to write resource metrics and node
 * status into a TimescaleDB hypertable.
 *
 * Schema is created automatically by db_init_schema().
 */

#pragma once

#include <stdint.h>
#include "../../common/protocol.h"

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
 * Create tables if they do not exist.
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
