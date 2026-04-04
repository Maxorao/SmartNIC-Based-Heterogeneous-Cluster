#!/bin/bash
# Experiment J: Orchestration strategy effectiveness + blue-green migration overhead
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION=60
WARMUP=5
CONCURRENCY=200
WRK_THREADS=4
GEMM_THREADS=16
VIP="192.168.56.200"
OUT_DIR="${DATA_DIR}/J"
mkdir -p "$OUT_DIR"

echo "=== Experiment J: Orchestration Strategy Effectiveness ==="

# ---------------------------------------------------------------------------
# J.1: No orchestration (DGEMM + Nginx co-located on fujian host)
# ---------------------------------------------------------------------------
echo ""
echo "--- J.1: No orchestration (co-located) ---"

# Assign VIP to fujian host
ssh ${USER}@${FUJIAN_IP} "sudo ip addr add ${VIP}/24 dev enp94s0f1np1 2>/dev/null || true"
ssh ${USER}@${FUJIAN_IP} "sudo arping -c 3 -A -I enp94s0f1np1 ${VIP} &>/dev/null &"

# Start Nginx on host
ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx 2>/dev/null; \
    docker run -d --name nginx --network=host nginx:alpine"
sleep 3

# Run DGEMM + wrk simultaneously
ssh ${USER}@${FUJIAN_IP} "env OMP_NUM_THREADS=${GEMM_THREADS} taskset -c 0-15 ${GEMM_BENCH} --size=1024 --duration=${DURATION}" \
    > "${OUT_DIR}/j1_gemm.txt" 2>&1 &
GEMM_PID=$!

sleep ${WARMUP}
wrk -t${WRK_THREADS} -c${CONCURRENCY} -d$((DURATION - WARMUP))s \
    http://${VIP}/ > "${OUT_DIR}/j1_wrk.txt" 2>&1

wait $GEMM_PID 2>/dev/null || true

# Cleanup
ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx"
ssh ${USER}@${FUJIAN_IP} "sudo ip addr del ${VIP}/24 dev enp94s0f1np1 2>/dev/null || true"
echo "J.1 done."

# ---------------------------------------------------------------------------
# J.2: Static orchestration (Nginx on SmartNIC, DGEMM on host)
# ---------------------------------------------------------------------------
echo ""
echo "--- J.2: Static orchestration (Nginx → SmartNIC) ---"

# Start Nginx on BF2, assign VIP to BF2
ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx 2>/dev/null; \
    docker run -d --name nginx --network=host nginx:alpine'"
sleep 2
ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'ip addr add ${VIP}/24 dev p1 2>/dev/null || true'"
ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'arping -c 3 -A -I p1 ${VIP} &>/dev/null &'"
sleep 2

# Run DGEMM on host + wrk against VIP
ssh ${USER}@${FUJIAN_IP} "env OMP_NUM_THREADS=${GEMM_THREADS} taskset -c 0-15 ${GEMM_BENCH} --size=1024 --duration=${DURATION}" \
    > "${OUT_DIR}/j2_gemm.txt" 2>&1 &
GEMM_PID=$!

sleep ${WARMUP}
wrk -t${WRK_THREADS} -c${CONCURRENCY} -d$((DURATION - WARMUP))s \
    http://${VIP}/ > "${OUT_DIR}/j2_wrk.txt" 2>&1

wait $GEMM_PID 2>/dev/null || true

# Cleanup
ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx'"
ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'ip addr del ${VIP}/24 dev p1 2>/dev/null || true'"
echo "J.2 done."

# ---------------------------------------------------------------------------
# J.3: Blue-green migration overhead (5 iterations)
# ---------------------------------------------------------------------------
echo ""
echo "--- J.3: Blue-green migration overhead (5 iterations) ---"

echo "run,container_ms,health_ms,vip_ms,stop_ms,total_ms" > "${OUT_DIR}/j3_migration_times.csv"
for i in $(seq 1 5); do
    echo "  Iteration ${i}/5..."

    # Setup: Nginx on fujian host with VIP
    ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx 2>/dev/null; \
        docker run -d --name nginx --network=host nginx:alpine"
    ssh ${USER}@${FUJIAN_IP} "sudo ip addr add ${VIP}/24 dev enp94s0f1np1 2>/dev/null || true"
    ssh ${USER}@${FUJIAN_IP} "sudo arping -c 3 -A -I enp94s0f1np1 ${VIP} &>/dev/null &"
    sleep 3

    # Measure each stage
    T_TOTAL_START=$(date +%s%N)

    # Stage 1: Start new container on BF2
    T1_START=$(date +%s%N)
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx-new 2>/dev/null; \
        docker run -d --name nginx-new --network=host nginx:alpine'"
    T1_END=$(date +%s%N)

    # Stage 2: Health check
    T2_START=$(date +%s%N)
    for attempt in $(seq 1 30); do
        if ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} \
            'curl -sf -o /dev/null http://localhost:80/'" 2>/dev/null; then
            break
        fi
        sleep 1
    done
    T2_END=$(date +%s%N)

    # Stage 3: VIP switch
    T3_START=$(date +%s%N)
    ssh ${USER}@${FUJIAN_IP} "sudo ip addr del ${VIP}/24 dev enp94s0f1np1 2>/dev/null || true"
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'ip addr add ${VIP}/24 dev p1 2>/dev/null || true'"
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'arping -c 3 -A -I p1 ${VIP} &>/dev/null &'"
    T3_END=$(date +%s%N)

    # Stage 4: Stop old container
    T4_START=$(date +%s%N)
    ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx"
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rename nginx-new nginx'"
    T4_END=$(date +%s%N)

    T_TOTAL_END=$(date +%s%N)

    # Calculate durations (nanoseconds → milliseconds)
    T1=$(( (T1_END - T1_START) / 1000000 ))
    T2=$(( (T2_END - T2_START) / 1000000 ))
    T3=$(( (T3_END - T3_START) / 1000000 ))
    T4=$(( (T4_END - T4_START) / 1000000 ))
    TT=$(( (T_TOTAL_END - T_TOTAL_START) / 1000000 ))

    echo "${i},${T1},${T2},${T3},${T4},${TT}" >> "${OUT_DIR}/j3_migration_times.csv"
    echo "  iter=${i}: start=${T1}ms, health=${T2}ms, vip=${T3}ms, stop=${T4}ms, total=${TT}ms"

    # Cleanup for next iteration
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx 2>/dev/null || true'"
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'ip addr del ${VIP}/24 dev p1 2>/dev/null || true'"
    sleep 2
done

echo "J.3 done. Timings: ${OUT_DIR}/j3_migration_times.csv"
echo ""
echo "=== Experiment J Complete ==="
echo "J.1: Parse GFLOPS from j1_gemm.txt, Nginx req/s from j1_wrk.txt"
echo "J.2: Parse GFLOPS from j2_gemm.txt, Nginx req/s from j2_wrk.txt"
echo "J.3: Migration stage timings in j3_migration_times.csv"
