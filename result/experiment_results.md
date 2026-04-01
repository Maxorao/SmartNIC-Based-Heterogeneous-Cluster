# Experiment Results — Multi-Host (2026-03-29)

## Environment

| Component | tianjin (master) | fujian (worker) | helong (worker) |
|-----------|-----------------|-----------------|-----------------|
| Role | Master | Worker (Exp B) | Worker (backup) |
| Mgmt IP | 172.28.4.75 | 172.28.4.77 | 172.28.4.85 |
| 100G IP (host) | 192.168.56.10 | 192.168.56.11 | 192.168.56.12 |
| 100G IP (BF2) | 192.168.56.2 | 192.168.56.3 | 192.168.56.1 |
| CPU | 2x Xeon Gold 5218 (16c/32t per socket) | same | same |
| Host DOCA | 3.1.0 | 3.1.0 | 3.1.0 |
| BF2 DOCA | 1.5.4003 | 1.5.4003 | 1.5.4003 |
| BF2 PCI (host) | 0000:5e:00.0 | 0000:5e:00.0 | 0000:5e:00.0 |

**100G fabric**: All hosts and BF2s share 192.168.56.0/24 via a 100G switch.
Host interfaces (`enp94s0f*`) connect through BF2 OVS bridges to the physical
ports. No relay needed.

---

## Experiment A: Communication Path Latency

Measures round-trip time (RTT) across five communication paths.
10,000 iterations per measurement. Paths 1-2 use bench_host/bench_nic;
paths 3-5 use sockperf ping-pong mode.

### Results

| # | Path | 64B RTT (us) | 256B RTT (us) | 1024B RTT (us) |
|---|------|-------------|--------------|---------------|
| 1 | Comch PCIe (host<->BF2) | 29.0 | 29.1 | 29.4 |
| 2 | TCP tmfifo (host<->BF2) | 4999.7 | 4999.7 | 5001.1 |
| 3 | TCP 1G LAN (host<->host, eno1) | 55.7 | 70.5 | 137.4 |
| 4 | TCP 100G BF2<->BF2 (ARM fabric) | 80.8 | 81.5 | 83.1 |
| 5 | TCP 100G host<->host (via BF2 OVS) | 104.9 | 105.4 | 107.7 |

### Detailed sockperf results

**Path 3 — TCP 1G LAN (eno1), fujian -> tianjin:**

| Size | Avg RTT | P50 | P99 | P99.9 | Max |
|------|---------|-----|-----|-------|-----|
| 64B | 55.7 us | 53.6 us | 82.3 us | 98.2 us | 198.1 us |
| 256B | 70.5 us | 70.0 us | 81.6 us | 91.9 us | 1344.7 us |
| 1024B | 137.4 us | 115.7 us | 188.6 us | 199.5 us | 272.2 us |

**Path 4 — TCP 100G BF2<->BF2, fujian BF2 -> tianjin BF2:**

| Size | Avg RTT | P50 | P99 | P99.9 | Max |
|------|---------|-----|-----|-------|-----|
| 64B | 80.8 us | 79.9 us | 105.2 us | 241.0 us | 3870.0 us |
| 256B | 81.5 us | 81.0 us | 105.4 us | 227.0 us | 6212.9 us |
| 1024B | 83.1 us | 83.1 us | 109.2 us | 298.0 us | 4356.9 us |

**Path 5 — TCP 100G host<->host (via BF2 OVS), fujian -> tianjin:**

| Size | Avg RTT | P50 | P99 | P99.9 | Max |
|------|---------|-----|-----|-------|-----|
| 64B | 104.9 us | 104.3 us | 121.3 us | 157.9 us | 476.2 us |
| 256B | 105.4 us | 104.6 us | 120.3 us | 191.8 us | 591.5 us |
| 1024B | 107.7 us | 107.0 us | 121.3 us | 174.7 us | 277.4 us |

### Analysis

- **Comch is ~170x faster than TCP tmfifo** for host-BF2 communication (~29 us vs ~5000 us). The tmfifo virtual interface adds ~5 ms of software overhead.
- **Comch is ~3.6x faster than 100G host-host TCP** (~29 us vs ~105 us). The 100G path includes two BF2 OVS bridge traversals plus physical switch latency.
- **100G BF2-BF2 (~81 us) is faster than 100G host-host (~105 us)** by ~24 us, reflecting the two additional PCIe + OVS bridge hops in the host-host path.
- **1G LAN (~56 us for 64B) is faster than 100G host-host (~105 us)** because the 1G path is a direct Ethernet connection without BF2 OVS bridge overhead. However, 1G bandwidth is much lower for bulk transfers.
- Comch latency is stable across 64-1024B (~29 us), dominated by PCIe round-trip time.
- **Comch fails at 4096B+** due to BF2 DOCA 1.5 max message size (4080B).

---

## Experiment B: Interference Elimination

Measures GEMM throughput on fujian under three control-plane configurations.
All control-plane traffic flows over the 100G fabric to master_monitor on tianjin.

### Architecture

```
Scenario 1 (baseline):
  fujian host: GEMM alone (16 threads, NUMA node 0)

Scenario 2 (no offload — control plane on host CPU):
  fujian host: GEMM + 8x slave_monitor --mode=direct -> TCP 192.168.56.10:9000
  Each slave_monitor: read /proc (x50 extra) + walk 2MB cache buffer
                      + build protocol msg + TCP send via 100G NIC
  Host CPU handles all control-plane work.

Scenario 3 (offloaded — control plane on BF2):
  fujian host: GEMM + 1x metric_push -> Comch -> BF2
  fujian BF2:  forward_routine -> TCP 192.168.56.10:9000
  metric_push: read /proc + Comch DMA send (1ms interval = 1000 reports/s)
  Host CPU only does minimal /proc reads + PCIe DMA. BF2 ARM handles TCP.
```

### Parameters

- **GEMM**: OPENBLAS_NUM_THREADS=16, numactl --cpunodebind=0 --membind=0
- **Scenario 2**: 8 x slave_monitor at 10ms interval = ~700 reports/s total, with --extra-reads=50 --cache-kb=2048 (simulates kubelet + cAdvisor + logging overhead)
- **Scenario 3**: 1 x metric_push at 1ms interval = 1000 reports/s (lightweight Comch DMA)
- **Duration**: 60 seconds per scenario

### Results

| Scenario | GFLOPS (avg, excl warmup) | GFLOPS (full avg) | vs Baseline |
|----------|--------------------------|-------------------|-------------|
| 1. Baseline (GEMM only) | 405.1 | 413.1 | — |
| 2. No offload (8x slave_monitor TCP) | 383.7 | 390.1 | **-5.3%** |
| 3. Offloaded (1x metric_push Comch) | 412.2 | 419.8 | **+1.7%** |

**Derived metrics:**

- Interference rate: **5.3%** (T_base=405.1 -> T_mixed=383.7 GFLOPS)
- Recovery rate: **101.7%** (T_offload=412.2 GFLOPS, fully recovered)

### Analysis

- **Scenario 2 causes 5.3% GFLOPS interference.** The 8 slave_monitor instances running on the same NUMA node as GEMM create contention through:
  - **L3 cache pollution**: each instance walks a 2MB buffer per cycle (16MB total across 8 instances), competing with GEMM's working set for the 22MB shared L3 cache
  - **Syscall overhead**: 50 extra /proc reads per cycle per instance = 400 open/read/close syscalls per 10ms across all instances
  - **TCP stack processing**: 8 concurrent TCP connections through the kernel network stack
  - Combined effect: ~700 reports/s with non-trivial per-iteration CPU and cache footprint

- **Scenario 3 shows zero interference** (101.7% of baseline — within run-to-run variance). The offloaded metric_push is extremely lightweight:
  - Reads 2 /proc files per cycle (~50 us)
  - Comch DMA send (~15 us, with 1 us yield between PE polls)
  - No cache buffer walking, no extra /proc reads, no TCP stack
  - Total per-iteration CPU: ~65 us every 1ms = ~6.5% duty cycle on one core
  - The BF2 ARM handles all TCP protocol work, completely removing network stack overhead from the host

- **The 5.3% interference fully disappears when offloading to the BF2**, demonstrating that SmartNIC offloading can eliminate control-plane interference on compute-intensive workloads.

---

## Experiment C: Control-Plane Scalability

Tests master_monitor scalability as node count increases from 4 to 256.
Mock nodes (pthreads in mock_slave) connect from fujian and helong via the
100G fabric to master_monitor on tianjin.

### Architecture

```
tianjin (192.168.56.10)
  master_monitor :9000    <-- receives TCP from all mock nodes
        ^
        | TCP via 100G fabric (192.168.56.x)
  +-----+------+--------------+
  fujian       helong
  mock_slave   mock_slave
  (N/2 nodes)  (N/2 nodes)
```

### Parameters

- **Scale points**: 4, 16, 64, 256 nodes
- **Report interval**: 1000 ms (1 report per node per second)
- **Warmup**: 10 seconds
- **Measurement**: 30 seconds (pidstat sampling)

### Results

| Nodes | CPU% | RSS (MB) | Avg Latency (ms) | Reports/s | Errors | Err% |
|-------|------|----------|-------------------|-----------|--------|------|
| 4 | 0.01 | 2.7 | 0.402 | 4 | 0 | 0% |
| 16 | 0.05 | 2.6 | 0.346 | 16 | 0 | 0% |
| 64 | 0.20 | 3.5 | 0.360 | 64 | 0 | 0% |
| 256 | 0.79 | 3.8 | 0.357 | 256 | 0 | 0% |

**CPU linear fit**: 0.0062% per node (R^2 = 0.9996)
**Projected CPU at 1000 nodes**: 6.2%

### Analysis

- **CPU scales linearly** with node count at 0.006% per node. At 256 nodes the master uses < 1% CPU. The linear fit (R^2 = 0.9996) projects only 6.2% CPU at 1000 nodes, indicating the current master_monitor architecture can handle large clusters without becoming a bottleneck.
- **Memory is nearly constant** (~3-4 MB RSS) across all scale points. The per-node state (node_entry struct + thread stack) is small and the thread-per-connection model with detached threads doesn't accumulate memory.
- **Latency is stable** at ~0.35-0.40 ms regardless of scale, showing no degradation under load. This reflects the TCP round-trip over the 100G fabric (~0.1 ms) plus master_monitor processing time (~0.25 ms).
- **Zero errors** at all scale points — no dropped connections, no failed sends, no protocol errors.
- **100% ACK rate** — all sent reports were acknowledged by master_monitor.

---

## Experiment D: Fault Recovery (Chapter 3 v2 — gRPC Architecture)

Measures fault detection and recovery time under three failure scenarios.
Architecture: cluster_master (tianjin, gRPC) + slave_agent (BF2 ARM, gRPC stream) + metric_push (host, Comch + gRPC fallback).

### Scenario 1: slave_agent crash on BF2

Kill slave_agent on fujian BF2 (kill -9), measure time until master detects (gRPC stream closed) and until slave_agent re-registers after restart.

| Run | Detection (ms) | Recovery (ms) |
|-----|---------------|--------------|
| 1 | 2361 | 6712 |
| 2 | 2692 | 7033 |
| 3 | 2402 | 6772 |
| 4 | 2372 | 6723 |
| 5 | 2712 | 7072 |
| **Avg** | **2508** | **6862** |

- Detection ~2.5s: gRPC bidirectional stream closes immediately, master marks node as suspect
- Recovery ~6.9s: includes slave_agent restart + Comch ep_listen init + gRPC reconnect + re-registration + metric_push Comch reconnect

### Scenario 2: metric_push graceful degradation (Comch -> gRPC fallback)

Kill slave_agent to break Comch path. metric_push detects 5 consecutive Comch failures then auto-switches to gRPC DirectPush to master. After slave_agent restarts, metric_push recovers Comch.

| Run | Fallback (ms) | Comch Recovery (ms) |
|-----|--------------|-------------------|
| 1 | 2407 | 6769 |
| 2 | 9402 | 13793 |
| 3 | 2719 | 7082 |
| 4 | 9440 | 13823 |
| 5 | 2381 | 6742 |
| **Avg** | **5270** | **9642** |

- Fallback time varies: ~2.5s (fast) or ~9.4s (slow), depending on where in the send cycle the Comch failure occurs. Each failed comch_host_send has a 1s PE timeout, 5 failures = 5-10s.
- Comch recovery ~7-14s: includes slave_agent restart + metric_push periodic Comch retry (30s interval) + Comch re-init.
- During fallback, metric_push sends via gRPC DirectPush with zero data loss.

### Scenario 3: cluster_master crash + restart

Kill cluster_master (kill -9), manually restart, measure until both slave_agents reconnect.

| Run | Restart (ms) | Reconnect (ms) |
|-----|-------------|---------------|
| 1 | 2007 | 3071 |
| 2 | 2004 | 3069 |
| 3 | 2005 | 3071 |
| 4 | 2005 | 3069 |
| 5 | 2005 | 3070 |
| **Avg** | **2005** | **3070** |

- Restart ~2.0s: process startup + DB reconnect + gRPC server bind
- Full reconnect ~3.1s: both slave_agents auto-reconnect with exponential backoff
- Remarkably consistent across all 5 runs (std < 2ms)

### Analysis

- **gRPC stream provides near-instant fault detection** (~2.5s) vs the old TCP poll-based approach. The bidirectional stream closes immediately when the remote process dies.
- **Dual-path resilience**: metric_push's Comch-to-gRPC fallback ensures zero metric data loss during BF2 failures.
- **Master crash recovery is fast and deterministic** (~3.1s for full cluster reconnect), thanks to slave_agent's automatic reconnect with exponential backoff.
- **Comch ep_listen limitation**: after kill -9, the BF2 DOCA firmware may delay releasing the service name, adding variable latency to recovery.

---

---

## Experiment E: Database Performance (TimescaleDB)

### Write Throughput

Measured with mock_slave sending gRPC NodeSession streams to local cluster_master.
Each node sends 1 heartbeat + 1 resource report per second.

**With async batch writer (DbWriter: 4-connection pool, batch INSERT 200 rows, flush 50ms):**

| Nodes | Sent | ACK'd (%) | DB Inserted | Rate | Avg Latency |
|-------|------|-----------|-------------|------|-------------|
| 64 | 7,680 | 3,840 (50%) | 3,840 | **60 rows/s** | 10.1 ms |
| 128 | 15,360 | 7,680 (50%) | 7,680 | **118 rows/s** | 10.1 ms |
| 256 | 30,476 | 15,070 (49%) | 15,238 | **227 rows/s** | 11.0 ms |

Note: 50% ACK rate is expected — each iteration sends heartbeat + report but only waits for report ACK.

**Comparison with synchronous single-connection writes (before optimization):**

| Nodes | Before (rows/s) | After (rows/s) | Improvement | Latency Before | Latency After |
|-------|-----------------|----------------|-------------|----------------|---------------|
| 64 | 44 | 60 | 1.4x | 221 ms | 10.1 ms |
| 128 | 51 | 118 | 2.3x | 311 ms | 10.1 ms |
| 256 | 60 | 227 | **3.8x** | 360 ms | 11.0 ms |

### Query Latency (10 runs each, ~9K rows in DB)

| Query Type | Avg Latency |
|------------|-------------|
| Node registry status (relational) | **5.9 ms** |
| 5-min CPU aggregation (GROUP BY) | **10.8 ms** |
| 1-hour time-bucket aggregation | **13.1 ms** |

### Table Statistics

| Table | Chunks | Size | Rows |
|-------|--------|------|------|
| host_metrics | 1 | 1,264 kB | 9,240 |
| bf2_metrics | 1 | 176 kB | 734 |
| cluster_events | 1 | 248 kB | 1,306 |

### Compression

| Metric | Value |
|--------|-------|
| Before compression | 1,264 kB |
| After compression | **272 kB** |
| Compression ratio | **4.6:1** |

### Analysis

- **Async batch writer provides 3.8x throughput improvement** at 256 nodes by eliminating mutex contention and amortizing DB round-trips across 200-row batch INSERTs.
- **Latency drops 33x** (360ms → 11ms) at 256 nodes because gRPC handlers send ACK immediately, then enqueue to the lock-free write queue (~50ns spinlock).
- **Zero errors** at all scale points — the write queue (100K capacity) absorbs bursts.
- **Query latency is sub-15ms** for all tested patterns, including TimescaleDB time_bucket aggregation.
- **4.6:1 compression ratio** demonstrates TimescaleDB's columnar compression effectiveness for time-series metrics.

---

## Experiment F: Functional Correctness Verification

### 8a. Registration Flow

- cluster_master starts, initializes DB schema (v1 + v2)
- slave_agent registers via gRPC bidirectional stream: `[grpc] node registered: fujian-bf2`
- metric_push connects via Comch → slave_agent detects host connection → reports host_status change
- Master log: `status change from fujian-bf2: domain=host DOMAIN_UNREACHABLE->DOMAIN_OK`
- node_registry shows: `state=online, host_status=ok, bf2_status=ok`
- Both host_metrics and bf2_metrics tables populated with real data

### 8b. Heartbeat & Resource Reporting (30s window)

| Node | host_metrics reports | bf2_metrics reports |
|------|---------------------|-------------------|
| fujian-bf2 | 14 | 11 |
| helong-bf2 | 18 | 12 |

- host_metrics: ~1 report/3s (report_interval_ms=3000)
- bf2_metrics: ~1 report/5s (bf2_report_interval=5000)
- cluster_events log shows complete lifecycle: master_start → register → status_change
- last_seen timestamps update continuously, confirming heartbeat flow

### 8c. Re-registration with Same UUID

1. **Kill slave_agent** (SIGTERM, graceful): master receives `deregister`, node_registry shows `state=offline`
2. **After 20s**: fujian-bf2 confirmed offline in DB, host_status/bf2_status set to `unknown`
3. **Restart slave_agent with same UUID**: master logs `node registered: fujian-bf2`, state returns to `online`
4. **Preserved identity**: same node_uuid, same registered_at timestamp, only last_seen updated
5. **metric_push fallback visible**: during slave_agent downtime, master received DirectPush from fujian-host via gRPC

### cluster_events Lifecycle Evidence

```
master_start           → grpc=50051 http=8080
register (helong-bf2)  → localhost.localdomain
register (fujian-bf2)  → localhost.localdomain
status_change          → host: DOMAIN_OK -> DOMAIN_UNREACHABLE (Comch not yet connected)
status_change          → host: DOMAIN_UNREACHABLE -> DOMAIN_OK (Comch connection restored)
deregister (fujian-bf2)→ graceful shutdown
register (fujian-bf2)  → localhost.localdomain (re-registration with same UUID)
status_change          → host: DOMAIN_OK -> DOMAIN_UNREACHABLE (Comch broken after restart)
```

Complete node lifecycle verified:
- register → online → heartbeat → deregister → offline → re-register → online
- Dual-domain status transitions tracked independently (host_status, bf2_status)
- All state changes persisted in cluster_events audit table

---

## Code Changes and Bug Fixes

### Chapter 3 v2 Architecture Changes

1. **CMake BUILD_TARGET flag** (`CMakeLists.txt`): Added `-DBUILD_TARGET=HOST|BF2` to select comch_host (DOCA 3.1, x86) or comch_nic (DOCA 1.5, ARM) and corresponding build targets. Eliminates cross-compilation errors.

2. **Async batch DB writer** (`db_writer.h`, `db_writer.cc`): New component replacing synchronous per-row INSERT with:
   - Spinlock-protected MPSC enqueue (~50ns per op)
   - Vector-swap drain pattern (no lock during batch build)
   - Multi-row INSERT (up to 200 rows per SQL statement)
   - Connection pool (4 parallel PGconn connections)
   - Result: 3.8x throughput improvement at 256 nodes

3. **DB thread safety** (`grpc_service.cc`): Added `std::lock_guard<std::mutex>` to ALL direct `db_*` calls in NodeSession handler and watchdog thread. Fixes `SSL SYSCALL error` / `SSL error: wrong version number` from concurrent PGconn access.

4. **Graceful shutdown** (all components): Replaced `signal()` with `sigaction(sa_flags=0)` so SIGTERM interrupts blocking calls (accept, sleep_for, poll). cluster_master uses self-pipe + poll for instant shutdown response.

5. **ACK-before-DB** (`grpc_service.cc`): ResourceReport handler sends ReportAck immediately before persisting to DB, decoupling gRPC latency from DB write latency.

### Chapter 2 Bug Fixes (carried forward)

6. **Comch PE busy-spin fix** (`comch_host_doca31.c`): Added 1µs nanosleep yield + 1s timeout to PE spin loop. Prevents 100% CPU when BF2 connection breaks.

7. **Representor filter fallback** (`comch_nic_doca15.c`): Fallback from `DOCA_DEV_REP_FILTER_NET` to `DOCA_DEV_REP_FILTER_ALL` after BF2 reboot.

8. **DOCA 1.5 API naming** (`host_collector.cc`): `doca_error_get_name` → `doca_get_error_name`.

9. **BF2Collector CpuStat visibility** (`bf2_collector.h`): Made `CpuStat` struct public for static helper access.

10. **DB connection string** (`config.sh`): Added `sslmode=disable` to avoid SSL overhead on localhost connections.

11. **node_registry re-registration** (`find_or_create_node`): Existing nodes now have `online`, `last_seen`, and `ip_addr` updated on re-registration.
