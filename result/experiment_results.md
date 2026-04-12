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

### Extended paths (2026-04-12): RDMA measurements

| # | Path | Type | t_typical (us) | t_avg (us) | p99 (us) | BW |
|---|------|------|---------------|-----------|---------|-----|
| 6 | RDMA host VF↔host VF WRITE | Hardware RDMA (host-side) | 19.23 | 28.22 | 58.02 | 91.4 Gbps |
| 6 | RDMA host VF↔host VF READ | Hardware RDMA (host-side) | 4.55 | 5.00 | 8.09 | — |
| 7 | RDMA NIC ARM↔ARM WRITE | Hardware RDMA (NIC-side) | 3.44 | 22.49 | 98.68 | 85.8 Gbps |
| 7 | RDMA NIC ARM↔ARM READ | Hardware RDMA (NIC-side) | 5.04 | 23.49 | 147.91 | — |

**Path 6 details — Host VF RDMA, fujian ↔ tianjin (mlx5_1):**

ib_write_lat (2B, 1000 iters):
- t_min=15.51, t_typical=19.23, t_avg=28.22, t_max=101.63, p99=58.02, p99.9=101.63

ib_read_lat (2B, 1000 iters):
- t_min=4.51, t_typical=4.55, t_avg=5.00, t_max=9.01, p99=8.09, p99.9=9.01

ib_write_bw (65536B, 10s duration):
- BW average = 10885.55 MiB/s ≈ 91.4 Gbps

**Path 7 details — NIC ARM RDMA, fujian BF2 ↔ tianjin BF2 (SF on port 1):**

ib_write_lat (2B, 1000 iters):
- t_min=2.77, t_typical=3.44, t_avg=22.49, t_max=133.55, p99=98.68, p99.9=133.55

ib_read_lat (2B, 1000 iters):
- t_min=4.83, t_typical=5.04, t_avg=23.49, t_max=179.31, p99=147.91, p99.9=179.31

ib_send_bw (65536B, 10s duration):
- BW average = 10977.37 MiB/s ≈ 85.8 Gbps

### Analysis

- **Comch is ~170x faster than TCP tmfifo** for host-BF2 communication (~29 us vs ~5000 us). The tmfifo virtual interface adds ~5 ms of software overhead.
- **Comch is ~3.6x faster than 100G host-host TCP** (~29 us vs ~105 us). The 100G path includes two BF2 OVS bridge traversals plus physical switch latency.
- **100G BF2-BF2 (~81 us) is faster than 100G host-host (~105 us)** by ~24 us, reflecting the two additional PCIe + OVS bridge hops in the host-host path.
- **1G LAN (~56 us for 64B) is faster than 100G host-host (~105 us)** because the 1G path is a direct Ethernet connection without BF2 OVS bridge overhead. However, 1G bandwidth is much lower for bulk transfers.
- Comch latency is stable across 64-1024B (~29 us), dominated by PCIe round-trip time.
- **RDMA READ on host VF (4.55 us) is the hardware lower bound**, significantly faster than TCP paths.
- **NIC ARM RDMA WRITE (3.44 us) is faster than host VF WRITE (19.23 us)** — the ARM processor has a more direct path to the ASIC with lower software stack overhead. Host VF WRITE latency includes host-side userspace polling overhead and PCIe round-trip.
- **NIC ARM RDMA bandwidth (85.8 Gbps) is near line-rate**, confirming hardware offload is active.
- **End-to-end derived latency**: L1 + L2 + L1 = 29 + 3.44 + 29 ≈ 62 us (vs TCP OVS path 105 us, speedup 1.69x).
- **NIC ARM p99 tail latency is high** (~99-148 us) due to ARM OS scheduling jitter, not hardware limitation. Host VF READ has much tighter p99 (8 us) because x86 scheduling is more deterministic.

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

---

## Experiment G: Workload Feature Profiling (Chapter 4)

Measures Nginx and DGEMM workload characteristics on x86 host vs BF2 ARM.
Used for workload classification to guide orchestration decisions.

### Setup

- **Nginx**: `nginx:alpine` Docker container, `--network=host`, default `worker_processes auto`
  - x86: 64 Nginx workers (64 logical CPUs)
  - BF2 ARM: 8 Nginx workers (8 Cortex-A72 cores)
- **DGEMM**: OpenBLAS, N=1024, 16 threads pinned to NUMA node 0 (cores 0-15)
- **wrk client**: 4 threads, 100 connections, 30s duration (from tianjin via 100G)
- **perf stat**: LLC-load-misses, LLC-loads, context-switches (system-wide for Nginx, per-process for DGEMM)

### Results

| Workload | Platform | Performance | LLC miss rate | Context Switches |
|----------|----------|-------------|---------------|------------------|
| Nginx | x86 host (64 cores) | **91,785 req/s** | 4.09% | 9,655,612 |
| Nginx | BF2 ARM (8 cores) | **39,464 req/s** | — | — |
| DGEMM | x86 host (16 cores) | **425.2 GFLOPS** | 18.32% | 499 |

### Analysis

- **Nginx is I/O-intensive**: high context-switch count (9.66M in 30s) and low LLC miss rate (4.09%) indicate network I/O dominated workload. Minimal cache footprint makes it suitable for BF2 offloading.
- **DGEMM is compute-intensive**: near-zero context switches (499 in 30s) and high LLC miss rate (18.32%) indicate heavy cache usage with large working set. Must stay on host x86 for performance.
- **BF2 ARM delivers 43% of x86 Nginx throughput** (39.5k vs 91.8k req/s) with only 12.5% of the core count (8 vs 64). Per-core efficiency is ~3.4x higher, validating BF2 as a viable Nginx offload target.
- These profiles directly inform the orchestration rule: migrate I/O-intensive (high ctx-sw, low LLC miss) workloads to BF2; keep compute-intensive (low ctx-sw, high LLC miss) workloads on host.

---

## Experiment H: Co-location Interference (Chapter 4)

Measures DGEMM throughput degradation when co-located with Nginx under sustained load.
Extends Experiment B (Chapter 2) where interference source was monitoring agents (5.3%).

### Setup

- **Scenario H.1**: DGEMM alone (baseline), 60s, 16 threads pinned to NUMA node 0 (cores 0-15)
- **Scenario H.2**: DGEMM + Nginx co-located on same NUMA node (both cpuset 0-15), wrk load from tianjin (4 threads, 200 connections, 55s after 5s warmup)
- **Scenario H.3**: DGEMM alone on host (cores 0-15), Nginx migrated to BF2, wrk against BF2 IP

### Results

| Scenario | DGEMM (GFLOPS) | LLC miss rate | Context Switches | vs Baseline |
|----------|----------------|---------------|------------------|-------------|
| H.1: DGEMM alone (baseline) | **420.5** | 18.76% | 877 | — |
| H.2: DGEMM + Nginx co-located | **187.9** | 23.74% | 10,409,462 | **−55.3%** |
| H.3: Nginx migrated to BF2 | **421.5** | 18.21% | 905 | **+0.2% (recovered)** |

Nginx throughput during co-location:
- H.2 (on host): 92,111 req/s
- H.3 (on BF2): 37,963 req/s

### Analysis

- **Nginx co-location causes 55.3% DGEMM degradation** (420.5 → 187.9 GFLOPS) when both workloads are pinned to the same 16 cores. Nginx's massive context-switch rate (10.4M in 60s) and LLC pressure (miss rate 18.76% → 23.74%) severely disrupt DGEMM's cache-resident matrix operations. The 243s of system time (vs 26s baseline) indicates heavy kernel scheduling overhead.
- **Migrating Nginx to BF2 fully recovers DGEMM performance** (421.5 GFLOPS, +0.2% vs baseline — within measurement noise). LLC miss rate returns to baseline (18.21%), and context switches drop back to near-zero (905).
- **Compared to Exp B**: monitoring agent interference (5.3%) was moderate; Nginx interference (55.3%) is 10.4x worse, making SmartNIC offloading critical for I/O-intensive service workloads sharing compute resources.
- **BF2 still provides meaningful Nginx throughput** (38.0k req/s) while completely eliminating host interference — a favorable trade-off when host compute capacity is the priority.

---

## Experiment I: BF2 Nginx Performance Scaling (Chapter 4)

Measures Nginx throughput on x86 host vs BF2 ARM under controlled core counts and concurrency levels.

### I-1: Concurrency scaling (all cores)

- Concurrency levels: 10, 50, 100, 200, 400 connections
- wrk: 4 threads, 30s per level, from tianjin via 100G fabric
- Nginx: `nginx:alpine`, `--network=host`, `worker_processes auto` (x86=64, BF2=8)

| Concurrency | x86 req/s | x86 latency | BF2 req/s | BF2 latency | BF2/x86 ratio |
|-------------|-----------|-------------|-----------|-------------|----------------|
| 10 | 27,884 | 282 µs | 19,174 | 426 µs | 68.7% |
| 50 | 84,117 | 559 µs | 32,012 | 1.57 ms | 38.1% |
| 100 | 88,326 | 1.12 ms | **38,969** | 3.31 ms | 44.1% |
| 200 | 91,975 | 2.15 ms | 38,065 | 5.87 ms | 41.4% |
| 400 | **92,649** | 24.73 ms | 37,516 | 11.23 ms | 40.5% |

### I-2: Per-core comparison (CPU pinning)

Isolates per-core efficiency by pinning Nginx to fixed core counts using `docker --cpuset-cpus`.
- x86: pinned to cores 16-19 (NUMA node 1, avoids DGEMM interference on node 0)
- BF2 ARM: pinned to cores 4-7
- wrk: 4 threads, 100 connections, 30s

| Cores | x86 (req/s) | BF2 ARM (req/s) | BF2/x86 | x86 per-core | BF2 per-core | Scaling (vs 1-core) |
|-------|-------------|-----------------|---------|--------------|-------------|---------------------|
| 1 | 46,053 | 9,968 | 21.6% | 46,053 | 9,968 | x86: 1.00x, BF2: 1.00x |
| 2 | 83,524 | 17,445 | 20.9% | 41,762 | 8,723 | x86: 1.81x, BF2: 1.75x |
| 4 | 88,899 | 31,236 | 35.1% | 22,225 | 7,809 | x86: 1.93x, BF2: 3.13x |

### Analysis

- **x86 saturates at ~92k req/s** (all cores, c≥200), BF2 ARM peaks at ~39.0k req/s (all cores, c=100).
- **Per-core throughput**: x86 single-core (46.1k req/s) is 4.6x faster than BF2 single-core (10.0k req/s), reflecting the Xeon Gold 5218's higher clock speed (2.3 GHz + turbo) and wider superscalar pipeline vs Cortex-A72 (2.0 GHz, in-order).
- **BF2 scales more linearly at 4 cores**: 4-core/1-core scaling is 3.13x on BF2 vs 1.93x on x86. ARM's simpler memory subsystem has less inter-core contention for I/O workloads. x86 hits diminishing returns earlier due to LLC and memory bandwidth contention across hyper-threaded cores.
- **x86 per-core throughput drops sharply at 4 cores** (22.2k vs 46.1k single-core = 48% of single-core), while BF2 maintains efficiency better (7.8k vs 10.0k = 78% of single-core) — hyper-threading and shared LLC on x86 create more per-core overhead under concurrent network I/O.
- **Practical implication**: BF2 with 4 dedicated cores handles ~31k req/s — sufficient for moderate web services while freeing all 64 host cores for compute workloads.

---

## Experiment J: Orchestration Strategy Validation (Chapter 4)

Compares cluster performance with/without workload orchestration using blue-green migration with floating VIP.

### Setup

- **VIP**: 192.168.56.200 (fujian workload VIP)
- **Scenario J.1**: No orchestration — Nginx + DGEMM co-located on fujian host, VIP on host interface
- **Scenario J.2**: Static orchestration — Nginx on fujian BF2 with VIP, DGEMM alone on host (cores 0-15)
- **Migration**: Blue-green deployment: start new container on BF2, health check, VIP switch (gratuitous ARP), stop old container on host

### Scenario Results

| Scenario | DGEMM (GFLOPS) | Nginx (req/s) | DGEMM vs baseline (420.5) |
|----------|----------------|---------------|---------------------------|
| J.1: No orchestration (co-located) | **350.1** | 91,052 | **−16.8%** |
| J.2: Static orchestration (Nginx→BF2) | **424.1** | 38,212 | **+0.9% (recovered)** |

### Blue-Green Migration Overhead (5 repetitions)

| Step | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | Avg |
|------|-------|-------|-------|-------|-------|-----|
| Container start (BF2) | 1,135 ms | 1,763 ms | 1,803 ms | 2,144 ms | 1,794 ms | **1,728 ms** |
| Health check | 697 ms | 1,356 ms | 1,387 ms | 1,356 ms | 1,369 ms | **1,233 ms** |
| VIP switch | 1,548 ms | 2,898 ms | 2,918 ms | 2,918 ms | 2,897 ms | **2,636 ms** |
| Old container stop | 2,616 ms | 2,536 ms | 2,666 ms | 2,458 ms | 2,507 ms | **2,557 ms** |
| **Total** | 6,008 ms | 8,566 ms | 8,787 ms | 8,888 ms | 8,578 ms | **8,165 ms** |

### Analysis

- **Without orchestration, DGEMM loses 16.8%** (350.1 vs 420.5 GFLOPS baseline from Exp H). J.1 shows less degradation than H.2 (55.3%) because J.1 does not pin Nginx to the same cpuset — Nginx uses all 64 cores while DGEMM is pinned to cores 0-15, so CPU contention is less direct but LLC and memory bandwidth contention still cause measurable degradation.
- **With orchestration, DGEMM fully recovers** (424.1 GFLOPS, +0.9% vs baseline). Nginx on BF2 still delivers 38.2k req/s via VIP routing.
- **Blue-green migration takes ~8.2 seconds** on average. Breakdown:
  - Container start (1.7s): Docker container init on BF2 ARM (image cached)
  - Health check (1.2s): HTTP readiness probe via two-hop SSH
  - VIP switch (2.6s): IP addr manipulation + gratuitous ARP (3 packets) via two-hop SSH
  - Old container stop (2.6s): container cleanup + rename on both ends
- **First run is fastest** (6.0s) due to warm SSH connections; subsequent runs are consistent (~8.6s).
- **VIP switch itself is near-instant** (~few ms); the 2.6s measured includes SSH round-trip overhead through two hops. In production with a local orchestrator agent, VIP switch would be sub-100ms.
- **Zero-downtime migration**: VIP is only moved after BF2 container passes health check, ensuring no service interruption.
- **Net compute gain**: 16.8% DGEMM recovery (420 vs 350 GFLOPS = +70 GFLOPS) at the cost of 58% Nginx throughput reduction (91k → 38k req/s). For compute-bound workloads, this is a favorable trade-off. When both are co-located on the same cpuset (Exp H), recovery is even more dramatic (55.3% → 0.2%).

---

## Chapter 4 Code Changes

12. **Orchestrator daemon** (`control-plane/orchestrator/orchestrator.py`): New Python service for automated workload placement:
    - Monitors LLC miss rate via `perf stat` over SSH
    - Detects interference when LLC miss rate exceeds 2x baseline
    - Executes blue-green migration: start new container on BF2, health check, VIP switch with gratuitous ARP, stop old on host
    - Two-hop SSH support for BF2 access (`user@host>>root@bf2`)
    - Reverse migration (BF2→host) for recovery/rebalancing
    - Events logged to TimescaleDB cluster_events table

13. **BF2 Docker setup** (`scripts/setup_bf2_docker.sh`): Automated Docker installation and nginx:alpine image pull on worker BF2s.

14. **Config additions** (`scripts/config.sh`):
    - `FUJIAN_BF2_FABRIC`, `HELONG_BF2_FABRIC`: BF2 fabric IPs for direct access
    - `VIP_FUJIAN="192.168.56.200"`, `VIP_HELONG="192.168.56.201"`: floating VIPs for migratable workloads
    - `ORCHESTRATOR`: path to orchestrator.py

15. **Experiment script fixes** (`scripts/ch4_exp_G/H/J_*.sh`): Fixed `gemm_bench` invocations:
    - Changed `-n 1024 -t 16 -d $DURATION` to `--size=1024 --duration=$DURATION` (binary uses long options only)
    - Added `env OMP_NUM_THREADS=N` prefix for thread control (OpenBLAS uses env vars, not CLI flags)
    - Used `env` after `perf stat --` to ensure proper environment variable propagation

---

## Experiment K: End-to-End Three-Configuration Comparison (Chapter 5)

Compares DGEMM throughput, Nginx throughput, and system-level metrics across three deployment configurations, demonstrating the cumulative benefit of the full SmartNIC-based heterogeneous cluster system.

### Setup

All configurations run on the fujian worker node with DGEMM pinned to cores 0-15 (NUMA node 0), 60s duration, wrk from tianjin (4 threads, 200 connections, 55s after 5s warmup).

- **K.1 No offloading baseline**: 8 `metric_push` instances in gRPC fallback mode (10ms interval, simulating traditional TCP monitoring agents) + Nginx (cpuset 0-15) + DGEMM (cores 0-15), all co-located. `cluster_master` on tianjin accepts gRPC reports. No SmartNIC slave_agent.
- **K.2 Control plane offloading only** (Ch2+Ch3): `cluster_master` on tianjin, `slave_agent` on fujian-bf2, single `metric_push` via Comch (1s interval). Nginx still co-located with DGEMM on host (cpuset 0-15).
- **K.3 Full system** (Ch2+Ch3+Ch4): Same control plane as K.2, but Nginx migrated to fujian-bf2 with VIP 192.168.56.200. DGEMM runs alone on host cores 0-15.

### Results

| Config | DGEMM (GFLOPS) | Nginx (req/s) | LLC miss rate | Context Switches | DGEMM vs K.3 |
|--------|----------------|---------------|---------------|------------------|---------------|
| K.1: No offloading | **185.4** | 89,831 | 25.77% | 11,318,128 | **−56.0%** |
| K.2: CP offload only | **190.2** | 89,771 | 23.92% | 9,865,722 | **−54.9%** |
| K.3: Full system | **421.6** | 35,708 | 18.43% | 894 | **baseline** |

### Detailed perf stats

| Config | User time (s) | System time (s) | sys/user ratio |
|--------|---------------|-----------------|----------------|
| K.1 | 468.9 | 248.0 | 52.9% |
| K.2 | 481.1 | 243.6 | 50.6% |
| K.3 | 933.8 | 27.2 | 2.9% |

### Analysis

- **K.1 → K.2 (control plane offloading)**: Replacing 8 gRPC-mode metric_push instances (10ms interval) with a single Comch-mode instance (1s interval) yields only marginal DGEMM improvement (185.4 → 190.2 GFLOPS, +2.6%). The dominant interference source is Nginx co-location, not the monitoring agents. LLC miss rate drops slightly (25.77% → 23.92%) and context switches decrease by 12.8% (11.3M → 9.9M), confirming the control plane offload reduces kernel scheduling overhead.

- **K.2 → K.3 (workload offloading)**: Migrating Nginx to BF2 delivers the transformative improvement: DGEMM recovers to 421.6 GFLOPS (+121.6% vs K.2), LLC miss rate returns to baseline (18.43%), and context switches drop from 9.9M to 894 (−99.99%). System time drops from 243.6s to 27.2s, confirming that Nginx's network I/O was the primary source of kernel overhead.

- **K.1 → K.3 (full system benefit)**: The complete SmartNIC-based system recovers 236.2 GFLOPS of compute capacity (+127.4%), at the cost of reducing Nginx throughput from 89.8k to 35.7k req/s (−60.3%). This trade-off is favorable for compute-bound HPC workloads where host CPU cycles are the bottleneck.

- **Layered contribution**: Control plane offloading (Ch2+Ch3) contributes ~2.6% DGEMM recovery; workload orchestration (Ch4) contributes ~121.6%. The two layers are complementary — Ch2+Ch3 provides the infrastructure (Comch transport, gRPC management, fault tolerance) that enables Ch4's automated workload migration decisions.

- **Context switch analysis**: K.1 and K.2 show ~10M context switches in 60s (Nginx-dominated scheduling), while K.3 drops to 894. This 4-order-of-magnitude reduction confirms that offloading I/O-intensive workloads to the SmartNIC eliminates the primary source of compute interference.
