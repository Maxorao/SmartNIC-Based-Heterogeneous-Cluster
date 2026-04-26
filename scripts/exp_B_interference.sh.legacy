#!/bin/bash
# Experiment B: Control Plane Interference Elimination Effect
# Adapted for tianjin single-host + BF2 setup.
#
# Tests GEMM throughput in three scenarios:
#   Scenario 1 (baseline):   gemm_bench alone
#   Scenario 2 (no offload): gemm_bench + slave_monitor in direct TCP mode (100ms interval)
#   Scenario 3 (offload):    gemm_bench + slave_monitor in DOCA Comch offload mode (100ms interval)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION="${GEMM_DURATION:-60}"
WARMUP_SECS=5

echo "=== Experiment B: Interference Elimination ==="
echo "Duration per scenario: ${DURATION}s | Compute cores: ${COMPUTE_CORES}"
echo "Output dir: ${DATA_DIR}/B"
echo ""

# ── Helper: kill PIDs ─────────────────────────────────────────────────────────
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

    # Monitor gemm_bench with perf stat
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

# ── Start master_monitor in background (needed for scenarios 2 & 3) ──────────
echo "[exp_B] Starting master_monitor on port ${MASTER_PORT}..."
"${MASTER_MONITOR}" --port="${MASTER_PORT}" \
    > "${DATA_DIR}/B/master.log" 2>&1 &
MASTER_PID=$!
sleep 2

# ── Scenario 1: Baseline (gemm only) ─────────────────────────────────────────
run_scenario 1 "gemm_bench alone (baseline)"

# ── Scenario 2: No offload — slave_monitor runs on host via TCP ──────────────
echo "--- Scenario 2: gemm + slave_monitor (direct TCP, no offload) ---"

# Pin slave_monitor to the same cores as GEMM to force CPU and cache competition
taskset -c "${COMPUTE_CORES}" "${SLAVE_MONITOR}" \
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

# Start forward_routine on BF2
echo "  Starting forward_routine on BF2..."
ssh root@${BF_IP} "pkill -f forward_routine 2>/dev/null || true" || true
sleep 1
ssh root@${BF_IP} \
    "nohup ${NIC_FORWARD_ROUTINE} --pci=${NIC_PCI} \
     --master-ip=${MASTER_IP} --master-port=${MASTER_PORT} \
     > /tmp/forward_routine.log 2>&1 &"
sleep 3

# Also pin to compute cores — fair comparison: offload path should cause less
# interference even when competing on the same cores, because Comch sends are
# PCIe DMA (no kernel TCP stack, no blocking syscalls)
sudo taskset -c "${COMPUTE_CORES}" "${SLAVE_MONITOR}" \
    --mode=offload \
    --pci="${HOST_PCI}" \
    --interval="${HIGH_LOAD_INTERVAL}" \
    --node-id="exp-node-$(hostname)" \
    > "${DATA_DIR}/B/scenario3_slave.log" 2>&1 &
SLAVE_PID=$!
sleep 2

run_scenario 3 "gemm + slave_monitor offload (${HIGH_LOAD_INTERVAL}ms interval)"
cleanup_pids "${SLAVE_PID}"

# Cleanup forward_routine and master
ssh root@${BF_IP} "pkill -f forward_routine 2>/dev/null || true" || true
cleanup_pids "${MASTER_PID}"

# ── Print quick summary ───────────────────────────────────────────────────────
echo ""
echo "=== Quick Summary ==="
for s in 1 2 3; do
    f="${DATA_DIR}/B/scenario${s}_gflops.txt"
    if [ -f "$f" ]; then
        avg=$(awk "NR>${WARMUP_SECS} && NR<=(NR-${WARMUP_SECS}) {s+=\$1; n++} END{if(n>0) printf \"%.3f\", s/n; else print \"N/A\"}" "$f" 2>/dev/null || echo "N/A")
        echo "  Scenario ${s}: avg GFLOPS = ${avg}"
    fi
done

# ── Emit machine-readable summary for run_repeated.sh ─────────────────────────
SUMMARY_CSV="${SUMMARY_CSV:-${DATA_DIR}/B/summary.csv}"
python3 "${SCRIPT_DIR}/analyze/emit_summary.py" \
    --exp B --data-dir "${DATA_DIR}/B" --out "${SUMMARY_CSV}" || true

echo ""
echo "Run: python3 ${SCRIPT_DIR}/analyze/analyze_B.py"
