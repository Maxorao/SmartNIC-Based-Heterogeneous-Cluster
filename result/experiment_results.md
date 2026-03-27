# Experiment Results — Tianjin (2026-03-27)

**Host:** tianjin, Ubuntu 22.04, x86, 172.28.4.75
**Host DOCA:** 3.1.0
**BF2 (ARM):** DOCA 1.5.4003, Ubuntu 20.04
**BF2 PCI (host view):** 0000:5e:00.0
**BF2 PCI (ARM view):** 03:00.0
**Connection:** tmfifo_net0 (192.168.100.1 <-> 192.168.100.2)

---

## Experiment A: Tunnel Latency

Measures one-way latency (RTT/2) of DOCA Comch (PCIe kernel-bypass) vs kernel TCP over tmfifo_net0.
Each configuration runs 10,000 ping-pong iterations after a 200-iteration warmup.

### Results

| Protocol | Size (B) | n | Avg (us) | P50 (us) | P99 (us) | P99.9 (us) | Max (us) |
|----------|----------|-------|----------|----------|----------|------------|----------|
| Comch | 64 | 10000 | 29.07 | 29.07 | 31.20 | 50.60 | 121.16 |
| Comch | 256 | 10000 | 29.31 | 29.04 | 31.49 | 76.11 | 1403.24 |
| Comch | 1024 | 10000 | 29.27 | 29.27 | 30.95 | 42.04 | 198.40 |
| Comch | 4096 | - | FAILED | - | - | - | - |
| Comch | 65536 | - | FAILED | - | - | - | - |
| TCP | 64 | 10000 | 4999.69 | 4999.71 | 5014.17 | 5038.46 | 5047.89 |
| TCP | 256 | 10000 | 4999.70 | 4999.68 | 5015.00 | 5040.83 | 5069.23 |
| TCP | 1024 | 10000 | 4999.62 | 4999.60 | 5015.13 | 5033.97 | 5050.81 |
| TCP | 4096 | 10000 | 5247.95 | 4999.56 | 19940.05 | 20020.14 | 29944.76 |
| TCP | 65536 | 10000 | 5305.95 | 4999.63 | 19948.54 | 24945.35 | 49882.20 |

### Analysis

- **Comch is ~170x faster than TCP** for small messages (~29 us vs ~5000 us).
- Comch latency is remarkably stable across 64-1024B payloads (~29 us), indicating that PCIe round-trip overhead dominates over payload size in this range.
- **Comch fails at 4096B and above** due to the BF2 DOCA 1.5 hardware message size cap of 4080 bytes. Applications requiring larger messages would need application-level fragmentation or migration to BF3 with DOCA 3.x on both sides.
- TCP P99 latency spikes at 4096B+ (reaching ~20 ms), likely caused by TCP segmentation and buffering effects over the tmfifo_net0 virtual interface.
- The ~5 ms baseline TCP latency reflects the tmfifo_net0 software path overhead, not physical network distance.

---

## Experiment B: Interference Elimination

Measures whether a co-located control-plane process (slave_monitor at 100 ms reporting interval) degrades GEMM compute throughput, and how much is recovered by offloading it to the BF2 ARM via Comch.

**GEMM pinned to cores 4-7, duration 60s per scenario.**

### Results

| Scenario | GFLOPS (mean +/- std) | LLC miss% | ctx-sw/s |
|----------|----------------------|-----------|----------|
| 1. Baseline (GEMM only) | 146.180 +/- 0.650 | 15.94% | 8.2 |
| 2. +slave_monitor (direct TCP) | 146.349 +/- 0.613 | 15.38% | 8.3 |
| 3. +slave_monitor (Comch offload) | 146.286 +/- 0.600 | 15.83% | 7.9 |

**Derived metrics:**

- Interference rate: **-0.1%** (T_base=146.180 -> T_mixed=146.349 GFLOPS)
- Recovery rate: **100.1%** (T_offload=146.286 GFLOPS)

### Analysis

- On tianjin, interference from slave_monitor is **negligible** across all three scenarios. All GFLOPS values fall within the noise margin (~0.6 std).
- This is expected because tianjin has sufficient CPU cores: GEMM is pinned to cores 4-7, while slave_monitor runs on a different core, avoiding direct contention for compute resources.
- The offload path shows marginally fewer context switches (7.9 vs 8.3/s), consistent with reduced kernel TCP stack activity on the host.
- LLC miss rates are similar across scenarios (~15-16%), confirming that the control-plane traffic at 100 ms intervals does not measurably pollute the last-level cache.
- **To observe meaningful interference**, future experiments could: (a) co-locate both workloads on overlapping cores via cgroup/cpuset, (b) increase the reporting frequency (e.g., 10 ms or 1 ms intervals), or (c) run on a machine with fewer cores where resource contention is unavoidable.

---

## Build Notes

Several source fixes were required to compile the codebase on tianjin:

1. **Missing `#include <stdbool.h>`** in `comch_host_doca31.c` and `comch_nic_doca15.c`.
2. **Dead legacy code in `comch_host.c`** (old DOCA 1.x API functions after the version dispatcher `#include`) conflicted with the DOCA 3.1 implementation. Removed.
3. **API migration**: `slave_monitor.c`, `forward_routine.c`, `bench_host.c`, and `bench_nic.c` all referenced the old `comch_host.h`/`comch_nic.h` headers with stack-allocated contexts. Updated to use the new `comch_api.h` pointer-based interface.
4. **DOCA 1.5 naming differences on BF2**: `doca_devinfo_rep_create_list` -> `doca_devinfo_rep_list_create`, `doca_error_get_descr` -> `doca_get_error_string`.
5. **Missing includes**: `<stdarg.h>`, `<math.h>` in `slave_monitor.c`; `<sys/socket.h>` in `protocol.h`; `<inttypes.h>` in `db.c`; `-I/usr/include/postgresql` for `libpq-fe.h`.
6. **PCI address format**: Host-side DOCA 3.1 requires full BDF format `0000:5e:00.0` (not `5e:00.0`).
