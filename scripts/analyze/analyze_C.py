#!/usr/bin/env python3
"""
Experiment C: Master-Monitor Scalability Analysis

Reads pidstat output for master_monitor at node counts [4, 16, 64, 256]
and produces:
  1. Console table: node count, avg CPU%, avg MEM MB, P99 latency
  2. summary.csv  — for LaTeX table / pgfplots

Input files (in ~/exp_data/C/):
  master_{N}nodes.txt   — pidstat -u -r output for master_monitor
  latency_{N}nodes.txt  — P99 latency lines from master_monitor log
"""

import os
import re
import sys
import numpy as np

DATA_DIR    = os.path.expanduser("~/exp_data/C")
NODE_COUNTS = [4, 16, 64, 256]


def parse_pidstat_cpu_mem(path: str) -> tuple[float, float]:
    """
    Parse pidstat -u -r output.
    Returns (avg_%CPU, avg_MEM_MB).
    pidstat columns (with -u):
      Time  UID  PID  %usr  %system  %guest  %wait  %CPU  CPU  Command
    pidstat columns (with -r, following the CPU block):
      Time  UID  PID  minflt/s  majflt/s  VSZ  RSS  %MEM  Command
    """
    cpus, mems_kb = [], []
    try:
        with open(path) as f:
            in_cpu_section = False
            for line in f:
                # pidstat prints a header line before each block
                if "%CPU" in line and "CPU" in line:
                    in_cpu_section = True
                    continue
                if "%MEM" in line and "RSS" in line:
                    in_cpu_section = False
                    continue
                if in_cpu_section:
                    parts = line.split()
                    # Last column is Command; %CPU is at index 7
                    if len(parts) >= 9 and parts[-1] == "master_monitor":
                        try:
                            cpus.append(float(parts[7]))
                        except (ValueError, IndexError):
                            pass
                else:
                    # Memory section: RSS (KB) at index 6
                    parts = line.split()
                    if len(parts) >= 8 and parts[-1] == "master_monitor":
                        try:
                            mems_kb.append(float(parts[6]))
                        except (ValueError, IndexError):
                            pass
    except FileNotFoundError:
        print(f"  [WARN] {path} not found", file=sys.stderr)

    avg_cpu = float(np.mean(cpus))  if cpus    else float("nan")
    avg_mem = float(np.mean(mems_kb)) / 1024.0 if mems_kb else float("nan")   # KB → MB
    return avg_cpu, avg_mem


def parse_p99_latency(path: str) -> float:
    """
    Extract P99 latency (ms) from master_monitor log lines like:
      [STATS] p99_latency=2.34ms  or  p99_latency: 2.34
    Returns the mean of all found values, or NaN.
    """
    vals = []
    try:
        with open(path) as f:
            for line in f:
                m = re.search(r"p99_latency[=:\s]+([\d.]+)", line, re.IGNORECASE)
                if m:
                    vals.append(float(m.group(1)))
    except FileNotFoundError:
        pass
    return float(np.mean(vals)) if vals else float("nan")


def analyze() -> None:
    results = []
    for n in NODE_COUNTS:
        cpu_path = os.path.join(DATA_DIR, f"master_{n}nodes.txt")
        lat_path = os.path.join(DATA_DIR, f"latency_{n}nodes.txt")

        avg_cpu, avg_mem = parse_pidstat_cpu_mem(cpu_path)
        p99_lat          = parse_p99_latency(lat_path)

        results.append({
            "nodes"    : n,
            "cpu_pct"  : avg_cpu,
            "mem_mb"   : avg_mem,
            "p99_ms"   : p99_lat,
        })

    print("\n=== Experiment C: Master-Monitor Scalability ===")
    print(f"{'Nodes':>8}  {'CPU (%)':>10}  {'MEM (MB)':>10}  {'P99 latency (ms)':>18}")
    print("-" * 54)
    for r in results:
        print(f"{r['nodes']:>8}  {r['cpu_pct']:>10.4f}  {r['mem_mb']:>10.2f}  {r['p99_ms']:>18.3f}")

    # Fit linear model for CPU vs nodes
    nodes = np.array([r["nodes"] for r in results if not np.isnan(r["cpu_pct"])])
    cpus  = np.array([r["cpu_pct"] for r in results if not np.isnan(r["cpu_pct"])])
    if len(nodes) >= 2:
        slope, intercept = np.polyfit(nodes, cpus, 1)
        print(f"\n  CPU linear fit: {slope*1000:.4f}‰ per node  (R² ≈ "
              f"{np.corrcoef(nodes, cpus)[0,1]**2:.4f})")

    # Save summary CSV
    import csv
    out = os.path.join(DATA_DIR, "summary.csv")
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["nodes", "cpu_pct", "mem_mb", "p99_ms"])
        w.writeheader()
        w.writerows(results)
    print(f"\nSummary saved → {out}")


if __name__ == "__main__":
    os.makedirs(DATA_DIR, exist_ok=True)
    analyze()
