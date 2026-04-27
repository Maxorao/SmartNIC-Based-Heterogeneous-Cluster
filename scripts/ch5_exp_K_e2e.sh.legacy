#!/bin/bash
# =========================================================================
# Experiment K: End-to-End Three-Configuration Comparison (Chapter 5)
#
# Compares DGEMM throughput and Nginx throughput across:
#   K.1: No offloading baseline (all on host, including TCP monitoring agents)
#   K.2: Control plane offloading only (Ch2+Ch3 stack, Nginx still on host)
#   K.3: Full system (Ch2+Ch3+Ch4, Nginx migrated to BF2)
#
# Run from: tianjin (master node)
# Target:   fujian (worker node) + fujian-bf2
# =========================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------
DURATION=60
WARMUP=5
WRK_THREADS=4
WRK_CONCURRENCY=200
VIP="192.168.56.200"

OUT_DIR="${DATA_DIR}/K"
mkdir -p "$OUT_DIR"

# SSH shortcuts
FUJIAN_SSH="${USER}@${FUJIAN_IP}"
FUJIAN_BF2_SSH="${USER}@${FUJIAN_IP}>>root@${BF_IP}"
TIANJIN_BF2_SSH="root@${BF_IP}"  # local BF2 on master

echo "============================================================"
echo " Experiment K: End-to-End Three-Configuration Comparison"
echo " Duration: ${DURATION}s per config | Output: ${OUT_DIR}"
echo "============================================================"

# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------
cleanup_all() {
    echo "[cleanup] Stopping all components..."
    # Kill monitoring agents on fujian
    ssh ${FUJIAN_SSH} "pkill -f metric_push 2>/dev/null; \
                       pkill -f gemm_bench 2>/dev/null; \
                       docker rm -f nginx 2>/dev/null" 2>/dev/null || true
    # Kill BF2 components on fujian
    ssh ${FUJIAN_SSH} "ssh root@${BF_IP} 'pkill -f slave_agent 2>/dev/null; \
                       docker rm -f nginx 2>/dev/null; \
                       ip addr del ${VIP}/24 dev p1 2>/dev/null'" 2>/dev/null || true
    # Kill master components on tianjin
    pkill -f cluster_master 2>/dev/null || true
    pkill -f master_watchdog 2>/dev/null || true
    # Remove VIP from fujian host
    ssh ${FUJIAN_SSH} "sudo ip addr del ${VIP}/24 dev enp94s0f1np1 2>/dev/null" 2>/dev/null || true
    sleep 3
    echo "[cleanup] Done"
}

# Clean slate
cleanup_all

# =========================================================================
# K.1: No Offloading Baseline
# =========================================================================
echo ""
echo "=========================================="
echo " K.1: No Offloading Baseline"
echo "=========================================="
echo "  - cluster_master on tianjin (receives gRPC reports)"
echo "  - metric_push x${N_MONITORS} instances on fujian host (gRPC fallback mode)"
echo "  - Nginx on fujian host (co-located with DGEMM)"
echo "  - DGEMM on fujian host (cores 0-15)"
echo "  - SmartNIC: no slave_agent, no Comch endpoint"

# Start cluster_master on tianjin (needed to accept gRPC DirectPush reports)
echo "[K.1] Starting cluster_master on tianjin..."
nohup ${CLUSTER_MASTER} \
    --grpc-port ${GRPC_PORT} \
    --http-port ${HTTP_STATUS_PORT} \
    --db-connstr "${DB_CONNSTR}" \
    > "${OUT_DIR}/k1_master.log" 2>&1 &
K1_MASTER_PID=$!
sleep 3

# Start Nginx on fujian host
ssh ${FUJIAN_SSH} "docker rm -f nginx 2>/dev/null; \
    docker run -d --name nginx --network=host --cpuset-cpus=0-15 nginx:alpine"
sleep 2

# Start N_MONITORS metric_push instances on fujian host
# Without slave_agent on BF2, Comch init will fail → automatic gRPC fallback
# This simulates traditional agents sending via full TCP/gRPC network stack
echo "[K.1] Starting ${N_MONITORS} metric_push instances (gRPC fallback)..."
for i in $(seq 1 ${N_MONITORS}); do
    ssh ${FUJIAN_SSH} "nohup taskset -c 0-15 ${METRIC_PUSH_V2} \
        --pci=${HOST_PCI} \
        --master-addr=${MASTER_100G}:${GRPC_PORT} \
        --interval=${HIGH_LOAD_INTERVAL} \
        --node-id=exp-k1-agent-${i} \
        > /dev/null 2>&1 &"
done
sleep 5  # wait for Comch failures and gRPC fallback activation
echo "[K.1] ${N_MONITORS} agents started (Comch failed → gRPC fallback active)"

# Start DGEMM with perf stat
echo "[K.1] Running DGEMM (${DURATION}s)..."
ssh ${FUJIAN_SSH} "sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_k1.txt -- \
    env OMP_NUM_THREADS=${GEMM_THREADS} taskset -c 0-15 ${GEMM_BENCH} \
    --size=1024 --duration=${DURATION}" \
    > "${OUT_DIR}/k1_gemm.txt" 2>&1 &
GEMM_PID=$!

# Start wrk pressure
sleep ${WARMUP}
wrk -t${WRK_THREADS} -c${WRK_CONCURRENCY} -d$((DURATION - WARMUP))s \
    http://${FUJIAN_100G}/ > "${OUT_DIR}/k1_wrk.txt" 2>&1

wait $GEMM_PID 2>/dev/null || true
ssh ${FUJIAN_SSH} "cat /tmp/perf_k1.txt" > "${OUT_DIR}/k1_perf.txt"

# Cleanup K.1
ssh ${FUJIAN_SSH} "pkill -f metric_push 2>/dev/null || true"
ssh ${FUJIAN_SSH} "docker rm -f nginx 2>/dev/null || true"
kill ${K1_MASTER_PID} 2>/dev/null || true
sleep 5

echo "[K.1] Done. Results: k1_gemm.txt, k1_wrk.txt, k1_perf.txt"

# =========================================================================
# K.2: Control Plane Offloading Only (Ch2 + Ch3)
# =========================================================================
echo ""
echo "=========================================="
echo " K.2: Control Plane Offloading Only"
echo "=========================================="
echo "  - cluster_master on tianjin"
echo "  - slave_agent on fujian-bf2"
echo "  - metric_push on fujian host (Comch mode)"
echo "  - Nginx on host (still co-located with DGEMM)"

# Start cluster_master on tianjin
echo "[K.2] Starting cluster_master..."
nohup ${CLUSTER_MASTER} \
    --grpc-port ${GRPC_PORT} \
    --http-port ${HTTP_STATUS_PORT} \
    --db-connstr "${DB_CONNSTR}" \
    > "${OUT_DIR}/k2_master.log" 2>&1 &
MASTER_PID=$!
sleep 3

# Start slave_agent on fujian BF2
echo "[K.2] Starting slave_agent on fujian-bf2..."
ssh ${FUJIAN_SSH} "ssh root@${BF_IP} 'pkill -f slave_agent 2>/dev/null || true; sleep 1; \
    nohup ${NIC_SLAVE_AGENT} \
    --dev-pci=${NIC_PCI} \
    --master-addr=${MASTER_100G}:${GRPC_PORT} \
    --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
    > /tmp/slave_agent.log 2>&1 &'"
sleep 3

# Start metric_push on fujian host (Comch mode — single lightweight instance)
# With slave_agent running on BF2, Comch init will succeed → PCIe offload path
echo "[K.2] Starting metric_push (Comch mode)..."
ssh ${FUJIAN_SSH} "nohup ${METRIC_PUSH_V2} \
    --pci=${HOST_PCI} \
    --master-addr=${MASTER_100G}:${GRPC_PORT} \
    --interval=1000 \
    --node-id=fujian \
    > /tmp/metric_push.log 2>&1 &"
sleep 2

# Start Nginx on fujian host (co-located with DGEMM)
ssh ${FUJIAN_SSH} "docker rm -f nginx 2>/dev/null; \
    docker run -d --name nginx --network=host --cpuset-cpus=0-15 nginx:alpine"
sleep 2

# Start DGEMM with perf stat
echo "[K.2] Running DGEMM (${DURATION}s)..."
ssh ${FUJIAN_SSH} "sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_k2.txt -- \
    env OMP_NUM_THREADS=${GEMM_THREADS} taskset -c 0-15 ${GEMM_BENCH} \
    --size=1024 --duration=${DURATION}" \
    > "${OUT_DIR}/k2_gemm.txt" 2>&1 &
GEMM_PID=$!

# Start wrk pressure
sleep ${WARMUP}
wrk -t${WRK_THREADS} -c${WRK_CONCURRENCY} -d$((DURATION - WARMUP))s \
    http://${FUJIAN_100G}/ > "${OUT_DIR}/k2_wrk.txt" 2>&1

wait $GEMM_PID 2>/dev/null || true
ssh ${FUJIAN_SSH} "cat /tmp/perf_k2.txt" > "${OUT_DIR}/k2_perf.txt"

# Cleanup K.2: stop Nginx and metric_push (keep cluster_master and slave_agent for K.3)
ssh ${FUJIAN_SSH} "docker rm -f nginx 2>/dev/null || true"
ssh ${FUJIAN_SSH} "pkill -f metric_push 2>/dev/null || true"
sleep 5

echo "[K.2] Done. Results: k2_gemm.txt, k2_wrk.txt, k2_perf.txt"

# =========================================================================
# K.3: Full System (Ch2 + Ch3 + Ch4 orchestration)
# =========================================================================
echo ""
echo "=========================================="
echo " K.3: Full System (with orchestration)"
echo "=========================================="
echo "  - cluster_master + slave_agent still running from K.2"
echo "  - metric_push on fujian host (Comch mode)"
echo "  - Nginx on fujian-bf2 (migrated)"
echo "  - DGEMM alone on host"

# Restart metric_push on fujian host (Comch mode)
ssh ${FUJIAN_SSH} "nohup ${METRIC_PUSH_V2} \
    --pci=${HOST_PCI} \
    --master-addr=${MASTER_100G}:${GRPC_PORT} \
    --interval=1000 \
    --node-id=fujian \
    > /tmp/metric_push.log 2>&1 &"
sleep 2

# Start Nginx on fujian BF2 (simulating orchestration result)
ssh ${FUJIAN_SSH} "ssh root@${BF_IP} 'docker rm -f nginx 2>/dev/null; \
    docker run -d --name nginx --network=host nginx:alpine'"
sleep 2

# Assign VIP to BF2
ssh ${FUJIAN_SSH} "ssh root@${BF_IP} 'ip addr add ${VIP}/24 dev p1 2>/dev/null || true'"
ssh ${FUJIAN_SSH} "ssh root@${BF_IP} 'arping -c 3 -A -I p1 ${VIP} &>/dev/null &'"
sleep 2

# Start DGEMM with perf stat (host alone)
echo "[K.3] Running DGEMM (${DURATION}s)..."
ssh ${FUJIAN_SSH} "sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_k3.txt -- \
    env OMP_NUM_THREADS=${GEMM_THREADS} taskset -c 0-15 ${GEMM_BENCH} \
    --size=1024 --duration=${DURATION}" \
    > "${OUT_DIR}/k3_gemm.txt" 2>&1 &
GEMM_PID=$!

# Start wrk pressure (against VIP on BF2)
sleep ${WARMUP}
wrk -t${WRK_THREADS} -c${WRK_CONCURRENCY} -d$((DURATION - WARMUP))s \
    http://${VIP}/ > "${OUT_DIR}/k3_wrk.txt" 2>&1

wait $GEMM_PID 2>/dev/null || true
ssh ${FUJIAN_SSH} "cat /tmp/perf_k3.txt" > "${OUT_DIR}/k3_perf.txt"

echo "[K.3] Done. Results: k3_gemm.txt, k3_wrk.txt, k3_perf.txt"

# =========================================================================
# Cleanup
# =========================================================================
cleanup_all

# ── Emit machine-readable summary for run_repeated.sh ─────────────────────────
SUMMARY_CSV="${SUMMARY_CSV:-${OUT_DIR}/summary.csv}"
python3 "${SCRIPT_DIR}/analyze/emit_summary.py" \
    --exp K --data-dir "${OUT_DIR}" --out "${SUMMARY_CSV}" || true

echo ""
echo "============================================================"
echo " Experiment K Complete"
echo "============================================================"
echo ""
echo "Results in: ${OUT_DIR}/"
echo ""
echo "Data to extract:"
echo "  K.1 (no offload):   GFLOPS from k1_gemm.txt, Nginx req/s from k1_wrk.txt"
echo "  K.2 (CP offload):   GFLOPS from k2_gemm.txt, Nginx req/s from k2_wrk.txt"
echo "  K.3 (full system):  GFLOPS from k3_gemm.txt, Nginx req/s from k3_wrk.txt"
echo ""
echo "Also: LLC miss rates and context switches from k{1,2,3}_perf.txt"
