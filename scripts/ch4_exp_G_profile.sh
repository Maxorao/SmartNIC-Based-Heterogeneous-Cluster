#!/bin/bash
# Experiment G: Workload feature profiling (Nginx vs DGEMM, x86 vs ARM)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION=30
CONCURRENCY=100
WRK_THREADS=4
OUT_DIR="${DATA_DIR}/G"
mkdir -p "$OUT_DIR"

echo "=== Experiment G: Workload Feature Profiling ==="
echo "Output: ${OUT_DIR}"

# ---------------------------------------------------------------------------
# G.1: Nginx on x86 (fujian host)
# ---------------------------------------------------------------------------
echo ""
echo "--- G.1: Nginx on x86 (fujian, 64 cores) ---"

# Start Nginx container on fujian host
ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx-bench 2>/dev/null; \
    docker run -d --name nginx-bench --network=host nginx:alpine"
sleep 3

# Measure LLC + context switches with perf while wrk runs
echo "Starting perf stat + wrk (${DURATION}s, ${CONCURRENCY} concurrent)..."
ssh ${USER}@${FUJIAN_IP} "sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -a -o /tmp/perf_nginx_x86.txt -- sleep ${DURATION}" &
PERF_PID=$!

# Run wrk from master (tianjin) against fujian
sleep 2  # let perf start first
wrk -t${WRK_THREADS} -c${CONCURRENCY} -d${DURATION}s \
    http://${FUJIAN_100G}/ > "${OUT_DIR}/wrk_nginx_x86.txt" 2>&1

wait $PERF_PID 2>/dev/null || true
ssh ${USER}@${FUJIAN_IP} "cat /tmp/perf_nginx_x86.txt" > "${OUT_DIR}/perf_nginx_x86.txt"

# Cleanup
ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx-bench"
echo "G.1 done. Results: ${OUT_DIR}/wrk_nginx_x86.txt, perf_nginx_x86.txt"

# ---------------------------------------------------------------------------
# G.2: Nginx on ARM (fujian BF2)
# ---------------------------------------------------------------------------
echo ""
echo "--- G.2: Nginx on ARM (fujian-bf2, 8 cores) ---"

# Start Nginx on BF2
ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx-bench 2>/dev/null; \
    docker run -d --name nginx-bench --network=host nginx:alpine'"
sleep 3

# Run wrk from master against BF2
wrk -t${WRK_THREADS} -c${CONCURRENCY} -d${DURATION}s \
    http://${FUJIAN_BF2_FABRIC}/ > "${OUT_DIR}/wrk_nginx_arm.txt" 2>&1

# Cleanup
ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx-bench'"
echo "G.2 done. Results: ${OUT_DIR}/wrk_nginx_arm.txt"

# ---------------------------------------------------------------------------
# G.3: DGEMM on x86 (fujian host)
# ---------------------------------------------------------------------------
echo ""
echo "--- G.3: DGEMM on x86 (fujian, 16 cores on NUMA0) ---"

ssh ${USER}@${FUJIAN_IP} "sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_dgemm_x86.txt -- \
    env OMP_NUM_THREADS=16 taskset -c 0-15 ${GEMM_BENCH} --size=1024 --duration=${DURATION}" \
    > "${OUT_DIR}/gemm_x86.txt" 2>&1

ssh ${USER}@${FUJIAN_IP} "cat /tmp/perf_dgemm_x86.txt" > "${OUT_DIR}/perf_dgemm_x86.txt"
echo "G.3 done. Results: ${OUT_DIR}/gemm_x86.txt, perf_dgemm_x86.txt"

echo ""
echo "=== Experiment G Complete ==="
echo "Please inspect the output files and report the results."
