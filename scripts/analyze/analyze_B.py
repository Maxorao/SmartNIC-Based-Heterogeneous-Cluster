#!/usr/bin/env python3
"""
Experiment B: Interference Elimination Analysis

Reads GFLOPS timeseries from three scenario files and computes:
  - T_base    (Scenario 1: gemm alone)
  - T_mixed   (Scenario 2: gemm + slave_monitor without offload)
  - T_offload (Scenario 3: gemm + slave_monitor with offload)
  - Interference rate  = (T_base − T_mixed)  / T_base × 100%
  - Recovery rate      = T_offload / T_base × 100%

Also parses perf stat output for LLC miss rates.

Input files (in ~/exp_data/B/):
  scenario{1,2,3}_gflops.txt   — one GFLOPS value per line (one per second)
  scenario{1,2,3}_perf.txt     — perf stat -I 1000 output
  scenario{1,2,3}_gflops.csv   — (optional) per-iteration detail from gemm_bench
"""

import os
import re
import sys
import numpy as np

DATA_DIR   = os.path.expanduser("~/exp_data/B")
SKIP_START = 5   # discard first N seconds (JIT warmup, process startup noise)
SKIP_END   = 5   # discard last N seconds


def load_gflops(filename: str) -> np.ndarray:
    path = os.path.join(DATA_DIR, filename)
    vals = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    vals.append(float(line.split(",")[0]))   # first column
                except ValueError:
                    pass
    except FileNotFoundError:
        print(f"  [WARN] {path} not found", file=sys.stderr)
        return np.array([])

    arr = np.array(vals)
    if len(arr) > SKIP_START + SKIP_END:
        return arr[SKIP_START : -SKIP_END]
    return arr


def parse_llc_miss_rate(filename: str) -> float:
    """
    Parse perf stat -I 1000 output.
    Returns LLC miss rate as a percentage (misses / loads * 100).
    """
    path = os.path.join(DATA_DIR, filename)
    total_misses = 0
    total_loads  = 0
    try:
        with open(path) as f:
            for line in f:
                # perf -I output format:
                #   <timestamp>  <count>  <event>  [<pct>]
                m_miss = re.search(r"([\d,]+)\s+LLC-load-misses", line)
                m_load = re.search(r"([\d,]+)\s+LLC-loads\b", line)
                if m_miss:
                    total_misses += int(m_miss.group(1).replace(",", ""))
                if m_load:
                    total_loads  += int(m_load.group(1).replace(",", ""))
    except FileNotFoundError:
        pass
    if total_loads == 0:
        return float("nan")
    return total_misses / total_loads * 100.0


def parse_ctx_switches(filename: str) -> float:
    """Return average context-switches per second from perf stat output."""
    path = os.path.join(DATA_DIR, filename)
    ctx_vals = []
    try:
        with open(path) as f:
            for line in f:
                m = re.search(r"([\d,]+)\s+context-switches", line)
                if m:
                    ctx_vals.append(int(m.group(1).replace(",", "")))
    except FileNotFoundError:
        pass
    return float(np.mean(ctx_vals)) if ctx_vals else float("nan")


def stats(arr: np.ndarray) -> dict:
    if len(arr) == 0:
        return {"mean": float("nan"), "std": float("nan"),
                "min": float("nan"), "max": float("nan")}
    return {
        "mean": float(np.mean(arr)),
        "std":  float(np.std(arr)),
        "min":  float(np.min(arr)),
        "max":  float(np.max(arr)),
    }


def analyze() -> None:
    s1 = load_gflops("scenario1_gflops.txt")
    s2 = load_gflops("scenario2_gflops.txt")
    s3 = load_gflops("scenario3_gflops.txt")

    st1, st2, st3 = stats(s1), stats(s2), stats(s3)

    llc1 = parse_llc_miss_rate("scenario1_perf.txt")
    llc2 = parse_llc_miss_rate("scenario2_perf.txt")
    llc3 = parse_llc_miss_rate("scenario3_perf.txt")

    ctx1 = parse_ctx_switches("scenario1_perf.txt")
    ctx2 = parse_ctx_switches("scenario2_perf.txt")
    ctx3 = parse_ctx_switches("scenario3_perf.txt")

    T_base    = st1["mean"]
    T_mixed   = st2["mean"]
    T_offload = st3["mean"]

    interference = (T_base - T_mixed)  / T_base * 100.0 if T_base else float("nan")
    recovery     = T_offload / T_base * 100.0 if T_base else float("nan")

    print("\n=== Experiment B: Interference Elimination ===")
    print(f"\n{'Scenario':<30} {'GFLOPS (mean±std)':>20} {'LLC miss%':>10} {'ctx-sw/s':>10}")
    print("-" * 75)
    print(f"{'1. Baseline (gemm only)':<30} "
          f"{st1['mean']:>8.3f} ± {st1['std']:.3f}    "
          f"{llc1:>9.2f}%  {ctx1:>9.1f}")
    print(f"{'2. +slave (direct, no offload)':<30} "
          f"{st2['mean']:>8.3f} ± {st2['std']:.3f}    "
          f"{llc2:>9.2f}%  {ctx2:>9.1f}")
    print(f"{'3. +slave (offload via SmartNIC)':<30} "
          f"{st3['mean']:>8.3f} ± {st3['std']:.3f}    "
          f"{llc3:>9.2f}%  {ctx3:>9.1f}")

    print()
    print(f"  Interference rate : {interference:.1f}%  "
          f"(T_base={T_base:.3f} → T_mixed={T_mixed:.3f} GFLOPS)")
    print(f"  Recovery rate     : {recovery:.1f}%  "
          f"(T_offload={T_offload:.3f} GFLOPS)")

    # Save for thesis
    out = os.path.join(DATA_DIR, "summary.txt")
    with open(out, "w") as f:
        f.write(f"T_base={T_base:.4f}\n")
        f.write(f"T_mixed={T_mixed:.4f}\n")
        f.write(f"T_offload={T_offload:.4f}\n")
        f.write(f"interference_pct={interference:.2f}\n")
        f.write(f"recovery_pct={recovery:.2f}\n")
        f.write(f"llc_miss_base={llc1:.4f}\n")
        f.write(f"llc_miss_mixed={llc2:.4f}\n")
        f.write(f"llc_miss_offload={llc3:.4f}\n")
    print(f"\nSummary saved → {out}")


if __name__ == "__main__":
    os.makedirs(DATA_DIR, exist_ok=True)
    analyze()
