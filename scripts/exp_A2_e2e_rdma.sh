#!/bin/bash
# exp_A2_e2e_rdma.sh — End-to-end Comch→RDMA→Comch latency measurement.
#
# Topology:
#   tianjin host (pinger) ↔ Comch ↔ tianjin BF2 (e2e_nic client) ↔ RDMA ↔
#                                    fujian BF2 (e2e_nic server) ↔ Comch ↔ fujian host (ponger)
#
# Prerequisites:
#   - bf2_rdma_setup.sh has run on tianjin (role A) and fujian (role B) BF2s
#   - e2e_host binary built on both hosts (x86 target)
#   - e2e_nic binary built on both BF2s (BF2 target)
#   - Verified via bf2_cluster_verify.sh

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

OUT_DIR="${DATA_DIR}/A_e2e"
mkdir -p "$OUT_DIR"

SIZES="${SIZES:-64 128 256 1024}"
ITERS="${ITERS:-10000}"
PORT=7888
SERVICE="e2e-latency"

# SF IPs (must match bf2_rdma_setup.sh)
TIANJIN_SF_IP="192.168.56.102"
FUJIAN_SF_IP="192.168.56.103"

echo "=========================================="
echo " Experiment A2: End-to-end Comch-RDMA-Comch latency"
echo "=========================================="

cleanup() {
    echo "[exp_A2] Cleaning up..."
    ssh "${USER}@${FUJIAN_IP}" "pkill -f e2e_host 2>/dev/null; \
        ssh root@${BF_IP} 'pkill -f e2e_nic 2>/dev/null'" 2>/dev/null || true
    ssh "root@${BF_IP}" "pkill -f e2e_nic 2>/dev/null" 2>/dev/null || true
    pkill -f e2e_host 2>/dev/null || true
    sleep 1
}
trap cleanup EXIT INT TERM
cleanup

# Startup order matters: fujian BF2 server blocks on RDMA accept until
# tianjin BF2 client connects, so its Comch isn't up until then. Hosts can
# only attach Comch after their local BF2 has started Comch.
#
# Order: fujian BF2 server -> tianjin BF2 client -> fujian ponger -> tianjin pinger

# ---------------------------------------------------------------------------
# Step 1: Start server-side NIC forwarder on fujian BF2
# ---------------------------------------------------------------------------
echo ""
echo "[step 1] Starting e2e_nic server on fujian BF2 (${FUJIAN_SF_IP}:${PORT})..."
ssh "${USER}@${FUJIAN_IP}" \
    "ssh root@${BF_IP} 'nohup /root/experiments/build/bench/rdma_e2e/e2e_nic \
      --mode=server --bind-ip=${FUJIAN_SF_IP} --port=${PORT} \
      --dev-pci=${NIC_PCI} --service=${SERVICE} \
      > /tmp/e2e_nic_server.log 2>&1 &'"
sleep 2

# ---------------------------------------------------------------------------
# Step 2: Start NIC client forwarder on tianjin BF2 (triggers RDMA connect,
#         which unblocks fujian BF2 to start its Comch listener)
# ---------------------------------------------------------------------------
echo "[step 2] Starting e2e_nic client on tianjin BF2 (→${FUJIAN_SF_IP}:${PORT})..."
ssh "root@${BF_IP}" \
    "nohup /root/experiments/build/bench/rdma_e2e/e2e_nic \
      --mode=client --peer-ip=${FUJIAN_SF_IP} --port=${PORT} \
      --dev-pci=${NIC_PCI} --service=${SERVICE} \
      > /tmp/e2e_nic_client.log 2>&1 &"
sleep 4

# ---------------------------------------------------------------------------
# Step 3: Start ponger on fujian host (Comch endpoint, echoes back)
# ---------------------------------------------------------------------------
echo "[step 3] Starting e2e_host ponger on fujian host..."
ssh "${USER}@${FUJIAN_IP}" \
    "nohup sudo ${EXP_BASE}/build/bench/rdma_e2e/e2e_host \
      --mode=ponger --pci=${HOST_PCI} --service=${SERVICE} \
      > /tmp/e2e_host_ponger.log 2>&1 &"
sleep 3

# ---------------------------------------------------------------------------
# Step 4: Run pinger on tianjin host for each message size
# ---------------------------------------------------------------------------
for size in ${SIZES}; do
    echo ""
    echo "[step 4] Running pinger size=${size} iters=${ITERS}..."
    OUT_CSV="${OUT_DIR}/e2e_size${size}.csv"
    sudo "${EXP_BASE}/build/bench/rdma_e2e/e2e_host" \
        --mode=pinger --pci="${HOST_PCI}" \
        --size="${size}" --iters="${ITERS}" \
        --output="${OUT_CSV}" --service="${SERVICE}" \
        2>&1 | tee "${OUT_DIR}/e2e_size${size}.log"
done

# ---------------------------------------------------------------------------
# Step 5: Aggregate
# ---------------------------------------------------------------------------
echo ""
echo "[step 5] Aggregating..."
python3 "${SCRIPT_DIR}/analyze/analyze_A_e2e.py" --data-dir "${OUT_DIR}"

echo ""
echo "=== Experiment A2 Complete ==="
echo "Results in: ${OUT_DIR}"
