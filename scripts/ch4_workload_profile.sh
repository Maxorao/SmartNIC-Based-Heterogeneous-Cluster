#!/bin/bash
# ch4_workload_profile.sh — Experiment F: Workload feature profiling
#
# Measures Nginx and DGEMM characteristics on x86 host and BF2 ARM.
# Collects: CPU%, LLC miss rate, network I/O, context switches, req/s, GFLOPS.

set -e
source "$(dirname "$0")/config.sh"

WORKER="${FUJIAN_IP}"
WORKER_100G="${FUJIAN_100G}"
BF2_100G="${FUJIAN_BF2_FABRIC}"
BF2_SSH="root@192.168.100.2"
DURATION=30
WRK_THREADS=4
WRK_CONNS=100
DATA="${DATA_DIR}/ch4_F"
mkdir -p "${DATA}"

echo "============================================"
echo "  Experiment F: Workload Feature Profiling"
echo "============================================"

# -------------------------------------------------------------------
# F.1: Nginx on x86 host
# -------------------------------------------------------------------
echo ""
echo "--- F.1: Nginx on x86 host (${WORKER_100G}) ---"

ssh "$(whoami)@${WORKER}" "
    docker rm -f nginx-profile 2>/dev/null
    docker run -d --name nginx-profile --network=host nginx:alpine
    sleep 3

    # Start perf stat in background
    sudo perf stat -e LLC-load-misses,LLC-loads,context-switches -a \
        -o /tmp/perf_nginx_x86.txt sleep ${DURATION} &
    PERF_PID=\$!

    # Start sar for network I/O
    sar -n DEV 1 ${DURATION} > /tmp/sar_nginx_x86.txt 2>&1 &
    SAR_PID=\$!

    sleep 2
    echo 'Perf + sar started, waiting for wrk...'
    wait \${PERF_PID} \${SAR_PID} 2>/dev/null
"

# Run wrk from tianjin (master)
echo "Running wrk against x86 Nginx (${WORKER_100G})..."
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${DURATION}s \
    "http://${WORKER_100G}/" 2>&1 | tee "${DATA}/wrk_nginx_x86.txt"

# Collect results
scp "$(whoami)@${WORKER}:/tmp/perf_nginx_x86.txt" "${DATA}/"
scp "$(whoami)@${WORKER}:/tmp/sar_nginx_x86.txt" "${DATA}/"
ssh "$(whoami)@${WORKER}" "docker rm -f nginx-profile"

echo "--- F.1 Results ---"
cat "${DATA}/perf_nginx_x86.txt"

# -------------------------------------------------------------------
# F.2: Nginx on BF2 ARM
# -------------------------------------------------------------------
echo ""
echo "--- F.2: Nginx on BF2 ARM (${BF2_100G}) ---"

ssh "$(whoami)@${WORKER}" "
    ssh ${BF2_SSH} '
        docker rm -f nginx-profile 2>/dev/null
        docker run -d --name nginx-profile --network=host nginx:alpine
        sleep 3
        echo \"Nginx started on BF2\"
    '
"

echo "Running wrk against BF2 Nginx (${BF2_100G})..."
wrk -t${WRK_THREADS} -c${WRK_CONNS} -d${DURATION}s \
    "http://${BF2_100G}/" 2>&1 | tee "${DATA}/wrk_nginx_bf2.txt"

ssh "$(whoami)@${WORKER}" "ssh ${BF2_SSH} 'docker rm -f nginx-profile'"

# -------------------------------------------------------------------
# F.3: DGEMM on x86 host
# -------------------------------------------------------------------
echo ""
echo "--- F.3: DGEMM on x86 host ---"

ssh "$(whoami)@${WORKER}" "
    sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
        numactl --cpunodebind=0 --membind=0 \
        env OPENBLAS_NUM_THREADS=16 \
        ~/experiments/bench/gemm_bench/gemm_bench --duration=${DURATION} \
        > /tmp/gemm_x86.txt 2>/tmp/perf_gemm_x86.txt
"
scp "$(whoami)@${WORKER}:/tmp/gemm_x86.txt" "${DATA}/"
scp "$(whoami)@${WORKER}:/tmp/perf_gemm_x86.txt" "${DATA}/"

echo "--- F.3 Results ---"
tail -5 "${DATA}/gemm_x86.txt"
cat "${DATA}/perf_gemm_x86.txt"

# -------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------
echo ""
echo "============================================"
echo "  Experiment F Complete"
echo "  Data saved to: ${DATA}/"
echo "============================================"
ls -la "${DATA}/"
