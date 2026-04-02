#!/bin/bash
# ch4_orchestration.sh — Experiment I: Orchestration strategy validation
#
# Compares cluster performance under two configurations:
#   Scenario 1 (no orchestration): Nginx + DGEMM on same host
#   Scenario 2 (static orchestration): Nginx on BF2, DGEMM on host
#
# Also measures blue-green migration overhead (VIP switch latency).

set -e
source "$(dirname "$0")/config.sh"

WORKER="${FUJIAN_IP}"
WORKER_100G="${FUJIAN_100G}"
BF2_100G="${FUJIAN_BF2_FABRIC}"
BF2_SSH="root@192.168.100.2"
HOST_IFACE="enp94s0f1np1"
BF2_IFACE="p1"
VIP="192.168.56.200"
DURATION=60
WRK_THREADS=4
WRK_CONNS=200
DATA="${DATA_DIR}/ch4_I"
mkdir -p "${DATA}"

echo "============================================"
echo "  Experiment I: Orchestration Strategy"
echo "============================================"

# -------------------------------------------------------------------
# Scenario 1: No orchestration — Nginx + DGEMM both on host
# -------------------------------------------------------------------
echo ""
echo "--- Scenario 1: No orchestration (Nginx + DGEMM on host) ---"

# Assign VIP to host
ssh "$(whoami)@${WORKER}" "
    sudo ip addr add ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
    docker rm -f nginx-exp 2>/dev/null
    docker run -d --name nginx-exp --network=host --cpuset-cpus=0-15 nginx:alpine
    sleep 3
"

# Start wrk against VIP
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${DURATION}s \
    "http://${VIP}/" > "${DATA}/wrk_scenario1.txt" 2>&1 &
WRK_PID=$!

# Run DGEMM with perf stat
ssh "$(whoami)@${WORKER}" "
    sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
        -o /tmp/perf_orch_s1.txt \
        numactl --cpunodebind=0 --membind=0 \
        env OPENBLAS_NUM_THREADS=16 \
        ~/experiments/bench/gemm_bench/gemm_bench --duration=${DURATION} \
        > /tmp/gemm_orch_s1.txt 2>&1
"

wait ${WRK_PID} 2>/dev/null
scp "$(whoami)@${WORKER}:/tmp/gemm_orch_s1.txt" "${DATA}/"
scp "$(whoami)@${WORKER}:/tmp/perf_orch_s1.txt" "${DATA}/"

echo "Scenario 1 GFLOPS:"
tail -3 "${DATA}/gemm_orch_s1.txt"
echo "Scenario 1 Nginx:"
grep "Requests/sec" "${DATA}/wrk_scenario1.txt" || true

# Cleanup scenario 1
ssh "$(whoami)@${WORKER}" "
    docker rm -f nginx-exp
    ip addr del ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
"

sleep 5

# -------------------------------------------------------------------
# Blue-green migration measurement
# -------------------------------------------------------------------
echo ""
echo "--- Blue-green migration overhead measurement ---"

# Setup: Nginx on host with VIP
ssh "$(whoami)@${WORKER}" "
    docker run -d --name nginx-exp --network=host nginx:alpine
    ip addr add ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
    sleep 3
"

# Measure: blue-green migration to BF2
echo "run,new_container_ms,health_check_ms,vip_switch_ms,old_stop_ms,total_ms" \
    > "${DATA}/migration_overhead.csv"

for run in $(seq 1 5); do
    echo "  Migration run ${run}/5..."
    T_START=$(date +%s%3N)

    # Step 1: Start new container on BF2
    ssh "$(whoami)@${WORKER}" "
        ssh ${BF2_SSH} '
            docker rm -f nginx-new 2>/dev/null
            docker run -d --name nginx-new --network=host nginx:alpine
        '
    "
    T_CONTAINER=$(date +%s%3N)

    # Step 2: Health check
    until ssh "$(whoami)@${WORKER}" "ssh ${BF2_SSH} 'curl -sf http://localhost/ >/dev/null'" 2>/dev/null; do
        sleep 0.2
    done
    T_HEALTH=$(date +%s%3N)

    # Step 3: VIP switch
    ssh "$(whoami)@${WORKER}" "
        ip addr del ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
        ssh ${BF2_SSH} '
            ip addr add ${VIP}/24 dev ${BF2_IFACE} 2>/dev/null || true
            arping -c 2 -A -I ${BF2_IFACE} ${VIP} &>/dev/null &
        '
    "
    T_VIP=$(date +%s%3N)

    # Step 4: Stop old container
    ssh "$(whoami)@${WORKER}" "docker rm -f nginx-exp 2>/dev/null"
    T_STOP=$(date +%s%3N)

    # Calculate durations
    CONTAINER_MS=$((T_CONTAINER - T_START))
    HEALTH_MS=$((T_HEALTH - T_CONTAINER))
    VIP_MS=$((T_VIP - T_HEALTH))
    STOP_MS=$((T_STOP - T_VIP))
    TOTAL_MS=$((T_STOP - T_START))

    echo "    container=${CONTAINER_MS}ms health=${HEALTH_MS}ms vip=${VIP_MS}ms stop=${STOP_MS}ms total=${TOTAL_MS}ms"
    echo "${run},${CONTAINER_MS},${HEALTH_MS},${VIP_MS},${STOP_MS},${TOTAL_MS}" \
        >> "${DATA}/migration_overhead.csv"

    # Reset for next run: move Nginx back to host
    ssh "$(whoami)@${WORKER}" "
        ssh ${BF2_SSH} '
            ip addr del ${VIP}/24 dev ${BF2_IFACE} 2>/dev/null || true
            docker rename nginx-new nginx-exp 2>/dev/null || true
            docker rm -f nginx-exp 2>/dev/null
        '
        docker run -d --name nginx-exp --network=host nginx:alpine
        ip addr add ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
        sleep 2
    "
done

echo ""
echo "Migration overhead:"
cat "${DATA}/migration_overhead.csv"

# Cleanup
ssh "$(whoami)@${WORKER}" "
    docker rm -f nginx-exp 2>/dev/null
    ip addr del ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
"

sleep 5

# -------------------------------------------------------------------
# Scenario 2: Static orchestration — Nginx on BF2, DGEMM on host
# -------------------------------------------------------------------
echo ""
echo "--- Scenario 2: Static orchestration (Nginx on BF2, DGEMM on host) ---"

# Start Nginx on BF2 with VIP
ssh "$(whoami)@${WORKER}" "
    ssh ${BF2_SSH} '
        docker rm -f nginx-exp 2>/dev/null
        docker run -d --name nginx-exp --network=host nginx:alpine
        ip addr add ${VIP}/24 dev ${BF2_IFACE} 2>/dev/null || true
        arping -c 2 -A -I ${BF2_IFACE} ${VIP} &>/dev/null &
        sleep 3
    '
"

# Start wrk against VIP (now routed to BF2)
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${DURATION}s \
    "http://${VIP}/" > "${DATA}/wrk_scenario2.txt" 2>&1 &
WRK_PID=$!

# Run DGEMM on host (now without Nginx interference)
ssh "$(whoami)@${WORKER}" "
    sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
        -o /tmp/perf_orch_s2.txt \
        numactl --cpunodebind=0 --membind=0 \
        env OPENBLAS_NUM_THREADS=16 \
        ~/experiments/bench/gemm_bench/gemm_bench --duration=${DURATION} \
        > /tmp/gemm_orch_s2.txt 2>&1
"

wait ${WRK_PID} 2>/dev/null
scp "$(whoami)@${WORKER}:/tmp/gemm_orch_s2.txt" "${DATA}/"
scp "$(whoami)@${WORKER}:/tmp/perf_orch_s2.txt" "${DATA}/"

echo "Scenario 2 GFLOPS:"
tail -3 "${DATA}/gemm_orch_s2.txt"
echo "Scenario 2 Nginx (on BF2):"
grep "Requests/sec" "${DATA}/wrk_scenario2.txt" || true

# Cleanup
ssh "$(whoami)@${WORKER}" "
    ssh ${BF2_SSH} '
        docker rm -f nginx-exp 2>/dev/null
        ip addr del ${VIP}/24 dev ${BF2_IFACE} 2>/dev/null || true
    '
"

# -------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------
echo ""
echo "============================================"
echo "  Experiment I Summary"
echo "============================================"

for s in 1 2; do
    f="${DATA}/gemm_orch_s${s}.txt"
    if [ -f "$f" ]; then
        avg=$(awk 'NR>5 {s+=$1; n++} END{if(n>0) printf "%.1f", s/n}' "$f")
        echo "  Scenario ${s}: DGEMM avg GFLOPS = ${avg}"
    fi
done

echo ""
echo "  Scenario 1 Nginx (on host):"
grep "Requests/sec" "${DATA}/wrk_scenario1.txt" 2>/dev/null || echo "    N/A"
echo "  Scenario 2 Nginx (on BF2):"
grep "Requests/sec" "${DATA}/wrk_scenario2.txt" 2>/dev/null || echo "    N/A"

echo ""
echo "  Migration overhead (5 runs):"
cat "${DATA}/migration_overhead.csv"

echo ""
echo "Compare with Experiment B baseline: 405.1 GFLOPS"
echo "Data saved to: ${DATA}/"
