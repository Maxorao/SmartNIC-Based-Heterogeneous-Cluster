#!/bin/bash
# ch4_exp_K_threshold_sweep.sh — Threshold sensitivity sweep for θ_llc
#
# Runs the orchestrator in dry-run mode at 5 θ_llc factors × 3 interference
# patterns, recording every decision to analyse false-positive / false-negative
# rates against the ground-truth label.
#
# Interference patterns (labels = ground truth for interference present?):
#   - "no_interference"   : DGEMM alone, no Nginx     (label: FALSE)
#   - "weak_interference" : DGEMM + Nginx @ 10 conc   (label: MAYBE)
#   - "heavy_interference": DGEMM + Nginx @ 200 conc  (label: TRUE)
#
# θ_llc factors tested: 1.2, 1.5, 2.0, 2.5, 3.0
#
# Output: ~/exp_data/threshold_sweep/
#   decisions_<theta>_<label>.csv  (per run)
#   roc.csv                         (aggregated TP/FP/TN/FN)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

OUT_DIR="${DATA_DIR}/threshold_sweep"
mkdir -p "${OUT_DIR}"

# Parameters
DURATION_PER_RUN=300   # 5 minutes per (theta × label)
POLL_INTERVAL=5
WINDOW_SIZE=6
RUN_REPEATS=3

THETAS="1.2 1.5 2.0 2.5 3.0"
LABELS="no_interference weak_interference heavy_interference"

# ---------------------------------------------------------------------------
# Workload injection helpers
# ---------------------------------------------------------------------------
start_interference() {
    local label="$1"
    case "${label}" in
        no_interference)
            # Only DGEMM on host cores 0-15
            ssh "${USER}@${FUJIAN_IP}" "nohup env OMP_NUM_THREADS=${GEMM_THREADS} \
                taskset -c 0-15 ${GEMM_BENCH} --size=1024 --duration=9999 \
                > /tmp/dgemm_bg.log 2>&1 &"
            ;;
        weak_interference)
            # DGEMM + Nginx at low concurrency
            ssh "${USER}@${FUJIAN_IP}" "docker rm -f nginx-bench 2>/dev/null; \
                docker run -d --name nginx-bench --network=host \
                --cpuset-cpus=0-15 nginx:alpine"
            ssh "${USER}@${FUJIAN_IP}" "nohup env OMP_NUM_THREADS=${GEMM_THREADS} \
                taskset -c 0-15 ${GEMM_BENCH} --size=1024 --duration=9999 \
                > /tmp/dgemm_bg.log 2>&1 &"
            # launch wrk in background at low concurrency
            nohup wrk -t2 -c10 -d"${DURATION_PER_RUN}s" \
                "http://${FUJIAN_100G}/" > /tmp/wrk_weak.log 2>&1 &
            echo $! > /tmp/wrk_bg.pid
            ;;
        heavy_interference)
            # DGEMM + Nginx at high concurrency
            ssh "${USER}@${FUJIAN_IP}" "docker rm -f nginx-bench 2>/dev/null; \
                docker run -d --name nginx-bench --network=host \
                --cpuset-cpus=0-15 nginx:alpine"
            ssh "${USER}@${FUJIAN_IP}" "nohup env OMP_NUM_THREADS=${GEMM_THREADS} \
                taskset -c 0-15 ${GEMM_BENCH} --size=1024 --duration=9999 \
                > /tmp/dgemm_bg.log 2>&1 &"
            nohup wrk -t4 -c200 -d"${DURATION_PER_RUN}s" \
                "http://${FUJIAN_100G}/" > /tmp/wrk_heavy.log 2>&1 &
            echo $! > /tmp/wrk_bg.pid
            ;;
    esac
    sleep 3  # let workloads reach steady state
}

stop_interference() {
    ssh "${USER}@${FUJIAN_IP}" "pkill -f gemm_bench 2>/dev/null; \
        docker rm -f nginx-bench 2>/dev/null" 2>/dev/null || true
    if [ -f /tmp/wrk_bg.pid ]; then
        kill "$(cat /tmp/wrk_bg.pid)" 2>/dev/null || true
        rm -f /tmp/wrk_bg.pid
    fi
    sleep 3
}

cleanup_all() {
    stop_interference
    pkill -f orchestrator.py 2>/dev/null || true
}
trap cleanup_all EXIT INT TERM

# ---------------------------------------------------------------------------
# Main sweep
# ---------------------------------------------------------------------------
for label in ${LABELS}; do
    for theta in ${THETAS}; do
        for repeat in $(seq 1 ${RUN_REPEATS}); do
            echo ""
            echo "=============================================="
            echo " Run: label=${label}  theta=${theta}  repeat=${repeat}/${RUN_REPEATS}"
            echo "=============================================="

            DEC_LOG="${OUT_DIR}/decisions_theta${theta}_${label}_r${repeat}.csv"
            rm -f "${DEC_LOG}"

            # Start workload injection
            start_interference "${label}"

            # Launch orchestrator in dry-run mode
            python3 "${ORCHESTRATOR}" \
                --llc-threshold "${theta}" \
                --poll-interval "${POLL_INTERVAL}" \
                --window-size "${WINDOW_SIZE}" \
                --decision-log "${DEC_LOG}" \
                --dry-run \
                --log-level INFO \
                > "${OUT_DIR}/orch_${theta}_${label}_r${repeat}.log" 2>&1 &
            ORCH_PID=$!

            sleep "${DURATION_PER_RUN}"

            # Stop orchestrator
            kill ${ORCH_PID} 2>/dev/null || true
            wait ${ORCH_PID} 2>/dev/null || true

            stop_interference

            if [ -f "${DEC_LOG}" ]; then
                n=$(wc -l < "${DEC_LOG}")
                echo "  [OK] ${n} decisions recorded in ${DEC_LOG}"
            else
                echo "  [WARN] No decision log produced"
            fi
        done
    done
done

echo ""
echo "=== Threshold sweep complete ==="
echo "Analyse results: python3 ${SCRIPT_DIR}/analyze/analyze_threshold_sweep.py"
