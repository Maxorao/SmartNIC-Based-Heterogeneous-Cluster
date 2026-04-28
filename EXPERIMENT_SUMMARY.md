# Experiment Summary — April 2026

All experiments run on the 3-node cluster: tianjin (master), fujian (worker), helong (worker).

## Phase 1: Cluster Verification
- gRPC control plane verified (cluster_master ↔ slave_agent)
- Comch PCIe path verified (host ↔ BF2)
- Metric push via both gRPC fallback and Comch modes

## Phase 2: Experiment A2 — End-to-End Comch→RDMA→Comch Latency
- Measured full 7-layer round-trip: host→Comch→BF2→RDMA→BF2→Comch→host→(ponger)→back
- 4 message sizes (64/128/256/1024B), 10000 iterations each
- **One-way latency: 64.4–66.0 μs** (validates hierarchical kernel-bypass tunnel)
- Results: `~/exp_data/A_e2e/summary.md`

## Phase 3: Repeated Experiments B, H, K (10× each)

### Experiment B — Control-Plane Offload (n=10)
DGEMM + simulated kubelet stack (metric_push + 4 agent simulators at 10Hz)

| Metric | S1 (baseline) | S2 (no offload) | S3 (full offload) |
|--------|--------------|-----------------|-------------------|
| GFLOPS | 412.88 | 388.67 | 412.83 |
| LLC miss-rate | 18.4% | 19.3% | 18.6% |
| Context switches | 476 | 6081 | 674 |

- **Recovery: 99.99%** — Comch offload eliminates monitoring interference
- **Speedup: 1.063×** — marginal but statistically significant (p<0.001)
- Summary: `~/exp_data/B_repeats_v3_summary.md`

### Experiment H — Data-Plane Co-location (n=10)
DGEMM + Nginx on same cores vs BF2 offload

| Metric | H.1 (baseline) | H.2 (co-located) | H.3 (BF2 offload) |
|--------|---------------|------------------|-------------------|
| GFLOPS | 413.97 | 50.75 | 413.45 |
| Nginx req/s | — | 175,770 | 7,572 |
| LLC miss-rate | 18.4% | 26.9% | 18.3% |
| Context switches | 513 | 128M | 475 |

- **Interference: 87.74%** — co-located Nginx collapses DGEMM to 12% of baseline
- **Recovery: 99.88%** — BF2 offload fully recovers DGEMM
- **Speedup: 8.16×** — largest in thesis
- Summary: `~/exp_data/H_repeats_summary.md`

### Experiment K — End-to-End Three-Configuration (n=10)
Full stack: no offload vs control-plane-only vs full system offload

| Metric | K.1 (no offload) | K.2 (ctrl-plane) | K.3 (full system) |
|--------|-----------------|-----------------|-------------------|
| GFLOPS | 50.44 | 49.88 | 412.99 |
| Nginx req/s | 176,552 | 176,722 | 7,503 |
| LLC miss-rate | 26.3% | 26.6% | 18.1% |
| Context switches | 129M | 129M | 668 |

- **K.1 vs K.2: p=0.271 (not significant)** — control-plane offload alone has no effect when data-plane is co-located
- **Speedup K.3/K.1: 8.19×** — full offload required for meaningful recovery
- Summary: `~/exp_data/K_repeats_summary.md`

## Phase 4: Supplementary Experiments

### K.4 — cgroups v2 Exclusive Cpuset Baseline
DGEMM (cores 0-15) + Nginx (cores 16-31) with cgroups v2 CPU isolation

| Metric | K.1 (pathological) | K.4 (cgroups) | K.3 (full offload) |
|--------|-------------------|---------------|---------------------|
| GFLOPS | 50.44 | 394.84 | 412.99 |
| Nginx req/s | 176,552 | 151,935 | 7,503 |
| LLC miss-rate | 26.3% | 18.5% | 18.1% |
| Context switches | 129M | 1,527 | 668 |

- **K.4 recovery: 95.6%** of full offload, 7.83× vs K.1
- cgroups v2 is a viable alternative to BF2 offload when data-plane stays on host

### I2 — Nginx Tail Latency (x86 vs ARM)
wrk --latency at 4 concurrency levels (50/100/200/400)

| Platform | Max RPS | P50 (c200) | P99 (c200) | P99 (c400) |
|----------|---------|------------|------------|------------|
| x86 (fujian host) | ~246k | 0.55ms | 8.97ms | 9.87ms |
| ARM (fujian BF2) | ~7.6k | 25.2ms | 38.8ms | 351.6ms |

- BF2 ARM serves ~3% of x86 throughput; tail latency degrades severely under load
- Constrained by tmfifo PCIe bandwidth + ARM core performance

### J2 — Migration via Local orchestrator_agent
Blue-green migration (host → BF2) using gRPC agent instead of two-hop SSH

| Run | Container | Health | VIP | Total |
|-----|-----------|--------|-----|-------|
| 1 (cold) | 1802ms | 2380ms | 2910ms | 10216ms |
| 2–5 (warm) | ~1822ms | ~2372ms | ~1598ms | ~8516ms |

- Stable ~8.5s migration time (excluding cold start)
- orchestrator_agent confirmed functional

### Threshold Sweep — θ_llc Sensitivity
5 thresholds × 3 interference patterns, cores 48-63 (NUMA node1)

| θ | no_int (FP) | heavy_int (TP) | Verdict |
|---|------------|---------------|---------|
| 1.2 | 0% | 100% | **PERFECT** |
| 1.5 | 0% | 100% | PERFECT |
| 2.0 | 0% | 0% | Misses interference |
| 2.5 | 0% | 0% | Misses interference |
| 3.0 | 0% | 0% | Misses interference |

- **Recommended: θ = 1.2**
- NUMA node choice is critical: node0 showed anti-correlation (LLC dropped during interference)
- 3 bugs fixed in orchestrator.py (sudo perf, -C per-CPU, --llc-baseline honoured)

## Phase 5: RDMA Control Plane Integration Test
- `rdma_bridge_slave` / `rdma_bridge_master` built on both BF2s
- Full hot-path verified: metric_push → Comch → slave_agent → UDS → rdma_bridge_slave → RDMA → rdma_bridge_master → gRPC → cluster_master
- **91 messages forwarded, 0 drops, 0 errors**
- RDMA CM handshake: 3rd attempt (2 REJECTED + success)

## Bug Fixes (orchestrator.py)
1. `perf` → `sudo perf` (perf stat -a requires root)
2. `-a` (all CPUs) → `-C 48-63` (measure compute cores only)
3. `calibrate_baseline()` no longer overwrites `--llc-baseline` value

## 100G Fabric Restoration
- Host VF IPs restored: tianjin=192.168.56.10, fujian=192.168.56.11
- BF2 fabric IPs verified: tianjin=192.168.56.2, fujian=192.168.56.3
- wrk from tianjin → fujian 100G now functional

---

## Key Takeaways

1. **BF2 offload fully recovers compute throughput** (99.9% recovery, 8.2× speedup) when data-plane workloads are co-located
2. **Control-plane offload alone is negligible** when data-plane interference dominates (K.1=K.2)
3. **cgroups v2 achieves 95.6% of BF2 offload benefit** — viable software-only alternative
4. **LLC miss-rate can detect interference** but requires clean-socket placement and conservative θ (~1.2)
5. **RDMA hot-path for control plane works** end-to-end with zero errors
6. **ARM BF2 throughput is severely limited** (~3% of x86) — BF2s should run control-plane, not data-plane
