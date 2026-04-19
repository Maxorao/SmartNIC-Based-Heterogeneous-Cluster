#!/bin/bash
# ch5_exp_K_cgroups.sh — Adds configuration K.4 (cgroups-isolated baseline) to K.
#
# Rationale: review pointed out that K.1 (8 TCP agents + Nginx co-pinned to
# DGEMM cores) is a pathological baseline. A fair "data-center standard" baseline
# should use cgroups v2 to partition CPU exclusive sets:
#   - DGEMM cpuset: cores 0-15 (NUMA0)
#   - Nginx cpuset: cores 16-31 (NUMA1), exclusive (DGEMM cannot encroach)
# This approximates what a well-tuned Kubernetes deployment with static CPU
# management would do on a single node.
#
# K.4 result will be compared side-by-side with K.1 (worst case) and K.3 (full system).
#
# Usage (after running ch5_exp_K_e2e.sh first to have K.1/K.2/K.3):
#   bash ch5_exp_K_cgroups.sh

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION="${DURATION:-60}"
WARMUP=5
WRK_THREADS=4
WRK_CONCURRENCY=200
VIP="192.168.56.200"
OUT_DIR="${DATA_DIR}/K"
mkdir -p "$OUT_DIR"

FUJIAN_SSH="${USER}@${FUJIAN_IP}"

echo "========================================================"
echo " K.4: cgroups v2 exclusive cpuset baseline"
echo " DGEMM: cores 0-15 | Nginx: cores 16-31 (exclusive)"
echo "========================================================"

# ---------------------------------------------------------------------------
# Check cgroups v2 availability
# ---------------------------------------------------------------------------
CGROUPS_OK=$(ssh ${FUJIAN_SSH} "mount | grep -q 'cgroup2.*cgroup2' && echo yes || echo no")
if [ "${CGROUPS_OK}" != "yes" ]; then
    echo "[ERROR] cgroups v2 not mounted on ${FUJIAN_IP}"
    echo "  Ensure kernel cmdline has: systemd.unified_cgroup_hierarchy=1"
    exit 1
fi

# Clean up any leftover state
cleanup_k4() {
    ssh ${FUJIAN_SSH} "docker rm -f nginx 2>/dev/null; \
        pkill -f gemm_bench 2>/dev/null; \
        systemd-run --wait --property=CPUAccounting=true true 2>/dev/null || true" 2>/dev/null || true
    ssh ${FUJIAN_SSH} "sudo ip addr del ${VIP}/24 dev enp94s0f1np1 2>/dev/null" || true
}
trap cleanup_k4 EXIT INT TERM
cleanup_k4

# ---------------------------------------------------------------------------
# Start Nginx in its own cgroup slice with exclusive cpuset 16-31
# ---------------------------------------------------------------------------
echo "[K.4] Starting Nginx in nginx.slice (cpuset=16-31, exclusive)..."
ssh ${FUJIAN_SSH} "docker rm -f nginx 2>/dev/null; \
    docker run -d --name nginx --network=host \
    --cpuset-cpus=16-31 --cpu-shares=512 \
    nginx:alpine"

# Assign VIP to fujian host so wrk can target the same VIP as K.1
ssh ${FUJIAN_SSH} "sudo ip addr add ${VIP}/24 dev enp94s0f1np1 2>/dev/null || true"
ssh ${FUJIAN_SSH} "sudo arping -c 3 -A -I enp94s0f1np1 ${VIP} &>/dev/null &" 2>/dev/null || true
sleep 3

# ---------------------------------------------------------------------------
# Start DGEMM in its own cgroup slice with exclusive cpuset 0-15
# ---------------------------------------------------------------------------
echo "[K.4] Running DGEMM in dgemm.slice (cpuset=0-15, exclusive) for ${DURATION}s..."
ssh ${FUJIAN_SSH} "sudo systemd-run --scope --slice=dgemm.slice \
    --property=AllowedCPUs=0-15 \
    --property=CPUWeight=200 \
    sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_k4.txt -- \
    env OMP_NUM_THREADS=${GEMM_THREADS} taskset -c 0-15 ${GEMM_BENCH} \
    --size=1024 --duration=${DURATION}" \
    > "${OUT_DIR}/k4_gemm.txt" 2>&1 &
GEMM_PID=$!

# Run wrk against VIP (host interface — Nginx is on NUMA1 via cgroups)
sleep ${WARMUP}
wrk -t${WRK_THREADS} -c${WRK_CONCURRENCY} -d$((DURATION - WARMUP))s \
    --latency \
    http://${VIP}/ > "${OUT_DIR}/k4_wrk.txt" 2>&1

wait $GEMM_PID 2>/dev/null || true
ssh ${FUJIAN_SSH} "cat /tmp/perf_k4.txt" > "${OUT_DIR}/k4_perf.txt"

cleanup_k4

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "[K.4] Done. Comparing against K.1/K.2/K.3:"
python3 "${SCRIPT_DIR}/analyze/emit_summary.py" \
    --exp K --data-dir "${OUT_DIR}" --out "${OUT_DIR}/summary_with_cgroups.csv" || true

if [ -f "${OUT_DIR}/summary_with_cgroups.csv" ]; then
    echo ""
    cat "${OUT_DIR}/summary_with_cgroups.csv"
fi

echo ""
echo "Key comparison:"
echo "  K.1 (pathological)     → see k1_gemm.txt"
echo "  K.4 (cgroups isolated) → see k4_gemm.txt (this run)"
echo "  K.3 (full system)      → see k3_gemm.txt"
echo ""
echo "Expected narrative: K.3/K.1 ≈ 2.27×; K.3/K.4 is the speedup vs a"
echo "fair SOTA baseline (likely much smaller, e.g. 1.1-1.4×)."
