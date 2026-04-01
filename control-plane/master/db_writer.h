/*
 * db_writer.h — Async batch DB writer with connection pool.
 *
 * gRPC handler threads enqueue metrics via lock-free spinlock (~50ns).
 * Dedicated writer threads drain the queue, batch rows into multi-row
 * INSERT statements, and flush to PostgreSQL via a connection pool.
 *
 * Typical throughput: 5000-10000 rows/s (vs ~60 rows/s with sync writes).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

struct db_ctx;
typedef struct db_ctx db_ctx_t;

class DbWriter {
public:
    /* pool_size = number of PGconn connections / writer threads
     * batch_size = max rows per INSERT statement
     * flush_ms = max time between flushes */
    DbWriter(const char* connstr, int pool_size = 4,
             int batch_size = 200, int flush_ms = 50);
    ~DbWriter();

    bool start();
    void stop();

    /* ---- High-frequency metric enqueue (called from gRPC threads) ---- */

    void enqueueHostMetrics(const char* node_uuid, uint64_t ts_ns,
                            float cpu, uint64_t mem_total, uint64_t mem_avail,
                            uint64_t net_rx, uint64_t net_tx);

    void enqueueBF2Metrics(const char* node_uuid, uint64_t ts_ns,
                           float arm_cpu, uint64_t arm_mem_total, uint64_t arm_mem_avail,
                           float temp, uint64_t port_rx, uint64_t port_tx,
                           uint64_t port_drops, uint32_t ovs_flows);

    /* ---- Low-frequency ops (still async but batched) ---- */

    void enqueueEvent(const char* node_uuid, const char* event_type,
                      const char* detail);

    /* ---- Stats ---- */
    uint64_t totalEnqueued() const { return total_enqueued_.load(std::memory_order_relaxed); }
    uint64_t totalFlushed()  const { return total_flushed_.load(std::memory_order_relaxed); }
    uint64_t totalDropped()  const { return total_dropped_.load(std::memory_order_relaxed); }

private:
    /* ---- Write operation types ---- */
    struct HostMetricOp {
        char     node_uuid[64];
        uint64_t ts_ns;
        float    cpu;
        uint64_t mem_total, mem_avail;
        uint64_t net_rx, net_tx;
    };

    struct BF2MetricOp {
        char     node_uuid[64];
        uint64_t ts_ns;
        float    arm_cpu;
        uint64_t arm_mem_total, arm_mem_avail;
        float    temp;
        uint64_t port_rx, port_tx, port_drops;
        uint32_t ovs_flows;
    };

    struct EventOp {
        char node_uuid[64];
        char event_type[32];
        char detail[256];
    };

    /* ---- Spinlock-protected pending queues ---- */
    alignas(64) std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    std::vector<HostMetricOp>  pending_host_;
    std::vector<BF2MetricOp>   pending_bf2_;
    std::vector<EventOp>       pending_events_;

    /* ---- Processing buffers (no lock needed, owned by writer) ---- */
    std::vector<HostMetricOp>  proc_host_;
    std::vector<BF2MetricOp>   proc_bf2_;
    std::vector<EventOp>       proc_events_;

    /* ---- Config ---- */
    std::string connstr_;
    int pool_size_;
    int batch_size_;
    int flush_ms_;

    /* ---- Connection pool + writer threads ---- */
    std::vector<db_ctx_t*>   conns_;
    std::vector<std::thread> writers_;
    std::atomic<bool> running_{false};

    /* ---- Stats ---- */
    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> total_flushed_{0};
    std::atomic<uint64_t> total_dropped_{0};

    static constexpr size_t MAX_PENDING = 100000;

    void spinLock()   { while (lock_.test_and_set(std::memory_order_acquire)) { /* spin */ } }
    void spinUnlock() { lock_.clear(std::memory_order_release); }

    void writerLoop(int thread_id);
    void flushHostBatch(db_ctx_t* conn, const HostMetricOp* ops, size_t count);
    void flushBF2Batch(db_ctx_t* conn, const BF2MetricOp* ops, size_t count);
    void flushEventBatch(db_ctx_t* conn, const EventOp* ops, size_t count);
};
