#!/usr/bin/env python3
"""
Experiment C: Control-Plane Scalability Analysis

Reads pidstat + mock_slave output for node counts [4, 16, 64, 256]
and produces:
  1. Console table: nodes, CPU%, RSS MB, avg latency, error rate
  2. summary.csv for LaTeX / pgfplots

Input files (in ~/exp_data/C/):
  pidstat_{N}nodes.txt          — pidstat -u -r output for master_monitor
  mock_{N}nodes_{ip}.txt        — mock_slave output per worker host
"""

import os
import re
import sys
import glob
import csv
import numpy as np

DATA_DIR    = os.path.expanduser("~/exp_data/C")
NODE_COUNTS = [4, 16, 64, 256]


def parse_pidstat(path: str):
    """Parse pidstat -u -r output. Returns (avg_cpu%, avg_rss_mb)."""
    cpus, rss_kb = [], []
    try:
        with open(path) as f:
            for line in f:
                parts = line.split()
                if len(parts) < 8 or parts[-1] != "master_monitor":
                    continue
                # CPU section: col 7 is %CPU
                try:
                    val = float(parts[7])
                    # Distinguish CPU vs memory by checking if col 6 looks like RSS (large int)
                    # In CPU section: col 7 = %CPU (0-100)
                    # In memory section: col 6 = RSS (KB, typically >1000)
                    if float(parts[6]) > 500:
                        # This is memory section — col 6 = RSS
                        rss_kb.append(float(parts[6]))
                    else:
                        cpus.append(val)
                except (ValueError, IndexError):
                    pass
    except FileNotFoundError:
        print(f"  [WARN] {path} not found", file=sys.stderr)

    avg_cpu = float(np.mean(cpus)) if cpus else float("nan")
    avg_rss = float(np.mean(rss_kb)) / 1024.0 if rss_kb else float("nan")
    return avg_cpu, avg_rss


def parse_mock_outputs(n: int):
    """
    Parse all mock_{N}nodes_*.txt files.
    Returns (total_sent, total_acked, total_errors, avg_latency_us).
    """
    pattern = os.path.join(DATA_DIR, f"mock_{n}nodes_*.txt")
    files = glob.glob(pattern)

    total_sent = total_acked = total_errors = 0
    total_lat = 0.0
    lat_count = 0

    for f in files:
        try:
            with open(f) as fh:
                for line in fh:
                    # Look for: TOTAL: sent=1234  acked=1234  errors=0  avg_lat=123.4 us
                    m = re.search(
                        r"TOTAL:\s+sent=(\d+)\s+acked=(\d+)\s+errors=(\d+)\s+avg_lat=([\d.]+)",
                        line
                    )
                    if m:
                        s, a, e, lat = int(m[1]), int(m[2]), int(m[3]), float(m[4])
                        total_sent += s
                        total_acked += a
                        total_errors += e
                        if a > 0:
                            total_lat += lat * a
                            lat_count += a
        except FileNotFoundError:
            pass

    avg_lat = total_lat / lat_count if lat_count > 0 else float("nan")
    return total_sent, total_acked, total_errors, avg_lat


def analyze():
    results = []
    for n in NODE_COUNTS:
        pidstat_path = os.path.join(DATA_DIR, f"pidstat_{n}nodes.txt")
        avg_cpu, avg_rss = parse_pidstat(pidstat_path)
        sent, acked, errors, avg_lat = parse_mock_outputs(n)
        err_rate = (errors / sent * 100) if sent > 0 else 0.0
        reports_per_sec = acked / 30.0 if acked > 0 else 0.0  # 30s measurement

        results.append({
            "nodes": n,
            "cpu_pct": avg_cpu,
            "rss_mb": avg_rss,
            "avg_lat_us": avg_lat,
            "reports_sec": reports_per_sec,
            "sent": sent,
            "acked": acked,
            "errors": errors,
            "err_rate": err_rate,
        })

    # Print table
    print("\n=== Experiment C: Control-Plane Scalability ===\n")
    hdr = f"{'Nodes':>6}  {'CPU%':>7}  {'RSS MB':>8}  {'Avg Lat (us)':>13}  {'Reports/s':>10}  {'Errors':>7}  {'Err%':>6}"
    print(hdr)
    print("-" * len(hdr))
    for r in results:
        print(f"{r['nodes']:>6}  {r['cpu_pct']:>7.2f}  {r['rss_mb']:>8.1f}  "
              f"{r['avg_lat_us']:>13.1f}  {r['reports_sec']:>10.1f}  "
              f"{r['errors']:>7}  {r['err_rate']:>5.1f}%")

    # Linear fit for CPU vs nodes
    nodes = np.array([r["nodes"] for r in results if not np.isnan(r["cpu_pct"])])
    cpus  = np.array([r["cpu_pct"] for r in results if not np.isnan(r["cpu_pct"])])
    if len(nodes) >= 2:
        slope, intercept = np.polyfit(nodes, cpus, 1)
        r2 = np.corrcoef(nodes, cpus)[0, 1] ** 2
        print(f"\n  CPU linear fit: {slope:.4f}% per node + {intercept:.2f}%  (R²={r2:.4f})")
        print(f"  Projected CPU at 1000 nodes: {slope * 1000 + intercept:.1f}%")

    # Save CSV
    out = os.path.join(DATA_DIR, "summary.csv")
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=[
            "nodes", "cpu_pct", "rss_mb", "avg_lat_us", "reports_sec",
            "sent", "acked", "errors", "err_rate"
        ])
        w.writeheader()
        w.writerows(results)
    print(f"\n  Summary saved → {out}")


if __name__ == "__main__":
    os.makedirs(DATA_DIR, exist_ok=True)
    analyze()
