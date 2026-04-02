#!/bin/bash
# ch4_interference.sh — Experiment G: Workload co-location interference
#
# Measures DGEMM throughput and LLC miss rate under three scenarios:
#   Scenario 1: DGEMM alone (baseline, can reuse Exp B data)
#   Scenario 2: DGEMM + Nginx on same NUMA node (interference)
#   Scenario 3: Nginx migrated to BF2, DGEMM alone (recovery)
#
# Compare with Experiment B (Ch.2): interference source = monitoring agents (5.3%)
# This experiment: interference source = Nginx (expected > 5.3%)

set -e
source "$(dirname "$0")/config.sh"

WORKER="${FUJIAN_IP}"
WORKER_100G="${FUJIAN_100G}"
BF2_100G="${FUJIAN_BF2_FABRIC}"
BF2_SSH="root@192.168.100.2"
DURATION=60
WRK_THREADS=4
WRK_CONNS=200
DATA="${DATA_DIR}/ch4_G"
mkdir -p "${DATA}"

echo "============================================"
echo "  Experiment G: Workload Co-location Interference"
echo "============================================"

# Helper: run DGEMM with perf stat
run_gemm_with_perf() {
    local scenario=$1
    local label=$2
    echo ""
    echo "--- Scenario ${scenario}: ${label} ---"

    ssh "$(whoami)@${WORKER}" "
        sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
            -o /tmp/perf_scenario${scenario}.txt \
            numactl --cpunodebind=0 --membind=0 \
            env OPENBLAS_NUM_THREADS=16 \
            ~/experiments/bench/gemm_bench/gemm_bench --duration=${DURATION} \
            > /tmp/gemm_scenario${scenario}.txt 2>&1
    "
    scp "$(whoami)@${WORKER}:/tmp/gemm_scenario${scenario}.txt" "${DATA}/"
    scp "$(whoami)@${WORKER}:/tmp/perf_scenario${scenario}.txt" "${DATA}/"

    echo "GFLOPS:"
    tail -3 "${DATA}/gemm_scenario${scenario}.txt"
    echo "Perf:"
    grep -E "LLC|context" "${DATA}/perf_scenario${scenario}.txt" || true
}

# -------------------------------------------------------------------
# Scenario 1: DGEMM baseline (no co-location)
# -------------------------------------------------------------------
ssh "$(whoami)@${WORKER}" "docker rm -f nginx-exp 2>/dev/null" || true
run_gemm_with_perf 1 "DGEMM baseline (no co-location)"

# -------------------------------------------------------------------
# Scenario 2: DGEMM + Nginx on same host (interference)
# -------------------------------------------------------------------
echo ""
echo "Starting Nginx on host (same NUMA node as DGEMM)..."
ssh "$(whoami)@${WORKER}" "
    docker rm -f nginx-exp 2>/dev/null
    docker run -d --name nginx-exp --network=host \
        --cpuset-cpus=0-15 \
        nginx:alpine
    sleep 3
    echo 'Nginx running on host'
"

# Start wrk in background (sustained load during DGEMM measurement)
echo "Starting wrk load generator..."
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${DURATION}s \
    "http://${WORKER_100G}/" > "${DATA}/wrk_scenario2.txt" 2>&1 &
WRK_PID=$!

run_gemm_with_perf 2 "DGEMM + Nginx co-located"

wait ${WRK_PID} 2>/dev/null
echo "Nginx wrk results:"
cat "${DATA}/wrk_scenario2.txt"

# Stop Nginx on host
ssh "$(whoami)@${WORKER}" "docker rm -f nginx-exp"

# -------------------------------------------------------------------
# Scenario 3: Nginx on BF2, DGEMM alone (after migration)
# -------------------------------------------------------------------
echo ""
echo "Starting Nginx on BF2..."
ssh "$(whoami)@${WORKER}" "
    ssh ${BF2_SSH} '
        docker rm -f nginx-exp 2>/dev/null
        docker run -d --name nginx-exp --network=host nginx:alpine
        sleep 3
        echo \"Nginx running on BF2\"
    '
"

# Start wrk against BF2 Nginx
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${DURATION}s \
    "http://${BF2_100G}/" > "${DATA}/wrk_scenario3.txt" 2>&1 &
WRK_PID=$!

run_gemm_with_perf 3 "DGEMM alone, Nginx on BF2 (after migration)"

wait ${WRK_PID} 2>/dev/null
echo "BF2 Nginx wrk results:"
cat "${DATA}/wrk_scenario3.txt"

# Cleanup
ssh "$(whoami)@${WORKER}" "ssh ${BF2_SSH} 'docker rm -f nginx-exp'"

# -------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------
echo ""
echo "============================================"
echo "  Experiment G Summary"
echo "============================================"
for s in 1 2 3; do
    f="${DATA}/gemm_scenario${s}.txt"
    if [ -f "$f" ]; then
        avg=$(awk 'NR>5 {s+=$1; n++} END{if(n>0) printf "%.1f", s/n}' "$f")
        echo "  Scenario ${s}: avg GFLOPS = ${avg}"
    fi
done
echo ""
echo "Compare with Experiment B Scenario 2 (monitoring agents): 383.7 GFLOPS (-5.3%)"
echo "Data saved to: ${DATA}/"
