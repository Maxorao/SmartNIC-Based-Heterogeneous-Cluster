#!/usr/bin/env python3
"""
emit_summary.py — Parse experiment output files and emit a one-row summary.csv.

Used by run_repeated.sh to collect a machine-readable summary per run.

Each experiment has its own parser:
  B — scenario1/2/3 GFLOPS + LLC + ctx-switches
  H — h1/h2/h3 GFLOPS + Nginx req/s + LLC + ctx-switches
  K — k1/k2/k3 GFLOPS + Nginx req/s + LLC + ctx-switches + cgroups k4

Usage:
    python3 emit_summary.py --exp B --data-dir ~/exp_data/B --out /tmp/summary.csv
"""

import argparse
import os
import re
import sys
import math
from typing import List


def mean(vs: List[float]) -> float:
    return sum(vs) / len(vs) if vs else float("nan")


def load_gflops(path: str, skip_start: int = 5, skip_end: int = 5) -> List[float]:
    try:
        with open(path) as f:
            vals = []
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    vals.append(float(line.split(",")[0]))
                except ValueError:
                    pass
            if len(vals) > skip_start + skip_end:
                return vals[skip_start:-skip_end] if skip_end > 0 else vals[skip_start:]
            return vals
    except FileNotFoundError:
        return []


def parse_gflops_from_text(path: str) -> float:
    """For files where gemm_bench prints totals like 'Average: N GFLOPS'."""
    try:
        with open(path) as f:
            content = f.read()
    except FileNotFoundError:
        return float("nan")
    m = re.search(r"Average[: ]+([\d.]+)\s*GFLOPS", content)
    if m:
        return float(m.group(1))
    # Fallback: average of per-iteration values in the file
    vals = load_gflops(path)
    return mean(vals) if vals else float("nan")


def parse_perf_llc(path: str) -> float:
    """Compute LLC miss rate % from perf stat output (sums misses/loads)."""
    total_misses = 0
    total_loads = 0
    try:
        with open(path) as f:
            for line in f:
                m_miss = re.search(r"([\d,]+)\s+LLC-load-misses", line)
                m_load = re.search(r"([\d,]+)\s+LLC-loads\b", line)
                if m_miss:
                    total_misses += int(m_miss.group(1).replace(",", ""))
                if m_load:
                    total_loads += int(m_load.group(1).replace(",", ""))
    except FileNotFoundError:
        return float("nan")
    if total_loads == 0:
        return float("nan")
    return total_misses / total_loads * 100.0


def parse_perf_ctx(path: str) -> float:
    """Total context-switches from perf stat output."""
    total = 0
    try:
        with open(path) as f:
            for line in f:
                m = re.search(r"([\d,]+)\s+context-switches", line)
                if m:
                    total += int(m.group(1).replace(",", ""))
    except FileNotFoundError:
        return float("nan")
    return float(total) if total > 0 else float("nan")


def parse_wrk_reqs(path: str) -> float:
    """Requests/sec from wrk output."""
    try:
        with open(path) as f:
            for line in f:
                m = re.search(r"Requests/sec:\s+([\d.]+)", line)
                if m:
                    return float(m.group(1))
    except FileNotFoundError:
        return float("nan")
    return float("nan")


def parse_wrk_latency(path: str, quantile: str = "99%") -> float:
    """Parse wrk --latency output for given quantile (e.g. '50%', '99%', '99.9%')."""
    try:
        with open(path) as f:
            content = f.read()
    except FileNotFoundError:
        return float("nan")
    # wrk latency output:
    # "     50%    1.50ms"  or  "   99.999%   100.00ms"
    pattern = rf"\s*{re.escape(quantile)}\s+([\d.]+)(ms|us|s)"
    m = re.search(pattern, content)
    if not m:
        return float("nan")
    value = float(m.group(1))
    unit = m.group(2)
    if unit == "s":
        value *= 1000
    elif unit == "us":
        value /= 1000
    return value  # in ms


# ---------------------------------------------------------------------------
# Per-experiment parsers
# ---------------------------------------------------------------------------

def summarize_B(data_dir: str) -> dict:
    s1 = mean(load_gflops(os.path.join(data_dir, "scenario1_gflops.txt")))
    s2 = mean(load_gflops(os.path.join(data_dir, "scenario2_gflops.txt")))
    s3 = mean(load_gflops(os.path.join(data_dir, "scenario3_gflops.txt")))
    llc1 = parse_perf_llc(os.path.join(data_dir, "scenario1_perf.txt"))
    llc2 = parse_perf_llc(os.path.join(data_dir, "scenario2_perf.txt"))
    llc3 = parse_perf_llc(os.path.join(data_dir, "scenario3_perf.txt"))
    ctx1 = parse_perf_ctx(os.path.join(data_dir, "scenario1_perf.txt"))
    ctx2 = parse_perf_ctx(os.path.join(data_dir, "scenario2_perf.txt"))
    ctx3 = parse_perf_ctx(os.path.join(data_dir, "scenario3_perf.txt"))
    interference = (s1 - s2) / s1 * 100 if s1 else float("nan")
    recovery_pct = s3 / s1 * 100 if s1 else float("nan")
    speedup = s3 / s2 if s2 else float("nan")
    return {
        "gflops_s1": s1, "gflops_s2": s2, "gflops_s3": s3,
        "llc_s1_pct": llc1, "llc_s2_pct": llc2, "llc_s3_pct": llc3,
        "ctx_s1": ctx1, "ctx_s2": ctx2, "ctx_s3": ctx3,
        "interference_pct": interference,
        "recovery_pct": recovery_pct,
        "speedup_offload": speedup,
    }


def summarize_H(data_dir: str) -> dict:
    g1 = parse_gflops_from_text(os.path.join(data_dir, "h1_gemm_alone.txt"))
    g2 = parse_gflops_from_text(os.path.join(data_dir, "h2_gemm_colocated.txt"))
    g3 = parse_gflops_from_text(os.path.join(data_dir, "h3_gemm_offloaded.txt"))
    rps2 = parse_wrk_reqs(os.path.join(data_dir, "h2_wrk.txt"))
    rps3 = parse_wrk_reqs(os.path.join(data_dir, "h3_wrk.txt"))
    llc1 = parse_perf_llc(os.path.join(data_dir, "h1_perf.txt"))
    llc2 = parse_perf_llc(os.path.join(data_dir, "h2_perf.txt"))
    llc3 = parse_perf_llc(os.path.join(data_dir, "h3_perf.txt"))
    ctx1 = parse_perf_ctx(os.path.join(data_dir, "h1_perf.txt"))
    ctx2 = parse_perf_ctx(os.path.join(data_dir, "h2_perf.txt"))
    ctx3 = parse_perf_ctx(os.path.join(data_dir, "h3_perf.txt"))
    interference = (g1 - g2) / g1 * 100 if g1 else float("nan")
    recovery_pct = g3 / g1 * 100 if g1 else float("nan")
    speedup_orch = g3 / g2 if g2 else float("nan")
    return {
        "gflops_h1": g1, "gflops_h2": g2, "gflops_h3": g3,
        "nginx_rps_h2": rps2, "nginx_rps_h3": rps3,
        "llc_h1_pct": llc1, "llc_h2_pct": llc2, "llc_h3_pct": llc3,
        "ctx_h1": ctx1, "ctx_h2": ctx2, "ctx_h3": ctx3,
        "interference_pct": interference,
        "recovery_pct": recovery_pct,
        "speedup_orch": speedup_orch,
    }


def summarize_K(data_dir: str) -> dict:
    g1 = parse_gflops_from_text(os.path.join(data_dir, "k1_gemm.txt"))
    g2 = parse_gflops_from_text(os.path.join(data_dir, "k2_gemm.txt"))
    g3 = parse_gflops_from_text(os.path.join(data_dir, "k3_gemm.txt"))
    g4 = parse_gflops_from_text(os.path.join(data_dir, "k4_gemm.txt"))  # cgroups (optional)
    rps1 = parse_wrk_reqs(os.path.join(data_dir, "k1_wrk.txt"))
    rps2 = parse_wrk_reqs(os.path.join(data_dir, "k2_wrk.txt"))
    rps3 = parse_wrk_reqs(os.path.join(data_dir, "k3_wrk.txt"))
    rps4 = parse_wrk_reqs(os.path.join(data_dir, "k4_wrk.txt"))
    llc1 = parse_perf_llc(os.path.join(data_dir, "k1_perf.txt"))
    llc2 = parse_perf_llc(os.path.join(data_dir, "k2_perf.txt"))
    llc3 = parse_perf_llc(os.path.join(data_dir, "k3_perf.txt"))
    llc4 = parse_perf_llc(os.path.join(data_dir, "k4_perf.txt"))
    ctx1 = parse_perf_ctx(os.path.join(data_dir, "k1_perf.txt"))
    ctx2 = parse_perf_ctx(os.path.join(data_dir, "k2_perf.txt"))
    ctx3 = parse_perf_ctx(os.path.join(data_dir, "k3_perf.txt"))
    ctx4 = parse_perf_ctx(os.path.join(data_dir, "k4_perf.txt"))
    speedup_e2e = g3 / g1 if g1 else float("nan")
    speedup_vs_cgroups = g3 / g4 if g4 and not math.isnan(g4) else float("nan")
    return {
        "gflops_k1": g1, "gflops_k2": g2, "gflops_k3": g3, "gflops_k4": g4,
        "nginx_rps_k1": rps1, "nginx_rps_k2": rps2,
        "nginx_rps_k3": rps3, "nginx_rps_k4": rps4,
        "llc_k1_pct": llc1, "llc_k2_pct": llc2, "llc_k3_pct": llc3, "llc_k4_pct": llc4,
        "ctx_k1": ctx1, "ctx_k2": ctx2, "ctx_k3": ctx3, "ctx_k4": ctx4,
        "speedup_e2e": speedup_e2e,
        "speedup_vs_cgroups": speedup_vs_cgroups,
    }


def fmt_value(v) -> str:
    if isinstance(v, float):
        if math.isnan(v):
            return ""
        return f"{v:.4f}"
    return str(v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exp", required=True, choices=["B", "H", "K"],
                    help="Experiment ID")
    ap.add_argument("--data-dir", required=True, help="Directory with experiment outputs")
    ap.add_argument("--out", required=True, help="Output CSV path (single row)")
    args = ap.parse_args()

    if args.exp == "B":
        summary = summarize_B(args.data_dir)
    elif args.exp == "H":
        summary = summarize_H(args.data_dir)
    elif args.exp == "K":
        summary = summarize_K(args.data_dir)
    else:
        print(f"Unknown exp: {args.exp}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        cols = list(summary.keys())
        f.write(",".join(cols) + "\n")
        f.write(",".join(fmt_value(summary[c]) for c in cols) + "\n")

    print(f"Summary written to {args.out}")
    for k, v in summary.items():
        print(f"  {k} = {fmt_value(v)}")


if __name__ == "__main__":
    main()
