#!/bin/bash
# =========================================================================
# Experiment H: Workload-co-location interference quantification
#
# Hypothesis: hosting a high-throughput data-plane workload (Nginx) on the
# same compute cores as DGEMM costs DGEMM throughput; migrating the same
# Nginx onto the BF2 SmartNIC frees those host cores and DGEMM recovers.
#
# Three scenarios, each runs gemm_bench for ${DURATION}s pinned to
# ${COMPUTE_CORES}:
#
#   H.1 baseline:    gemm alone.
#   H.2 co-located:  gemm + nginx (Docker on fujian host, cpuset same as
#                    gemm) under wrk pressure.
#   H.3 BF2 offload: gemm alone on host; nginx (Docker on fujian BF2)
#                    under wrk pressure. Host cores see no nginx.
#
# Topology / fix-ups vs the legacy script:
#   - CPU binding moved from 0-15 to 48-63 (NUMA node1, away from system
#     processes that default to socket 0).
#   - perf invoked via /usr/lib/linux-tools-* (the kernel-version-matched
#     binary on fujian; /usr/bin/perf complains).
#   - wrk now runs on fujian host (loopback for H.2, tmfifo for H.3) bound
#     to socket-0 cores 0-15 — keeps the load generator off socket 1 so it
#     does not contaminate the DGEMM measurement.
#   - The legacy script ran wrk from tianjin via ${FUJIAN_100G}, but
#     host-to-host 100G is not configured in this lab.
#
# Run: bash scripts/ch4_exp_H_interference.sh
# Override: EXP_DURATION (default 60), COMPUTE_CORES (default 48-63),
#           WRK_CORES (default 0-15), CONCURRENCY, WRK_THREADS.
# =========================================================================

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION="${EXP_DURATION:-${GEMM_DURATION:-60}}"
WARMUP=5
CONCURRENCY="${CONCURRENCY:-200}"
WRK_THREADS="${WRK_THREADS:-4}"
COMPUTE_CORES="${COMPUTE_CORES:-48-63}"
WRK_CORES="${WRK_CORES:-0-15}"

OUT_DIR="${DATA_DIR}/H"
mkdir -p "$OUT_DIR"

FUJIAN_SSH="${USER}@${FUJIAN_IP}"

echo "============================================================"
echo " Experiment H: Workload-Co-location Interference"
echo " Duration: ${DURATION}s | gemm cores: ${COMPUTE_CORES}"
echo " wrk cores: ${WRK_CORES} | wrk -c${CONCURRENCY} -t${WRK_THREADS}"
echo " Output: ${OUT_DIR}"
echo "============================================================"

cleanup_all() {
    echo "[cleanup] stopping all components..."
    ssh "${FUJIAN_SSH}" "pkill -f wrk 2>/dev/null; \
                         pkill -f gemm_bench 2>/dev/null; \
                         docker rm -f nginx-bench 2>/dev/null" 2>/dev/null || true
    ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} 'docker rm -f nginx-bench 2>/dev/null'" 2>/dev/null || true
    sleep 2
}
trap cleanup_all EXIT INT TERM
cleanup_all

# Detect a perf binary that matches a real installed kernel-tools package.
PERF_BIN=$(ssh "${FUJIAN_SSH}" "ls /usr/lib/linux-tools-*/perf 2>/dev/null | head -1")
[ -z "${PERF_BIN}" ] && PERF_BIN="perf"
echo "[H] using perf binary: ${PERF_BIN}"

run_gemm_with_perf() {
    local id="$1"
    local label="$2"
    local gflops_csv="${OUT_DIR}/h${id}_gflops.csv"
    local gflops_txt="${OUT_DIR}/h${id}_gemm.txt"
    local perf_txt="${OUT_DIR}/h${id}_perf.txt"

    echo "--- H.${id}: ${label} ---"
    ssh "${FUJIAN_SSH}" "sudo rm -f /tmp/gflops_h${id}.csv /tmp/perf_h${id}.txt"

    ssh "${FUJIAN_SSH}" "sudo ${PERF_BIN} stat -e LLC-load-misses,LLC-loads,context-switches,instructions \
        -o /tmp/perf_h${id}.txt -I 1000 -- \
        env OMP_NUM_THREADS=${GEMM_THREADS} \
        taskset -c ${COMPUTE_CORES} ${GEMM_BENCH} \
        --size=1024 --duration=${DURATION} \
        --output=/tmp/gflops_h${id}.csv" \
        > "${gflops_txt}" 2>&1

    ssh "${FUJIAN_SSH}" "cat /tmp/gflops_h${id}.csv" > "${gflops_csv}" 2>/dev/null || true
    ssh "${FUJIAN_SSH}" "cat /tmp/perf_h${id}.txt"  > "${perf_txt}"   2>/dev/null || true
    sleep 3
}

# =========================================================================
# H.1 baseline
# =========================================================================
run_gemm_with_perf 1 "DGEMM alone (baseline)"

# =========================================================================
# H.2 co-located: nginx in docker on fujian host (same NUMA cores as gemm)
# =========================================================================
echo "--- H.2 setup: starting nginx on fujian host (cpuset ${COMPUTE_CORES})..."
ssh "${FUJIAN_SSH}" "docker rm -f nginx-bench 2>/dev/null; \
    docker run -d --name nginx-bench --network=host \
    --cpuset-cpus=${COMPUTE_CORES} nginx:alpine"
sleep 3

# Sanity check
http_ok=$(ssh "${FUJIAN_SSH}" "curl -fsS -o /dev/null -w '%{http_code}' http://127.0.0.1/ 2>/dev/null || echo fail")
echo "  nginx on host responds: ${http_ok}"

# Start gemm in background, then drive wrk in parallel for the bulk of the run
ssh "${FUJIAN_SSH}" "sudo rm -f /tmp/gflops_h2.csv /tmp/perf_h2.txt"
ssh "${FUJIAN_SSH}" "sudo ${PERF_BIN} stat -e LLC-load-misses,LLC-loads,context-switches,instructions \
    -o /tmp/perf_h2.txt -I 1000 -- \
    env OMP_NUM_THREADS=${GEMM_THREADS} \
    taskset -c ${COMPUTE_CORES} ${GEMM_BENCH} \
    --size=1024 --duration=${DURATION} \
    --output=/tmp/gflops_h2.csv" \
    > "${OUT_DIR}/h2_gemm.txt" 2>&1 &
GEMM_PID=$!

sleep ${WARMUP}
ssh "${FUJIAN_SSH}" "taskset -c ${WRK_CORES} wrk -t${WRK_THREADS} -c${CONCURRENCY} \
    -d$((DURATION - WARMUP))s http://127.0.0.1/" \
    > "${OUT_DIR}/h2_wrk.txt" 2>&1

wait "${GEMM_PID}" 2>/dev/null || true
ssh "${FUJIAN_SSH}" "cat /tmp/gflops_h2.csv" > "${OUT_DIR}/h2_gflops.csv" 2>/dev/null || true
ssh "${FUJIAN_SSH}" "cat /tmp/perf_h2.txt"  > "${OUT_DIR}/h2_perf.txt"   2>/dev/null || true
ssh "${FUJIAN_SSH}" "docker rm -f nginx-bench"
sleep 3
echo "H.2 done."

# =========================================================================
# H.3 BF2 offload: nginx in docker on fujian BF2 (no host CPU footprint)
# =========================================================================
echo "--- H.3 setup: starting nginx on fujian BF2..."
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} \"docker rm -f nginx-bench 2>/dev/null; docker run -d --name nginx-bench --network=host nginx:alpine\""
sleep 4

# Sanity check via tmfifo
http_ok=$(ssh "${FUJIAN_SSH}" "curl -fsS -o /dev/null -w '%{http_code}' http://${BF_IP}/ 2>/dev/null || echo fail")
echo "  nginx on BF2 responds: ${http_ok}"

ssh "${FUJIAN_SSH}" "sudo rm -f /tmp/gflops_h3.csv /tmp/perf_h3.txt"
ssh "${FUJIAN_SSH}" "sudo ${PERF_BIN} stat -e LLC-load-misses,LLC-loads,context-switches,instructions \
    -o /tmp/perf_h3.txt -I 1000 -- \
    env OMP_NUM_THREADS=${GEMM_THREADS} \
    taskset -c ${COMPUTE_CORES} ${GEMM_BENCH} \
    --size=1024 --duration=${DURATION} \
    --output=/tmp/gflops_h3.csv" \
    > "${OUT_DIR}/h3_gemm.txt" 2>&1 &
GEMM_PID=$!

sleep ${WARMUP}
ssh "${FUJIAN_SSH}" "taskset -c ${WRK_CORES} wrk -t${WRK_THREADS} -c${CONCURRENCY} \
    -d$((DURATION - WARMUP))s http://${BF_IP}/" \
    > "${OUT_DIR}/h3_wrk.txt" 2>&1

wait "${GEMM_PID}" 2>/dev/null || true
ssh "${FUJIAN_SSH}" "cat /tmp/gflops_h3.csv" > "${OUT_DIR}/h3_gflops.csv" 2>/dev/null || true
ssh "${FUJIAN_SSH}" "cat /tmp/perf_h3.txt"  > "${OUT_DIR}/h3_perf.txt"   2>/dev/null || true
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} \"docker rm -f nginx-bench\""
echo "H.3 done."

# =========================================================================
# Quick summary + machine-readable CSV
# =========================================================================
echo ""
echo "=== Quick Summary ==="
for s in 1 2 3; do
    f="${OUT_DIR}/h${s}_gflops.csv"
    if [ -f "$f" ] && [ -s "$f" ]; then
        avg=$(awk -F, -v warm="${WARMUP}" \
            'NR>1 && NR>warm+1 && $2 != "" {s+=$2; n++} END{if(n>0) printf "%.2f", s/n; else print "N/A"}' \
            "$f" 2>/dev/null || echo "N/A")
        echo "  H.${s}: avg GFLOPS = ${avg}"
    else
        echo "  H.${s}: <no data>"
    fi
done
for s in 2 3; do
    f="${OUT_DIR}/h${s}_wrk.txt"
    if [ -f "$f" ]; then
        rps=$(awk '/Requests\/sec/ {print $2}' "$f")
        echo "  H.${s} wrk req/s: ${rps:-N/A}"
    fi
done

SUMMARY_CSV="${SUMMARY_CSV:-${OUT_DIR}/summary.csv}"
python3 "${SCRIPT_DIR}/analyze/emit_summary.py" \
    --exp H --data-dir "${OUT_DIR}" --out "${SUMMARY_CSV}" 2>/dev/null || \
    echo "(emit_summary.py failed; raw outputs in ${OUT_DIR})"

echo ""
echo "=== Experiment H Complete ==="
echo "Results: ${OUT_DIR}"
