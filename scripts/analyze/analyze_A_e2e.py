#!/usr/bin/env python3
"""
analyze_A_e2e.py — Analyse end-to-end Comch→RDMA→Comch latency.

Input: ~/exp_data/A_e2e/e2e_size<N>.csv (one row per iteration, rtt_ns column)
Output:
  - summary CSV: msg_size, n, rtt_min/mean/p50/p95/p99/p99.9/max, one-way versions
  - markdown table for thesis
"""
import argparse
import csv
import glob
import os
import statistics


def summarise(vals):
    if not vals:
        return None
    vals = sorted(vals)
    n = len(vals)
    return {
        "n": n,
        "min": vals[0],
        "mean": sum(vals) / n,
        "p50": vals[n // 2],
        "p95": vals[int(n * 0.95)],
        "p99": vals[int(n * 0.99)],
        "p999": vals[min(int(n * 0.999), n - 1)],
        "max": vals[-1],
        "stddev": statistics.pstdev(vals) if n > 1 else 0.0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default=os.path.expanduser("~/exp_data/A_e2e"))
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.data_dir, "e2e_size*.csv")))
    if not files:
        print(f"No files matching e2e_size*.csv in {args.data_dir}")
        return

    results = []
    for fp in files:
        # derive msg_size from filename
        name = os.path.basename(fp)
        try:
            size = int(name.replace("e2e_size", "").replace(".csv", ""))
        except ValueError:
            continue

        rtts = []
        with open(fp) as f:
            rdr = csv.DictReader(f)
            for row in rdr:
                try:
                    rtts.append(int(row["rtt_ns"]))
                except (KeyError, ValueError):
                    pass

        s = summarise(rtts)
        if not s:
            continue
        results.append({"size": size, **s})

    results.sort(key=lambda r: r["size"])

    # CSV summary
    out_csv = os.path.join(args.data_dir, "summary.csv")
    with open(out_csv, "w") as f:
        f.write("msg_size,n,rtt_min_ns,rtt_mean_ns,rtt_p50_ns,rtt_p95_ns,"
                "rtt_p99_ns,rtt_p999_ns,rtt_max_ns,rtt_stddev_ns,"
                "oneway_mean_us,oneway_p50_us,oneway_p99_us\n")
        for r in results:
            f.write(f"{r['size']},{r['n']},{r['min']},{r['mean']:.0f},"
                    f"{r['p50']},{r['p95']},{r['p99']},{r['p999']},{r['max']},"
                    f"{r['stddev']:.0f},"
                    f"{r['mean']/2000:.2f},{r['p50']/2000:.2f},"
                    f"{r['p99']/2000:.2f}\n")

    # Pretty print
    print(f"\n=== E2E Latency: Comch → RDMA → Comch → Comch → RDMA → Comch (RTT) ===")
    print(f"{'Size':>5}  {'N':>6}  {'Min':>7}  {'Mean':>7}  {'P50':>7}  "
          f"{'P99':>7}  {'Max':>8}  {'1way_mean':>10}  {'1way_p99':>9}")
    for r in results:
        print(f"{r['size']:>5}  {r['n']:>6}  "
              f"{r['min']/1000:>6.2f}μ  {r['mean']/1000:>6.2f}μ  "
              f"{r['p50']/1000:>6.2f}μ  {r['p99']/1000:>6.2f}μ  "
              f"{r['max']/1000:>7.1f}μ  "
              f"{r['mean']/2000:>9.2f}μ  {r['p99']/2000:>8.2f}μ")

    # Markdown
    md_path = os.path.join(args.data_dir, "summary.md")
    with open(md_path, "w") as f:
        f.write("# End-to-end Comch–RDMA–Comch Latency\n\n")
        f.write("Round-trip traverses: host→Comch→BF2→RDMA→BF2→Comch→host (ponger) "
                "and back the same path.\n\n")
        f.write("| Size (B) | N | RTT Mean (μs) | RTT P50 (μs) | RTT P99 (μs) | "
                "One-way Mean (μs) | One-way P99 (μs) |\n")
        f.write("|---:|---:|---:|---:|---:|---:|---:|\n")
        for r in results:
            f.write(f"| {r['size']} | {r['n']} | "
                    f"{r['mean']/1000:.2f} | {r['p50']/1000:.2f} | "
                    f"{r['p99']/1000:.2f} | "
                    f"{r['mean']/2000:.2f} | {r['p99']/2000:.2f} |\n")
    print(f"\nMarkdown: {md_path}")
    print(f"CSV:      {out_csv}")


if __name__ == "__main__":
    main()
