#!/usr/bin/env python3
"""
analyze_I_tail.py — Parse wrk --latency output from Experiment I2 and emit:
  - CSV: platform, concurrency, rps, p50, p99, p99.9, p99.99 (ms)
  - Markdown table for thesis
"""
import os
import re
import sys

DATA_DIR = os.path.expanduser("~/exp_data/I_tail")

PLATFORMS = ["x86", "arm"]
CONCURRENCIES = [50, 100, 200, 400]


def parse_file(path: str) -> dict:
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        text = f.read()

    out = {"path": path}

    m = re.search(r"Requests/sec:\s+([\d.]+)", text)
    out["rps"] = float(m.group(1)) if m else float("nan")

    for p, key in [("50.00", "p50"), ("75.00", "p75"), ("90.00", "p90"),
                   ("95.00", "p95"), ("99.00", "p99"), ("99.90", "p99_9"),
                   ("99.99", "p99_99")]:
        m = re.search(rf"^pctl {p}\s+([\d.]+)ms", text, re.MULTILINE)
        out[key] = float(m.group(1)) if m else float("nan")

    # wrk's built-in latency distribution table (fallback)
    if "rps" not in out or out["rps"] != out["rps"]:  # NaN check
        # attempt secondary parse from Latency Distribution section
        pass
    return out


def fmt(v) -> str:
    if isinstance(v, float) and v != v:
        return "N/A"
    if isinstance(v, float):
        return f"{v:.2f}"
    return str(v)


def main():
    os.makedirs(DATA_DIR, exist_ok=True)

    rows = []
    for plat in PLATFORMS:
        for c in CONCURRENCIES:
            path = os.path.join(DATA_DIR, f"{plat}_c{c}.txt")
            data = parse_file(path)
            if not data:
                continue
            rows.append({
                "platform": plat, "concurrency": c,
                "rps": data.get("rps", float("nan")),
                "p50": data.get("p50", float("nan")),
                "p99": data.get("p99", float("nan")),
                "p99_9": data.get("p99_9", float("nan")),
                "p99_99": data.get("p99_99", float("nan")),
            })

    # CSV
    csv_path = os.path.join(DATA_DIR, "tail_latency.csv")
    with open(csv_path, "w") as f:
        f.write("platform,concurrency,rps,p50_ms,p99_ms,p99_9_ms,p99_99_ms\n")
        for r in rows:
            f.write(f"{r['platform']},{r['concurrency']},"
                    f"{fmt(r['rps'])},{fmt(r['p50'])},{fmt(r['p99'])},"
                    f"{fmt(r['p99_9'])},{fmt(r['p99_99'])}\n")
    print(f"CSV: {csv_path}")

    # Markdown
    md_path = os.path.join(DATA_DIR, "tail_latency.md")
    with open(md_path, "w") as f:
        f.write("| Platform | Concurrency | RPS | P50 (ms) | P99 (ms) | P99.9 (ms) | P99.99 (ms) |\n")
        f.write("|----------|------------:|----:|--------:|--------:|----------:|-----------:|\n")
        for r in rows:
            f.write(f"| {r['platform']} | {r['concurrency']} | "
                    f"{fmt(r['rps'])} | {fmt(r['p50'])} | "
                    f"{fmt(r['p99'])} | {fmt(r['p99_9'])} | {fmt(r['p99_99'])} |\n")
    print(f"Markdown: {md_path}")

    # Print quick summary to terminal
    print("\n=== Tail Latency Summary ===")
    print(f"{'Platform':<6} {'Conc':>5} {'RPS':>10} {'P50':>8} {'P99':>8} "
          f"{'P99.9':>8} {'P99.99':>8}")
    for r in rows:
        print(f"{r['platform']:<6} {r['concurrency']:>5} "
              f"{fmt(r['rps']):>10} {fmt(r['p50']):>8} {fmt(r['p99']):>8} "
              f"{fmt(r['p99_9']):>8} {fmt(r['p99_99']):>8}")


if __name__ == "__main__":
    main()
