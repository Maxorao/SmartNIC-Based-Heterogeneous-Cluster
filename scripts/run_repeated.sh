#!/bin/bash
# run_repeated.sh — Run an experiment script N times and aggregate summary.csv
#
# Usage:
#   bash run_repeated.sh <exp_script> <n_repeats> <output_csv> [run_label]
#
# The inner experiment script must:
#   1. Place a single-line summary CSV at ${DATA_DIR}/<exp_id>/summary.csv
#      (header line + one data line)
#   OR
#   2. Emit its summary via the convention:
#      ${DATA_DIR}/<summary_file_from_SUMMARY_CSV_env>
#
# This wrapper:
#   - Runs the script N times (serial)
#   - After each run, appends the latest summary.csv row to <output_csv>
#   - Adds columns: run_index, run_timestamp, run_label

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

if [ $# -lt 3 ]; then
    echo "Usage: $0 <exp_script> <n_repeats> <output_csv> [run_label]"
    echo "Example: $0 scripts/exp_B_interference.sh 10 ~/exp_data/B/repeats.csv baseline"
    exit 1
fi

EXP_SCRIPT="$1"
N_REPEATS="$2"
OUTPUT_CSV="$3"
LABEL="${4:-default}"

if [ ! -f "${EXP_SCRIPT}" ]; then
    echo "Error: experiment script ${EXP_SCRIPT} not found"
    exit 1
fi

# Derive expected summary file from experiment-specific convention.
# Each supported script should set SUMMARY_CSV internally; we export a default.
SUMMARY_CSV_DEFAULT="${DATA_DIR}/$(basename "${EXP_SCRIPT}" .sh)_summary.csv"

mkdir -p "$(dirname "${OUTPUT_CSV}")"

header_written=0
> "${OUTPUT_CSV}.tmp"

for run_idx in $(seq 1 "${N_REPEATS}"); do
    echo ""
    echo "=============================================="
    echo " Run ${run_idx}/${N_REPEATS} — ${EXP_SCRIPT}"
    echo " Label: ${LABEL}"
    echo "=============================================="

    run_ts=$(date -Iseconds)

    # Per-run summary path
    SUMMARY_CSV_RUN="${DATA_DIR}/repeats/$(basename "${EXP_SCRIPT}" .sh)_run${run_idx}.csv"
    mkdir -p "$(dirname "${SUMMARY_CSV_RUN}")"

    export SUMMARY_CSV="${SUMMARY_CSV_RUN}"
    export RUN_LABEL="${LABEL}"

    # Run experiment; tolerate failure to keep collecting
    if ! bash "${EXP_SCRIPT}"; then
        echo "  [WARN] Run ${run_idx} returned non-zero, continuing"
    fi

    if [ ! -f "${SUMMARY_CSV_RUN}" ]; then
        # Try default location
        if [ -f "${SUMMARY_CSV_DEFAULT}" ]; then
            cp "${SUMMARY_CSV_DEFAULT}" "${SUMMARY_CSV_RUN}"
        else
            echo "  [WARN] No summary.csv produced by run ${run_idx}; skipping"
            continue
        fi
    fi

    # Append to aggregated CSV
    if [ "${header_written}" -eq 0 ]; then
        # Write header with extra columns
        head -n 1 "${SUMMARY_CSV_RUN}" | \
            awk -v OFS=',' '{print "run_index","run_timestamp","run_label",$0}' \
            > "${OUTPUT_CSV}.tmp"
        header_written=1
    fi

    tail -n +2 "${SUMMARY_CSV_RUN}" | \
        awk -v idx="${run_idx}" -v ts="${run_ts}" -v lbl="${LABEL}" -v OFS=',' \
        '{print idx, ts, lbl, $0}' \
        >> "${OUTPUT_CSV}.tmp"

    echo "  [OK] Run ${run_idx} summary appended"
done

mv "${OUTPUT_CSV}.tmp" "${OUTPUT_CSV}"

echo ""
echo "=============================================="
echo " Aggregated ${N_REPEATS} runs"
echo " Output: ${OUTPUT_CSV}"
echo "=============================================="
head -n 1 "${OUTPUT_CSV}"
echo "..."
tail -n 3 "${OUTPUT_CSV}"
echo ""
echo "Run: python3 ${SCRIPT_DIR}/analyze/stats.py ${OUTPUT_CSV}"
