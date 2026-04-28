#!/bin/bash
# =========================================================================
# Experiment K: End-to-End Three-Configuration Comparison (Chapter 5, v2)
#
# Compares DGEMM throughput and Nginx throughput across:
#
#   K.1: No offloading.
#        - cluster_master on tianjin (gRPC).
#        - N metric_push on fujian host (gRPC fallback — slave_agent
#          stopped). Simulates the legacy stack.
#        - Nginx in Docker on fujian host (cpuset = ${COMPUTE_CORES}).
#        - DGEMM on host fights cores with Nginx + agents + softirqs.
#
#   K.2: Control-plane offload (Ch.2 + Ch.3).
#        - cluster_master on tianjin.
#        - slave_agent on fujian BF2 (Comch listener).
#        - 1 metric_push on fujian host in Comch mode (PCIe path).
#        - Nginx still on fujian host. DGEMM still co-located with Nginx
#          but no longer fighting metric_push agents.
#
#   K.3: Full system (Ch.2 + Ch.3 + Ch.4 — workload migrated).
#        - cluster_master + slave_agent + metric_push (Comch).
#        - Nginx migrated to fujian BF2.
#        - DGEMM has the host to itself.
#
# Topology / lab fix-ups vs the legacy script:
#   - CPU binding: ${COMPUTE_CORES} (default 48-63) on NUMA node1, off
#     socket 0 / system processes.
#   - perf via /usr/lib/linux-tools-* (kernel-version-matched).
#   - wrk runs on fujian host, bound to ${WRK_CORES} (default 0-15) so
#     the load generator does not contaminate the DGEMM measurement.
#     - K.1 / K.2 target: http://127.0.0.1/ (Nginx on host loopback).
#     - K.3 target:       http://${BF_IP}/ (Nginx on BF2 via tmfifo).
#     The legacy script targeted ${FUJIAN_100G} from tianjin, but
#     host-to-host 100G is not configured in this lab.
#   - master-addr for slave_agent / metric_push: ${MASTER_HOST} (1G mgmt
#     LAN). slave_agent on BF2 routes via tmfifo, fujian host routes via
#     eno1.
#
# Run: bash scripts/ch5_exp_K_e2e.sh
# Override: EXP_DURATION (default 60), COMPUTE_CORES (default 48-63),
#           WRK_CORES (default 0-15), N_AGENTS (K.1 fallback agents,
#           default 4), CONCURRENCY, WRK_THREADS.
# =========================================================================

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION="${EXP_DURATION:-${GEMM_DURATION:-60}}"
WARMUP=5
WRK_THREADS="${WRK_THREADS:-4}"
WRK_CONCURRENCY="${WRK_CONCURRENCY:-${CONCURRENCY:-200}}"
COMPUTE_CORES="${COMPUTE_CORES:-48-63}"
WRK_CORES="${WRK_CORES:-0-15}"
N_AGENTS="${N_AGENTS:-4}"

OUT_DIR="${DATA_DIR}/K"
mkdir -p "$OUT_DIR"

FUJIAN_SSH="${USER}@${FUJIAN_IP}"
MASTER_ADDR="${MASTER_HOST}:${GRPC_PORT}"

echo "============================================================"
echo " Experiment K: End-to-End Three-Configuration Comparison"
echo " Duration: ${DURATION}s | gemm cores: ${COMPUTE_CORES}"
echo " wrk cores: ${WRK_CORES} | wrk -c${WRK_CONCURRENCY} -t${WRK_THREADS}"
echo " master endpoint: ${MASTER_ADDR}"
echo " Output: ${OUT_DIR}"
echo "============================================================"

cleanup_all() {
    echo "[cleanup] stopping all components..."
    ssh "${FUJIAN_SSH}" "pkill -f wrk 2>/dev/null; \
                         pkill -f gemm_bench 2>/dev/null; \
                         pkill -f metric_push 2>/dev/null; \
                         docker rm -f nginx 2>/dev/null" 2>/dev/null || true
    ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} 'pkill -f slave_agent 2>/dev/null; \
                         docker rm -f nginx 2>/dev/null'" 2>/dev/null || true
    pkill -f cluster_master 2>/dev/null || true
    sleep 2
}
trap cleanup_all EXIT INT TERM
cleanup_all

# Detect a perf binary that matches a real installed kernel-tools package.
PERF_BIN=$(ssh "${FUJIAN_SSH}" "ls /usr/lib/linux-tools-*/perf 2>/dev/null | head -1")
[ -z "${PERF_BIN}" ] && PERF_BIN="perf"
echo "[K] using perf binary on fujian: ${PERF_BIN}"

# Common: cluster_master on tianjin
echo "[K] starting cluster_master on tianjin..."
nohup "${CLUSTER_MASTER}" \
    --grpc-port "${GRPC_PORT}" \
    --http-port "${HTTP_STATUS_PORT}" \
    --db-connstr "${DB_CONNSTR}" \
    > "${OUT_DIR}/master.log" 2>&1 &
MASTER_PID=$!
sleep 3
if ! kill -0 "${MASTER_PID}" 2>/dev/null; then
    echo "[K] ERROR: cluster_master died at startup. Last log:"
    tail -30 "${OUT_DIR}/master.log"
    exit 1
fi

# Helper: run gemm with perf, write into ${OUT_DIR}/k${id}_*
run_gemm_with_perf_bg() {
    local id="$1"
    ssh "${FUJIAN_SSH}" "sudo rm -f /tmp/gflops_k${id}.csv /tmp/perf_k${id}.txt"
    ssh "${FUJIAN_SSH}" "sudo ${PERF_BIN} stat -e LLC-load-misses,LLC-loads,context-switches,instructions \
        -o /tmp/perf_k${id}.txt -I 1000 -- \
        env OMP_NUM_THREADS=${GEMM_THREADS} \
        taskset -c ${COMPUTE_CORES} ${GEMM_BENCH} \
        --size=1024 --duration=${DURATION} \
        --output=/tmp/gflops_k${id}.csv" \
        > "${OUT_DIR}/k${id}_gemm.txt" 2>&1 &
    GEMM_PID=$!
}

collect_gemm_outputs() {
    local id="$1"
    ssh "${FUJIAN_SSH}" "cat /tmp/gflops_k${id}.csv" > "${OUT_DIR}/k${id}_gflops.csv" 2>/dev/null || true
    ssh "${FUJIAN_SSH}" "cat /tmp/perf_k${id}.txt"  > "${OUT_DIR}/k${id}_perf.txt"   2>/dev/null || true
}

# =========================================================================
# K.1: No Offloading Baseline
# =========================================================================
echo ""
echo "=========================================="
echo " K.1: No Offloading"
echo "=========================================="
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} 'pkill -f slave_agent 2>/dev/null'" 2>/dev/null || true
sleep 2

echo "[K.1] starting nginx on fujian host (cpuset ${COMPUTE_CORES})..."
ssh "${FUJIAN_SSH}" "docker rm -f nginx 2>/dev/null; \
    docker run -d --name nginx --network=host --cpuset-cpus=${COMPUTE_CORES} nginx:alpine"
sleep 3

echo "[K.1] starting ${N_AGENTS} metric_push on fujian host (gRPC fallback)..."
for i in $(seq 1 "${N_AGENTS}"); do
    ssh "${FUJIAN_SSH}" "nohup taskset -c ${COMPUTE_CORES} ${METRIC_PUSH_V2} \
        --pci=${HOST_PCI} \
        --master-addr=${MASTER_ADDR} \
        --interval=1000 \
        --node-id=expK-k1-agent-${i} \
        > /tmp/metric_push_k1_${i}.log 2>&1 &"
done
sleep 7   # let Comch fail and gRPC fallback take over

echo "[K.1] running gemm + wrk for ${DURATION}s..."
run_gemm_with_perf_bg 1
sleep ${WARMUP}
ssh "${FUJIAN_SSH}" "taskset -c ${WRK_CORES} wrk -t${WRK_THREADS} -c${WRK_CONCURRENCY} \
    -d$((DURATION - WARMUP))s http://127.0.0.1/" \
    > "${OUT_DIR}/k1_wrk.txt" 2>&1
wait "${GEMM_PID}" 2>/dev/null || true
collect_gemm_outputs 1

ssh "${FUJIAN_SSH}" "pkill -f metric_push 2>/dev/null" 2>/dev/null || true
ssh "${FUJIAN_SSH}" "docker rm -f nginx" 2>/dev/null || true
sleep 4

# =========================================================================
# K.2: Control-Plane Offload — slave_agent on BF2, metric_push via Comch
# =========================================================================
echo ""
echo "=========================================="
echo " K.2: Control-Plane Offload"
echo "=========================================="
echo "[K.2] starting slave_agent on fujian BF2..."
SLAVE_CMD="nohup ${NIC_SLAVE_AGENT} --master-addr=${MASTER_ADDR} --hostname=fujian --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} --report-ms=1000 --bf2-report-ms=${HEARTBEAT_INTERVAL_MS} > /tmp/slave_agent_K.log 2>&1 < /dev/null &"
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} \"pkill -f slave_agent 2>/dev/null\""
sleep 1
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} \"${SLAVE_CMD}\""
sleep 4

echo "[K.2] starting nginx on fujian host..."
ssh "${FUJIAN_SSH}" "docker rm -f nginx 2>/dev/null; \
    docker run -d --name nginx --network=host --cpuset-cpus=${COMPUTE_CORES} nginx:alpine"
sleep 3

echo "[K.2] starting metric_push (Comch path) on fujian host..."
ssh "${FUJIAN_SSH}" "nohup sudo taskset -c ${COMPUTE_CORES} ${METRIC_PUSH_V2} \
    --pci=${HOST_PCI} \
    --master-addr=${MASTER_ADDR} \
    --interval=1000 \
    --node-id=expK-k2 \
    > /tmp/metric_push_k2.log 2>&1 &"
sleep 4
mode_line=$(ssh "${FUJIAN_SSH}" "grep -m1 'mode=' /tmp/metric_push_k2.log 2>/dev/null" || echo "")
echo "  metric_push: ${mode_line}"

echo "[K.2] running gemm + wrk for ${DURATION}s..."
run_gemm_with_perf_bg 2
sleep ${WARMUP}
ssh "${FUJIAN_SSH}" "taskset -c ${WRK_CORES} wrk -t${WRK_THREADS} -c${WRK_CONCURRENCY} \
    -d$((DURATION - WARMUP))s http://127.0.0.1/" \
    > "${OUT_DIR}/k2_wrk.txt" 2>&1
wait "${GEMM_PID}" 2>/dev/null || true
collect_gemm_outputs 2

ssh "${FUJIAN_SSH}" "pkill -f metric_push 2>/dev/null" 2>/dev/null || true
ssh "${FUJIAN_SSH}" "docker rm -f nginx" 2>/dev/null || true
sleep 4

# =========================================================================
# K.3: Full System — Nginx migrated to BF2, host has nothing but DGEMM
# =========================================================================
echo ""
echo "=========================================="
echo " K.3: Full System"
echo "=========================================="
echo "[K.3] migrating nginx to fujian BF2..."
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} \"docker rm -f nginx 2>/dev/null; docker run -d --name nginx --network=host nginx:alpine\""
sleep 4

# Sanity check
http_ok=$(ssh "${FUJIAN_SSH}" "curl -fsS -o /dev/null -w '%{http_code}' http://${BF_IP}/ 2>/dev/null || echo fail")
echo "  nginx on BF2 responds: ${http_ok}"

echo "[K.3] restarting metric_push (Comch path) on fujian host..."
ssh "${FUJIAN_SSH}" "nohup sudo taskset -c ${COMPUTE_CORES} ${METRIC_PUSH_V2} \
    --pci=${HOST_PCI} \
    --master-addr=${MASTER_ADDR} \
    --interval=1000 \
    --node-id=expK-k3 \
    > /tmp/metric_push_k3.log 2>&1 &"
sleep 3

echo "[K.3] running gemm + wrk for ${DURATION}s..."
run_gemm_with_perf_bg 3
sleep ${WARMUP}
ssh "${FUJIAN_SSH}" "taskset -c ${WRK_CORES} wrk -t${WRK_THREADS} -c${WRK_CONCURRENCY} \
    -d$((DURATION - WARMUP))s http://${BF_IP}/" \
    > "${OUT_DIR}/k3_wrk.txt" 2>&1
wait "${GEMM_PID}" 2>/dev/null || true
collect_gemm_outputs 3

ssh "${FUJIAN_SSH}" "pkill -f metric_push 2>/dev/null" 2>/dev/null || true
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} \"docker rm -f nginx\""
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} 'pkill -f slave_agent 2>/dev/null'" 2>/dev/null || true

# =========================================================================
# Summary
# =========================================================================
echo ""
echo "=== Quick Summary ==="
for s in 1 2 3; do
    f="${OUT_DIR}/k${s}_gflops.csv"
    if [ -f "$f" ] && [ -s "$f" ]; then
        avg=$(awk -F, -v warm="${WARMUP}" \
            'NR>1 && NR>warm+1 && $2 != "" {s+=$2; n++} END{if(n>0) printf "%.2f", s/n; else print "N/A"}' \
            "$f" 2>/dev/null || echo "N/A")
        echo "  K.${s}: avg GFLOPS = ${avg}"
    else
        echo "  K.${s}: <no data>"
    fi
done
for s in 1 2 3; do
    f="${OUT_DIR}/k${s}_wrk.txt"
    if [ -f "$f" ]; then
        rps=$(awk '/Requests\/sec/ {print $2}' "$f")
        echo "  K.${s} wrk req/s: ${rps:-N/A}"
    fi
done

SUMMARY_CSV="${SUMMARY_CSV:-${OUT_DIR}/summary.csv}"
python3 "${SCRIPT_DIR}/analyze/emit_summary.py" \
    --exp K --data-dir "${OUT_DIR}" --out "${SUMMARY_CSV}" 2>/dev/null || \
    echo "(emit_summary.py failed; raw outputs in ${OUT_DIR})"

echo ""
echo "=== Experiment K Complete ==="
echo "Results: ${OUT_DIR}"
