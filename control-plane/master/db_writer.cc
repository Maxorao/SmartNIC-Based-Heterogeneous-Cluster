/*
 * db_writer.cc — Async batch DB writer implementation.
 *
 * Key optimizations:
 * 1. Spinlock enqueue (~50ns per op, no syscall)
 * 2. Vector swap to drain queue (pointer swap under spinlock)
 * 3. Multi-row INSERT (100+ rows per SQL statement)
 * 4. Connection pool (multiple PGconn for parallel writes)
 */

#include "db_writer.h"
#include "db.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void ns_to_pg_timestamp(uint64_t ts_ns, char* buf, size_t buf_size)
{
    time_t sec = (time_t)(ts_ns / 1000000000ULL);
    long ns = (long)(ts_ns % 1000000000ULL);
    struct tm utc;
    gmtime_r(&sec, &utc);
    snprintf(buf, buf_size,
             "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
             utc.tm_hour, utc.tm_min, utc.tm_sec, ns / 1000);
}

/* Escape single quotes for SQL string literals */
static void sql_escape(const char* src, char* dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 2; i++) {
        if (src[i] == '\'') { dst[j++] = '\''; }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* ------------------------------------------------------------------ */
/* Constructor / Destructor                                            */
/* ------------------------------------------------------------------ */

DbWriter::DbWriter(const char* connstr, int pool_size,
                   int batch_size, int flush_ms)
    : connstr_(connstr)
    , pool_size_(pool_size)
    , batch_size_(batch_size)
    , flush_ms_(flush_ms)
{
    pending_host_.reserve(4096);
    pending_bf2_.reserve(1024);
    pending_events_.reserve(256);
    proc_host_.reserve(4096);
    proc_bf2_.reserve(1024);
    proc_events_.reserve(256);
}

DbWriter::~DbWriter()
{
    stop();
}

/* ------------------------------------------------------------------ */
/* Start / Stop                                                        */
/* ------------------------------------------------------------------ */

bool DbWriter::start()
{
    /* Create connection pool */
    for (int i = 0; i < pool_size_; i++) {
        db_ctx_t* conn = db_connect(connstr_.c_str());
        if (!conn) {
            fprintf(stderr, "[db_writer] failed to create connection %d/%d\n",
                    i, pool_size_);
            /* Clean up already-created connections */
            for (auto* c : conns_) db_disconnect(c);
            conns_.clear();
            return false;
        }
        conns_.push_back(conn);
    }

    fprintf(stderr, "[db_writer] connection pool: %d connections\n", pool_size_);

    /* Start writer threads */
    running_.store(true);
    for (int i = 0; i < pool_size_; i++) {
        writers_.emplace_back(&DbWriter::writerLoop, this, i);
    }

    fprintf(stderr, "[db_writer] started (%d writers, batch=%d, flush=%dms)\n",
            pool_size_, batch_size_, flush_ms_);
    return true;
}

void DbWriter::stop()
{
    if (!running_.load()) return;

    running_.store(false);

    for (auto& t : writers_) {
        if (t.joinable()) t.join();
    }
    writers_.clear();

    /* Final flush: drain anything remaining */
    if (!conns_.empty()) {
        spinLock();
        proc_host_.swap(pending_host_);
        proc_bf2_.swap(pending_bf2_);
        proc_events_.swap(pending_events_);
        spinUnlock();

        if (!proc_host_.empty())
            flushHostBatch(conns_[0], proc_host_.data(), proc_host_.size());
        if (!proc_bf2_.empty())
            flushBF2Batch(conns_[0], proc_bf2_.data(), proc_bf2_.size());
        if (!proc_events_.empty())
            flushEventBatch(conns_[0], proc_events_.data(), proc_events_.size());
    }

    for (auto* c : conns_) db_disconnect(c);
    conns_.clear();

    fprintf(stderr, "[db_writer] stopped (enqueued=%" PRIu64 " flushed=%" PRIu64
            " dropped=%" PRIu64 ")\n",
            total_enqueued_.load(), total_flushed_.load(), total_dropped_.load());
}

/* ------------------------------------------------------------------ */
/* Enqueue (called from many gRPC threads)                             */
/* ------------------------------------------------------------------ */

void DbWriter::enqueueHostMetrics(const char* node_uuid, uint64_t ts_ns,
                                  float cpu, uint64_t mem_total, uint64_t mem_avail,
                                  uint64_t net_rx, uint64_t net_tx)
{
    HostMetricOp op{};
    strncpy(op.node_uuid, node_uuid, sizeof(op.node_uuid) - 1);
    op.ts_ns = ts_ns;
    op.cpu = cpu;
    op.mem_total = mem_total;
    op.mem_avail = mem_avail;
    op.net_rx = net_rx;
    op.net_tx = net_tx;

    spinLock();
    if (pending_host_.size() < MAX_PENDING) {
        pending_host_.push_back(op);
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
    } else {
        total_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    spinUnlock();
}

void DbWriter::enqueueBF2Metrics(const char* node_uuid, uint64_t ts_ns,
                                 float arm_cpu, uint64_t arm_mem_total, uint64_t arm_mem_avail,
                                 float temp, uint64_t port_rx, uint64_t port_tx,
                                 uint64_t port_drops, uint32_t ovs_flows)
{
    BF2MetricOp op{};
    strncpy(op.node_uuid, node_uuid, sizeof(op.node_uuid) - 1);
    op.ts_ns = ts_ns;
    op.arm_cpu = arm_cpu;
    op.arm_mem_total = arm_mem_total;
    op.arm_mem_avail = arm_mem_avail;
    op.temp = temp;
    op.port_rx = port_rx;
    op.port_tx = port_tx;
    op.port_drops = port_drops;
    op.ovs_flows = ovs_flows;

    spinLock();
    if (pending_bf2_.size() < MAX_PENDING) {
        pending_bf2_.push_back(op);
        total_enqueued_.fetch_add(1, std::memory_order_relaxed);
    } else {
        total_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    spinUnlock();
}

void DbWriter::enqueueEvent(const char* node_uuid, const char* event_type,
                            const char* detail)
{
    EventOp op{};
    if (node_uuid) strncpy(op.node_uuid, node_uuid, sizeof(op.node_uuid) - 1);
    strncpy(op.event_type, event_type, sizeof(op.event_type) - 1);
    if (detail) strncpy(op.detail, detail, sizeof(op.detail) - 1);

    spinLock();
    pending_events_.push_back(op);
    total_enqueued_.fetch_add(1, std::memory_order_relaxed);
    spinUnlock();
}

/* ------------------------------------------------------------------ */
/* Writer thread                                                       */
/* ------------------------------------------------------------------ */

void DbWriter::writerLoop(int thread_id)
{
    /* Each thread gets its own connection from the pool */
    db_ctx_t* conn = conns_[thread_id];

    /* Local work buffers */
    std::vector<HostMetricOp> local_host;
    std::vector<BF2MetricOp>  local_bf2;
    std::vector<EventOp>      local_events;
    local_host.reserve(batch_size_ * 2);
    local_bf2.reserve(batch_size_);
    local_events.reserve(64);

    while (running_.load()) {
        /* Swap pending → processing under spinlock */
        {
            spinLock();
            if (thread_id == 0) {
                /* Thread 0 handles all types */
                pending_host_.swap(local_host);
                pending_bf2_.swap(local_bf2);
                pending_events_.swap(local_events);
            } else {
                /* Other threads only help with host metrics overflow */
                if (pending_host_.size() > (size_t)batch_size_) {
                    /* Take half the pending work */
                    size_t half = pending_host_.size() / 2;
                    local_host.assign(pending_host_.begin() + half, pending_host_.end());
                    pending_host_.resize(half);
                }
            }
            spinUnlock();
        }

        bool did_work = false;

        /* Flush host metrics in batches */
        if (!local_host.empty()) {
            for (size_t i = 0; i < local_host.size(); i += batch_size_) {
                size_t count = std::min((size_t)batch_size_, local_host.size() - i);
                flushHostBatch(conn, local_host.data() + i, count);
            }
            total_flushed_.fetch_add(local_host.size(), std::memory_order_relaxed);
            local_host.clear();
            did_work = true;
        }

        /* Flush BF2 metrics */
        if (!local_bf2.empty()) {
            for (size_t i = 0; i < local_bf2.size(); i += batch_size_) {
                size_t count = std::min((size_t)batch_size_, local_bf2.size() - i);
                flushBF2Batch(conn, local_bf2.data() + i, count);
            }
            total_flushed_.fetch_add(local_bf2.size(), std::memory_order_relaxed);
            local_bf2.clear();
            did_work = true;
        }

        /* Flush events */
        if (!local_events.empty()) {
            flushEventBatch(conn, local_events.data(), local_events.size());
            total_flushed_.fetch_add(local_events.size(), std::memory_order_relaxed);
            local_events.clear();
            did_work = true;
        }

        if (!did_work) {
            std::this_thread::sleep_for(std::chrono::milliseconds(flush_ms_));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Batch INSERT builders                                               */
/* ------------------------------------------------------------------ */

void DbWriter::flushHostBatch(db_ctx_t* conn, const HostMetricOp* ops, size_t count)
{
    if (count == 0) return;

    /* Build multi-row INSERT:
     * INSERT INTO host_metrics (time, node_uuid, cpu_pct, mem_total_kb,
     *   mem_avail_kb, net_rx_bytes, net_tx_bytes) VALUES
     *   ('...', '...', 10.5, 100, 200, 300, 400),
     *   ('...', '...', 20.3, 100, 200, 300, 400); */

    std::string sql;
    sql.reserve(count * 150 + 200);
    sql = "INSERT INTO host_metrics (time, node_uuid, cpu_pct, mem_total_kb, "
          "mem_avail_kb, net_rx_bytes, net_tx_bytes) VALUES\n";

    char ts[64], uuid_esc[128];
    for (size_t i = 0; i < count; i++) {
        const auto& op = ops[i];
        ns_to_pg_timestamp(op.ts_ns, ts, sizeof(ts));
        sql_escape(op.node_uuid, uuid_esc, sizeof(uuid_esc));

        if (i > 0) sql += ",\n";
        char row[256];
        snprintf(row, sizeof(row),
                 "('%s','%s',%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ")",
                 ts, uuid_esc, op.cpu, op.mem_total, op.mem_avail,
                 op.net_rx, op.net_tx);
        sql += row;
    }
    sql += ";";

    db_exec_sql(conn, sql.c_str());
}

void DbWriter::flushBF2Batch(db_ctx_t* conn, const BF2MetricOp* ops, size_t count)
{
    if (count == 0) return;

    std::string sql;
    sql.reserve(count * 200 + 300);
    sql = "INSERT INTO bf2_metrics (time, node_uuid, arm_cpu_pct, arm_mem_total_kb, "
          "arm_mem_avail_kb, temperature_c, port_rx_bytes, port_tx_bytes, "
          "port_rx_drops, ovs_flow_count) VALUES\n";

    char ts[64], uuid_esc[128];
    for (size_t i = 0; i < count; i++) {
        const auto& op = ops[i];
        ns_to_pg_timestamp(op.ts_ns, ts, sizeof(ts));
        sql_escape(op.node_uuid, uuid_esc, sizeof(uuid_esc));

        if (i > 0) sql += ",\n";
        char row[320];
        snprintf(row, sizeof(row),
                 "('%s','%s',%.4f,%" PRIu64 ",%" PRIu64 ",%.2f,%" PRIu64
                 ",%" PRIu64 ",%" PRIu64 ",%u)",
                 ts, uuid_esc, op.arm_cpu, op.arm_mem_total, op.arm_mem_avail,
                 op.temp, op.port_rx, op.port_tx, op.port_drops, op.ovs_flows);
        sql += row;
    }
    sql += ";";

    db_exec_sql(conn, sql.c_str());
}

void DbWriter::flushEventBatch(db_ctx_t* conn, const EventOp* ops, size_t count)
{
    if (count == 0) return;

    std::string sql;
    sql.reserve(count * 300 + 200);
    sql = "INSERT INTO cluster_events (time, node_uuid, event_type, detail) VALUES\n";

    char uuid_esc[128], type_esc[64], detail_esc[512];
    for (size_t i = 0; i < count; i++) {
        const auto& op = ops[i];
        sql_escape(op.node_uuid, uuid_esc, sizeof(uuid_esc));
        sql_escape(op.event_type, type_esc, sizeof(type_esc));
        sql_escape(op.detail, detail_esc, sizeof(detail_esc));

        if (i > 0) sql += ",\n";
        char row[600];
        snprintf(row, sizeof(row),
                 "(NOW(),'%s','%s','%s')",
                 uuid_esc, type_esc, detail_esc);
        sql += row;
    }
    sql += ";";

    db_exec_sql(conn, sql.c_str());
}
