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
| Comch | 64 | 10000 | 29.02 | 29.04 | 30.24 | 37.75 | 107.90 |
| Comch | 256 | 10000 | 29.05 | 29.03 | 30.24 | 35.08 | 81.77 |
| Comch | 1024 | 10000 | 29.29 | 29.30 | 30.43 | 36.86 | 95.49 |
| Comch | 4096 | - | FAILED | - | - | - | - |
| Comch | 65536 | - | FAILED | - | - | - | - |
| TCP | 64 | 10000 | 4999.67 | 4998.61 | 5059.59 | 5081.48 | 5091.78 |
| TCP | 256 | 10000 | 4999.68 | 5000.62 | 5059.55 | 5065.74 | 5071.61 |
| TCP | 1024 | 10000 | 4999.60 | 5000.70 | 5058.69 | 5064.41 | 5071.49 |
| TCP | 4096 | 10000 | 5001.40 | 4997.74 | 5321.77 | 19936.30 | 20073.25 |
| TCP | 65536 | 10000 | 4984.38 | 4997.37 | 5329.62 | 19934.04 | 24829.76 |

### Analysis

- **Comch is ~170x faster than TCP** for small messages (~29 us vs ~5000 us).
- Comch latency is remarkably stable across 64-1024B payloads (~29 us), indicating that PCIe round-trip overhead dominates over payload size in this range.
- **Comch fails at 4096B and above** due to the BF2 DOCA 1.5 hardware message size cap of 4080 bytes. Applications requiring larger messages would need application-level fragmentation or migration to BF3 with DOCA 3.x on both sides.
- TCP P99 latency spikes at 4096B+ (reaching ~20 ms), likely caused by TCP segmentation and buffering effects over the tmfifo_net0 virtual interface.
- The ~5 ms baseline TCP latency reflects the tmfifo_net0 software path overhead, not physical network distance.

---

## Experiment B: Interference Elimination

Measures whether a co-located control-plane process (slave_monitor at **10 ms** reporting interval, pinned to the **same cores** as GEMM) degrades GEMM compute throughput, and how much is recovered by offloading it to the BF2 ARM via Comch.

**GEMM and slave_monitor both pinned to cores 4-7, duration 60s per scenario, reporting interval 10ms (100 reports/s).**

### Results

| Scenario | GFLOPS (mean +/- std) | LLC miss% | ctx-sw/s |
|----------|----------------------|-----------|----------|
| 1. Baseline (GEMM only) | 149.033 +/- 0.602 | 15.62% | 3.7 |
| 2. +slave_monitor (direct TCP, same cores) | 149.118 +/- 0.626 | 16.08% | 3.7 |
| 3. +slave_monitor (Comch offload, same cores) | 146.524 +/- 0.627 | 14.37% | 22.9 |

**Derived metrics:**

- Interference rate: **-0.1%** (T_base=149.033 -> T_mixed=149.118 GFLOPS)
- Recovery rate: **98.3%** (T_offload=146.524 GFLOPS)

### Analysis

- Scenario 2 (direct TCP) shows **no measurable interference** (-0.1%), which is surprising given the aggressive 10ms interval and same-core pinning. This suggests that the kernel TCP stack on the loopback path (to master_monitor on the same host) is lightweight enough to avoid contention.
- Scenario 3 (Comch offload) shows a **1.7% GFLOPS reduction** (149.033 -> 146.524) and a significant increase in context switches (3.7 -> 22.9/s). The Comch path involves PCIe DMA operations that trigger more interrupts and context switches on the shared cores, despite bypassing the kernel TCP stack.
- LLC miss rate actually **decreased** in the offload scenario (14.37% vs 15.62%), indicating the GFLOPS reduction is driven by CPU scheduling overhead (context switches) rather than cache pollution.
- The results suggest that for this specific workload on tianjin, the TCP loopback path is more efficient than the PCIe Comch path when both are co-located on the same cores. The Comch offload benefit would be more pronounced in a multi-host scenario where the control-plane traffic traverses a real network.

---

## Build Notes

Several source fixes were required to compile the codebase on tianjin:

1. **Missing `#include <stdbool.h>`** in `comch_host_doca31.c` and `comch_nic_doca15.c`.
2. **Dead legacy code in `comch_host.c`** (old DOCA 1.x API functions after the version dispatcher `#include`) conflicted with the DOCA 3.1 implementation. Removed.
3. **API migration**: `slave_monitor.c`, `forward_routine.c`, `bench_host.c`, and `bench_nic.c` all referenced the old `comch_host.h`/`comch_nic.h` headers with stack-allocated contexts. Updated to use the new `comch_api.h` pointer-based interface.
4. **DOCA 1.5 naming differences on BF2**: `doca_devinfo_rep_create_list` -> `doca_devinfo_rep_list_create`, `doca_error_get_descr` -> `doca_get_error_string`.
5. **Missing includes**: `<stdarg.h>`, `<math.h>` in `slave_monitor.c`; `<sys/socket.h>` in `protocol.h`; `<inttypes.h>` in `db.c`; `-I/usr/include/postgresql` for `libpq-fe.h`.
6. **PCI address format**: Host-side DOCA 3.1 requires full BDF format `0000:5e:00.0` (not `5e:00.0`).
7. **Representor filter fallback on BF2**: After BF2 reboot, `DOCA_DEV_REP_FILTER_NET` is not supported; added fallback to `DOCA_DEV_REP_FILTER_ALL` in `comch_nic_doca15.c`.
8. **Host PCI rescan required**: After BF2 reboot, the host must rescan PCI (`echo 1 > /sys/bus/pci/devices/0000:5e:00.0/remove && echo 1 > /sys/bus/pci/rescan`) for DOCA to rediscover the BF2 device.
