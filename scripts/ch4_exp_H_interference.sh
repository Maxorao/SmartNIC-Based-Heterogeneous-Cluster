#!/bin/bash
# Experiment H: Workload co-location interference quantification
# Three scenarios: DGEMM alone / DGEMM+Nginx co-located / Nginx on SmartNIC
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION=60
WARMUP=5
CONCURRENCY=200
WRK_THREADS=4
GEMM_THREADS=16
OUT_DIR="${DATA_DIR}/H"
mkdir -p "$OUT_DIR"

echo "=== Experiment H: Co-location Interference Quantification ==="

# ---------------------------------------------------------------------------
# H.1: DGEMM alone (baseline)
# ---------------------------------------------------------------------------
echo ""
echo "--- H.1: DGEMM alone (baseline, ${DURATION}s) ---"

ssh ${USER}@${FUJIAN_IP} "sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_h1.txt -- \
    taskset -c 0-15 ${GEMM_BENCH} -n 1024 -t ${GEMM_THREADS} -d ${DURATION}" \
    > "${OUT_DIR}/h1_gemm_alone.txt" 2>&1

ssh ${USER}@${FUJIAN_IP} "cat /tmp/perf_h1.txt" > "${OUT_DIR}/h1_perf.txt"
echo "H.1 done."

# ---------------------------------------------------------------------------
# H.2: DGEMM + Nginx co-located on same NUMA node
# ---------------------------------------------------------------------------
echo ""
echo "--- H.2: DGEMM + Nginx co-located (${DURATION}s) ---"

# Start Nginx on fujian host, bound to NUMA0
ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx-bench 2>/dev/null; \
    docker run -d --name nginx-bench --network=host \
    --cpuset-cpus=0-15 nginx:alpine"
sleep 3

# Start DGEMM + perf in background
ssh ${USER}@${FUJIAN_IP} "sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_h2.txt -- \
    taskset -c 0-15 ${GEMM_BENCH} -n 1024 -t ${GEMM_THREADS} -d ${DURATION}" \
    > "${OUT_DIR}/h2_gemm_colocated.txt" 2>&1 &
GEMM_PID=$!

# Run wrk pressure from master
sleep ${WARMUP}
wrk -t${WRK_THREADS} -c${CONCURRENCY} -d$((DURATION - WARMUP))s \
    http://${FUJIAN_100G}/ > "${OUT_DIR}/h2_wrk.txt" 2>&1

wait $GEMM_PID 2>/dev/null || true
ssh ${USER}@${FUJIAN_IP} "cat /tmp/perf_h2.txt" > "${OUT_DIR}/h2_perf.txt"
ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx-bench"
echo "H.2 done."

# ---------------------------------------------------------------------------
# H.3: Nginx migrated to SmartNIC, DGEMM alone on host
# ---------------------------------------------------------------------------
echo ""
echo "--- H.3: Nginx on SmartNIC, DGEMM alone (${DURATION}s) ---"

# Start Nginx on BF2
ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx-bench 2>/dev/null; \
    docker run -d --name nginx-bench --network=host nginx:alpine'"
sleep 3

# Start DGEMM on host with perf
ssh ${USER}@${FUJIAN_IP} "sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_h3.txt -- \
    taskset -c 0-15 ${GEMM_BENCH} -n 1024 -t ${GEMM_THREADS} -d ${DURATION}" \
    > "${OUT_DIR}/h3_gemm_offloaded.txt" 2>&1 &
GEMM_PID=$!

# Run wrk against BF2
sleep ${WARMUP}
wrk -t${WRK_THREADS} -c${CONCURRENCY} -d$((DURATION - WARMUP))s \
    http://${FUJIAN_BF2_FABRIC}/ > "${OUT_DIR}/h3_wrk.txt" 2>&1

wait $GEMM_PID 2>/dev/null || true
ssh ${USER}@${FUJIAN_IP} "cat /tmp/perf_h3.txt" > "${OUT_DIR}/h3_perf.txt"
ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx-bench'"
echo "H.3 done."

echo ""
echo "=== Experiment H Complete ==="
echo "Compare GFLOPS, LLC miss rate, and context switches across H.1/H.2/H.3"
