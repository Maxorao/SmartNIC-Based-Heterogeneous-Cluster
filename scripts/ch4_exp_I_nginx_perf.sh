#!/bin/bash
# Experiment I: Nginx performance on SmartNIC (concurrency scaling + per-core efficiency)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION=30
WRK_THREADS=4
OUT_DIR="${DATA_DIR}/I"
mkdir -p "$OUT_DIR"

echo "=== Experiment I: SmartNIC Nginx Performance ==="

# ---------------------------------------------------------------------------
# I.1: Concurrency scaling (x86 vs ARM, full cores)
# ---------------------------------------------------------------------------
echo ""
echo "--- I.1: Concurrency Scaling ---"

for CONC in 10 50 100 200 400; do
    echo "  Concurrency=${CONC}..."

    # x86 (fujian host, all 64 cores)
    ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx-bench 2>/dev/null; \
        docker run -d --name nginx-bench --network=host nginx:alpine"
    sleep 2
    wrk -t${WRK_THREADS} -c${CONC} -d${DURATION}s \
        http://${FUJIAN_100G}/ > "${OUT_DIR}/conc_x86_c${CONC}.txt" 2>&1
    ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx-bench"

    # ARM (fujian BF2, all 8 cores)
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx-bench 2>/dev/null; \
        docker run -d --name nginx-bench --network=host nginx:alpine'"
    sleep 2
    wrk -t${WRK_THREADS} -c${CONC} -d${DURATION}s \
        http://${FUJIAN_BF2_FABRIC}/ > "${OUT_DIR}/conc_arm_c${CONC}.txt" 2>&1
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx-bench'"

    echo "  Done c=${CONC}"
done

echo "I.1 done."

# ---------------------------------------------------------------------------
# I.2: Per-core efficiency (fixed concurrency=100)
# ---------------------------------------------------------------------------
echo ""
echo "--- I.2: Per-Core Efficiency ---"

for CORES in 1 2 4; do
    echo "  Cores=${CORES}..."
    CORE_LIST_X86="16-$((15 + CORES))"  # NUMA1 cores to avoid DGEMM
    CORE_LIST_ARM="4-$((3 + CORES))"     # cores 4-7

    # x86
    ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx-bench 2>/dev/null; \
        docker run -d --name nginx-bench --network=host \
        --cpuset-cpus=${CORE_LIST_X86} nginx:alpine"
    sleep 2
    wrk -t${WRK_THREADS} -c100 -d${DURATION}s \
        http://${FUJIAN_100G}/ > "${OUT_DIR}/core_x86_${CORES}c.txt" 2>&1
    ssh ${USER}@${FUJIAN_IP} "docker rm -f nginx-bench"

    # ARM
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx-bench 2>/dev/null; \
        docker run -d --name nginx-bench --network=host \
        --cpuset-cpus=${CORE_LIST_ARM} nginx:alpine'"
    sleep 2
    wrk -t${WRK_THREADS} -c100 -d${DURATION}s \
        http://${FUJIAN_BF2_FABRIC}/ > "${OUT_DIR}/core_arm_${CORES}c.txt" 2>&1
    ssh ${USER}@${FUJIAN_IP} "ssh root@${BF_IP} 'docker rm -f nginx-bench'"

    echo "  Done cores=${CORES}"
done

echo "I.2 done."
echo ""
echo "=== Experiment I Complete ==="
echo "Parse wrk output for req/s and latency from files in ${OUT_DIR}"
