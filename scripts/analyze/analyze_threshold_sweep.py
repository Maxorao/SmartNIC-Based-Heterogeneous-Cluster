#!/usr/bin/env python3
"""
analyze_threshold_sweep.py — FP/FN analysis for θ_llc sweep.

Ground-truth label mapping:
  no_interference     → actual_label = 0 (no interference)
  weak_interference   → actual_label = 0 (weak; algorithm should NOT trigger)
  heavy_interference  → actual_label = 1 (true interference)

Decisions:
  above_threshold column (0/1) = algorithm's prediction

For each θ, compute:
  TP = (actual=1) ∩ (predicted=1)
  FP = (actual=0) ∩ (predicted=1)
  TN = (actual=0) ∩ (predicted=0)
  FN = (actual=1) ∩ (predicted=0)

Then: precision, recall, F1, FPR (false positive rate) for threshold curve.

Input:  ~/exp_data/threshold_sweep/decisions_theta<θ>_<label>_r<N>.csv
Output: ~/exp_data/threshold_sweep/roc.csv + summary.md
"""

import csv
import glob
import os
import re
from collections import defaultdict

DATA_DIR = os.path.expanduser("~/exp_data/threshold_sweep")

LABEL_GT = {
    "no_interference":    0,
    "weak_interference":  0,   # weak: algorithm should NOT trigger
    "heavy_interference": 1,
}


def load_decisions(path: str):
    """Return list of (above_threshold: int, decision: str)."""
    rows = []
    try:
        with open(path) as f:
            rdr = csv.DictReader(f)
            for r in rdr:
                if r.get("decision") == "insufficient_window":
                    continue
                try:
                    above = int(r.get("above_threshold", 0))
                except ValueError:
                    above = 0
                rows.append((above, r.get("decision", "")))
    except FileNotFoundError:
        pass
    return rows


def main():
    # Group decisions by (theta, actual_label)
    stats = defaultdict(lambda: {"TP": 0, "FP": 0, "TN": 0, "FN": 0, "N": 0})

    pattern = os.path.join(DATA_DIR, "decisions_theta*_r*.csv")
    files = sorted(glob.glob(pattern))
    if not files:
        print(f"No decision files found in {DATA_DIR}")
        return

    for fp in files:
        name = os.path.basename(fp)
        # decisions_theta1.5_heavy_interference_r3.csv
        m = re.match(r"decisions_theta([\d.]+)_(\w+?)_r\d+\.csv$", name)
        if not m:
            continue
        theta = float(m.group(1))
        label = m.group(2)
        if label not in LABEL_GT:
            # tolerate multi-word label (e.g. "heavy_interference")
            parts = name.split("_")
            # fallback: extract label between theta and _r
            lm = re.search(r"theta[\d.]+_(.+?)_r\d+\.csv$", name)
            if lm and lm.group(1) in LABEL_GT:
                label = lm.group(1)
            else:
                continue
        gt = LABEL_GT[label]

        rows = load_decisions(fp)
        s = stats[theta]
        for above, _ in rows:
            s["N"] += 1
            if gt == 1 and above == 1:
                s["TP"] += 1
            elif gt == 1 and above == 0:
                s["FN"] += 1
            elif gt == 0 and above == 1:
                s["FP"] += 1
            else:
                s["TN"] += 1

    # Emit ROC CSV
    roc_csv = os.path.join(DATA_DIR, "roc.csv")
    with open(roc_csv, "w") as f:
        f.write("theta,N,TP,FP,TN,FN,precision,recall,f1,fpr\n")
        for theta in sorted(stats):
            s = stats[theta]
            tp, fp, tn, fn = s["TP"], s["FP"], s["TN"], s["FN"]
            precision = tp / (tp + fp) if (tp + fp) else 0.0
            recall = tp / (tp + fn) if (tp + fn) else 0.0
            f1 = 2 * precision * recall / (precision + recall) if (precision + recall) else 0.0
            fpr = fp / (fp + tn) if (fp + tn) else 0.0
            f.write(f"{theta},{s['N']},{tp},{fp},{tn},{fn},"
                    f"{precision:.3f},{recall:.3f},{f1:.3f},{fpr:.3f}\n")
    print(f"ROC CSV: {roc_csv}")

    # Markdown summary
    md = os.path.join(DATA_DIR, "summary.md")
    with open(md, "w") as f:
        f.write("# Threshold Sensitivity Sweep — Summary\n\n")
        f.write("| θ_llc | N | TP | FP | TN | FN | Precision | Recall | F1 | FPR |\n")
        f.write("|-----:|--:|---:|---:|---:|---:|----------:|-------:|---:|----:|\n")
        for theta in sorted(stats):
            s = stats[theta]
            tp, fp, tn, fn = s["TP"], s["FP"], s["TN"], s["FN"]
            precision = tp / (tp + fp) if (tp + fp) else 0.0
            recall = tp / (tp + fn) if (tp + fn) else 0.0
            f1 = 2 * precision * recall / (precision + recall) if (precision + recall) else 0.0
            fpr = fp / (fp + tn) if (fp + tn) else 0.0
            f.write(f"| {theta} | {s['N']} | {tp} | {fp} | {tn} | {fn} |"
                    f" {precision:.3f} | {recall:.3f} | {f1:.3f} | {fpr:.3f} |\n")
        f.write("\n*Ground truth labels: no/weak = 0, heavy = 1.*\n")
    print(f"Markdown: {md}")

    # Pretty print to terminal
    print("\n=== θ_llc sensitivity ===")
    print(f"{'θ':>5} {'N':>6} {'TP':>5} {'FP':>5} {'TN':>5} {'FN':>5} "
          f"{'Prec':>6} {'Recall':>7} {'F1':>6} {'FPR':>6}")
    for theta in sorted(stats):
        s = stats[theta]
        tp, fp, tn, fn = s["TP"], s["FP"], s["TN"], s["FN"]
        precision = tp / (tp + fp) if (tp + fp) else 0.0
        recall = tp / (tp + fn) if (tp + fn) else 0.0
        f1 = 2 * precision * recall / (precision + recall) if (precision + recall) else 0.0
        fpr = fp / (fp + tn) if (fp + tn) else 0.0
        print(f"{theta:>5.2f} {s['N']:>6} {tp:>5} {fp:>5} {tn:>5} {fn:>5} "
              f"{precision:>6.3f} {recall:>7.3f} {f1:>6.3f} {fpr:>6.3f}")


if __name__ == "__main__":
    main()
