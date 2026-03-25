#!/bin/bash
# Experiment B: Control Plane Interference Elimination Effect
# Tests GEMM throughput in three scenarios:
#   Scenario 1 (baseline):  gemm_bench alone
#   Scenario 2 (no offload): gemm_bench + slave_monitor in direct TCP mode (100ms interval)
#   Scenario 3 (offload):   gemm_bench + slave_monitor in DOCA Comch offload mode (100ms interval)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION=60        # seconds per scenario
WARMUP_SECS=5      # seconds to discard at start/end

echo "=== Experiment B: Interference Elimination ==="
echo "Duration per scenario: ${DURATION}s | Compute cores: ${COMPUTE_CORES}"
echo "Output dir: ${DATA_DIR}/B"
echo ""

# ── Helper: wait for a PID, kill it, wait for perf to flush ──────────────────
cleanup_pids() {
    for pid in "$@"; do
        kill "$pid" 2>/dev/null || true
    done
    sleep 1
}

run_scenario() {
    local scenario_id="$1"
    local description="$2"

    echo "--- Scenario ${scenario_id}: ${description} ---"

    # Start gemm_bench bound to compute cores
    taskset -c "${COMPUTE_CORES}" \
        "${GEMM_BENCH}" --duration="${DURATION}" \
        --output="${DATA_DIR}/B/scenario${scenario_id}_gflops.csv" \
        > "${DATA_DIR}/B/scenario${scenario_id}_gflops.txt" 2>/dev/null &
    GEMM_PID=$!

    sleep 1   # let gemm start before attaching perf

    # Monitor gemm_bench with perf stat, writing hardware counters every second
    perf stat -p "${GEMM_PID}" \
        -e LLC-load-misses,LLC-loads,context-switches,instructions \
        -I 1000 \
        --output "${DATA_DIR}/B/scenario${scenario_id}_perf.txt" \
        sleep "${DURATION}" &
    PERF_PID=$!

    wait "${GEMM_PID}" 2>/dev/null || true
    cleanup_pids "${PERF_PID}"

    echo "  Done. GFLOPS data: ${DATA_DIR}/B/scenario${scenario_id}_gflops.txt"
    sleep 5   # let system stabilize before next scenario
}

# ── Scenario 1: Baseline (gemm only) ─────────────────────────────────────────
run_scenario 1 "gemm_bench alone (baseline)"

# ── Scenario 2: No offload — slave_monitor runs on host via TCP ──────────────
echo "--- Scenario 2: gemm + slave_monitor (direct TCP, no offload) ---"

"${SLAVE_MONITOR}" \
    --mode=direct \
    --master-ip="${MASTER_IP}" \
    --master-port="${MASTER_PORT}" \
    --interval="${HIGH_LOAD_INTERVAL}" \
    --node-id="exp-node-$(hostname)" \
    > "${DATA_DIR}/B/scenario2_slave.log" 2>&1 &
SLAVE_PID=$!
sleep 2   # wait for slave to register

run_scenario 2 "gemm + slave_monitor direct (${HIGH_LOAD_INTERVAL}ms interval)"
cleanup_pids "${SLAVE_PID}"
sleep 5

# ── Scenario 3: Offload — slave_monitor routes via SmartNIC ──────────────────
echo "--- Scenario 3: gemm + slave_monitor (DOCA Comch offload) ---"
echo "  (Ensure forward_routine is running on ${GNODE2_BF_IP})"

"${SLAVE_MONITOR}" \
    --mode=offload \
    --pci="${GNODE2_PCI}" \
    --interval="${HIGH_LOAD_INTERVAL}" \
    --node-id="exp-node-$(hostname)" \
    > "${DATA_DIR}/B/scenario3_slave.log" 2>&1 &
SLAVE_PID=$!
sleep 2

run_scenario 3 "gemm + slave_monitor offload (${HIGH_LOAD_INTERVAL}ms interval)"
cleanup_pids "${SLAVE_PID}"

# ── Print quick summary ───────────────────────────────────────────────────────
echo ""
echo "=== Quick Summary ==="
for s in 1 2 3; do
    f="${DATA_DIR}/B/scenario${s}_gflops.txt"
    if [ -f "$f" ]; then
        # Skip warmup lines, compute mean
        avg=$(awk "NR>${WARMUP_SECS} && NR<=NF-${WARMUP_SECS} {s+=\$1; n++} END{printf \"%.3f\", s/n}" "$f")
        echo "  Scenario ${s}: avg GFLOPS = ${avg}"
    fi
done
echo ""
echo "Run: python3 ${SCRIPT_DIR}/analyze/analyze_B.py"
