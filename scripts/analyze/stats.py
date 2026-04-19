#!/usr/bin/env python3
"""
stats.py — Aggregate statistics across repeated experiment runs.

Input: a CSV file produced by run_repeated.sh with columns:
    run_index, run_timestamp, run_label, <experiment-specific columns>

Output:
    - Per-metric summary: N, mean, stddev, min, max, 95% CI (t-based)
    - Optional pairwise Welch's t-test between labels (via --compare-labels)
    - Optional per-scenario breakdown (via --group-by)

Usage:
    python3 stats.py <repeats.csv>
    python3 stats.py <repeats.csv> --metric gflops
    python3 stats.py <repeats.csv> --metric gflops --group-by scenario
    python3 stats.py <repeats.csv> --compare-labels baseline offload
"""

import argparse
import csv
import math
import sys
from collections import defaultdict
from typing import List, Dict


def load_csv(path: str) -> List[Dict[str, str]]:
    with open(path, "r") as f:
        rdr = csv.DictReader(f)
        return list(rdr)


def is_numeric_col(rows: List[Dict[str, str]], col: str) -> bool:
    for r in rows:
        v = r.get(col, "").strip()
        if not v:
            continue
        try:
            float(v)
            return True
        except ValueError:
            return False
    return False


def summarise(values: List[float]) -> Dict[str, float]:
    n = len(values)
    if n == 0:
        return {"n": 0}
    mean = sum(values) / n
    if n > 1:
        var = sum((x - mean) ** 2 for x in values) / (n - 1)
        stddev = math.sqrt(var)
        # 95% CI using t-distribution (approximate via z for n>=30, else Welch)
        t_95 = {
            2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776, 6: 2.571, 7: 2.447,
            8: 2.365, 9: 2.306, 10: 2.262, 15: 2.145, 20: 2.093, 30: 2.042,
        }
        t_val = t_95.get(n, 1.96 if n >= 30 else 2.042)
        se = stddev / math.sqrt(n)
        ci = t_val * se
    else:
        stddev = 0.0
        ci = 0.0
    return {
        "n": n,
        "mean": mean,
        "stddev": stddev,
        "min": min(values),
        "max": max(values),
        "ci95": ci,
        "cv_pct": (stddev / mean * 100) if mean != 0 else 0.0,
    }


def welch_t_test(a: List[float], b: List[float]) -> Dict[str, float]:
    """Returns Welch's t-statistic and a rough two-tailed p-value (approx)."""
    na, nb = len(a), len(b)
    if na < 2 or nb < 2:
        return {"t": float("nan"), "p_approx": float("nan"), "df": 0}
    ma = sum(a) / na
    mb = sum(b) / nb
    va = sum((x - ma) ** 2 for x in a) / (na - 1)
    vb = sum((x - mb) ** 2 for x in b) / (nb - 1)
    if va == 0 and vb == 0:
        return {"t": float("inf") if ma != mb else 0.0, "p_approx": 0.0, "df": na + nb - 2}
    se = math.sqrt(va / na + vb / nb)
    if se == 0:
        return {"t": float("inf"), "p_approx": 0.0, "df": na + nb - 2}
    t = (ma - mb) / se
    # Welch-Satterthwaite degrees of freedom
    num = (va / na + vb / nb) ** 2
    den = (va / na) ** 2 / (na - 1) + (vb / nb) ** 2 / (nb - 1)
    df = num / den if den > 0 else na + nb - 2
    # Rough two-tailed p-value approximation for moderate df
    # Use survival function of t-distribution — simple approximation:
    # For |t| large, p ~ 2 * exp(-t^2 / 2) is a very loose upper bound.
    # For presentation we just report |t| and df; user can compute exact p.
    abs_t = abs(t)
    # very rough two-tailed p for df>=10: use normal approximation for large df
    if df >= 10:
        # Abramowitz & Stegun approx for Q(x) = 1 - Phi(x)
        x = abs_t
        p_one = 0.5 * math.erfc(x / math.sqrt(2))
        p = 2 * p_one
    else:
        # crude: if t > 2.5 treat as p<0.05
        p = 0.05 if abs_t > 2.5 else 0.2
    return {"t": t, "p_approx": p, "df": df}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv_path")
    ap.add_argument("--metric", default=None,
                    help="Specific numeric column to analyse (default: all numeric)")
    ap.add_argument("--group-by", default=None,
                    help="Categorical column to group on (e.g. 'scenario')")
    ap.add_argument("--compare-labels", nargs=2, metavar=("A", "B"),
                    default=None, help="Pairwise t-test between two run_labels")
    args = ap.parse_args()

    rows = load_csv(args.csv_path)
    if not rows:
        print("No data rows found", file=sys.stderr)
        sys.exit(1)

    cols = rows[0].keys()

    # Determine metrics to analyse
    if args.metric:
        metrics = [args.metric]
    else:
        skip = {"run_index", "run_timestamp", "run_label"}
        metrics = [c for c in cols if c not in skip and is_numeric_col(rows, c)]

    group_col = args.group_by

    print(f"\n{'='*70}")
    print(f" Statistics from {args.csv_path}")
    print(f" Total runs: {len(rows)}")
    print(f"{'='*70}\n")

    for metric in metrics:
        if group_col and group_col in cols:
            groups = defaultdict(list)
            for r in rows:
                g = r.get(group_col, "")
                v = r.get(metric, "").strip()
                if v:
                    try:
                        groups[g].append(float(v))
                    except ValueError:
                        pass
            print(f"--- Metric: {metric} (grouped by {group_col}) ---")
            print(f"  {'Group':<20} {'N':>4} {'Mean':>12} {'Stddev':>10} "
                  f"{'CV%':>7} {'CI95':>10} {'Min':>10} {'Max':>10}")
            for g, vs in sorted(groups.items()):
                s = summarise(vs)
                if s["n"] == 0:
                    continue
                print(f"  {g:<20} {s['n']:>4} {s['mean']:>12.3f} "
                      f"{s['stddev']:>10.3f} {s['cv_pct']:>6.2f}% "
                      f"{s['ci95']:>10.3f} {s['min']:>10.3f} {s['max']:>10.3f}")
            print()
        else:
            values = []
            for r in rows:
                v = r.get(metric, "").strip()
                if v:
                    try:
                        values.append(float(v))
                    except ValueError:
                        pass
            if not values:
                continue
            s = summarise(values)
            print(f"--- Metric: {metric} ---")
            print(f"  N      = {s['n']}")
            print(f"  Mean   = {s['mean']:.3f}")
            print(f"  Stddev = {s['stddev']:.3f}  (CV = {s['cv_pct']:.2f}%)")
            print(f"  95% CI = ±{s['ci95']:.3f}")
            print(f"  Range  = [{s['min']:.3f}, {s['max']:.3f}]")
            print()

    if args.compare_labels:
        la, lb = args.compare_labels
        print(f"--- Pairwise t-test: '{la}' vs '{lb}' ---")
        for metric in metrics:
            a = [float(r[metric]) for r in rows
                 if r.get("run_label") == la and r.get(metric, "").strip()]
            b = [float(r[metric]) for r in rows
                 if r.get("run_label") == lb and r.get(metric, "").strip()]
            if len(a) < 2 or len(b) < 2:
                continue
            tt = welch_t_test(a, b)
            sa = summarise(a)
            sb = summarise(b)
            sig = "*" if tt["p_approx"] < 0.05 else " "
            print(f"  {metric}: {la} mean={sa['mean']:.3f}±{sa['stddev']:.3f} "
                  f"vs {lb} mean={sb['mean']:.3f}±{sb['stddev']:.3f}  "
                  f"t={tt['t']:.2f} df={tt['df']:.1f} "
                  f"p~{tt['p_approx']:.4f} {sig}")
        print()


if __name__ == "__main__":
    main()
