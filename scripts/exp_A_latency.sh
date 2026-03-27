#!/bin/bash
# exp_A_latency.sh — Experiment A: Tunnel Latency Measurement
#
# Adapted for tianjin single-host + BF2 (192.168.100.2) setup.
#
# Measures one-way latency of:
#   1. DOCA Comch (PCIe kernel-bypass)  — for each message size
#   2. Kernel TCP                        — same sizes

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

OUT_DIR="${DATA_DIR}/A"
mkdir -p "${OUT_DIR}"

# -------------------------------------------------------------------------
# Cleanup on exit
# -------------------------------------------------------------------------
cleanup() {
    echo "[exp_A] Cleaning up background processes..."
    ssh root@${BF_IP} "pkill -f bench_nic 2>/dev/null || true" || true
    echo "[exp_A] Done."
}
trap cleanup EXIT INT TERM

echo "========================================================"
echo "Experiment A: Tunnel Latency"
echo "Host:  tianjin (${HOST_IP})"
echo "NIC:   BF2 (${BF_IP})"
echo "PCI:   ${HOST_PCI}"
echo "Iters: ${BENCH_ITERS}"
echo "========================================================"

# -------------------------------------------------------------------------
# Step 1: Start bench_nic on BF2 in Comch mode
# -------------------------------------------------------------------------
echo ""
echo "[exp_A] Starting bench_nic (comch mode) on BF2..."
ssh root@${BF_IP} "pkill -f bench_nic 2>/dev/null || true" || true
sleep 1
ssh root@${BF_IP} \
    "nohup ${NIC_BENCH_NIC} --pci=${NIC_PCI} --mode=comch \
     > /tmp/bench_nic_comch.log 2>&1 &"
sleep 3  # let the listener start

# Verify it started
ssh root@${BF_IP} "pgrep -f bench_nic > /dev/null" || {
    echo "[exp_A] ERROR: bench_nic failed to start on BF2"
    ssh root@${BF_IP} "cat /tmp/bench_nic_comch.log"
    exit 1
}
echo "[exp_A] bench_nic (comch) started"

# -------------------------------------------------------------------------
# Step 2: Run bench_host (Comch mode) — all sizes
# -------------------------------------------------------------------------
echo ""
echo "[exp_A] Running bench_host --mode=comch --size=all ..."
sudo "${BENCH_HOST}" \
    --pci="${HOST_PCI}" \
    --mode=comch \
    --size=all \
    --iters="${BENCH_ITERS}" \
    --nic-ip="${BF_IP}" \
    --output-dir="${OUT_DIR}"

echo "[exp_A] Comch measurements complete."

# Kill Comch bench_nic before starting TCP mode
ssh root@${BF_IP} "pkill -f bench_nic 2>/dev/null || true" || true
sleep 1

# -------------------------------------------------------------------------
# Step 3: Start bench_nic on BF2 in TCP mode
# -------------------------------------------------------------------------
echo ""
echo "[exp_A] Starting bench_nic (tcp mode) on BF2..."
ssh root@${BF_IP} \
    "nohup ${NIC_BENCH_NIC} --pci=${NIC_PCI} --mode=tcp \
     > /tmp/bench_nic_tcp.log 2>&1 &"
sleep 2

ssh root@${BF_IP} "pgrep -f bench_nic > /dev/null" || {
    echo "[exp_A] ERROR: bench_nic (tcp) failed to start on BF2"
    ssh root@${BF_IP} "cat /tmp/bench_nic_tcp.log"
    exit 1
}
echo "[exp_A] bench_nic (tcp) started"

# -------------------------------------------------------------------------
# Step 4: Run bench_host (TCP mode) — all sizes
# -------------------------------------------------------------------------
echo ""
echo "[exp_A] Running bench_host --mode=tcp --size=all ..."
"${BENCH_HOST}" \
    --pci="${HOST_PCI}" \
    --mode=tcp \
    --size=all \
    --iters="${BENCH_ITERS}" \
    --nic-ip="${BF_IP}" \
    --output-dir="${OUT_DIR}"

echo "[exp_A] TCP measurements complete."

# -------------------------------------------------------------------------
# Step 5: List collected data
# -------------------------------------------------------------------------
echo ""
echo "[exp_A] Collected CSV files in ${OUT_DIR}:"
ls -lh "${OUT_DIR}"/*.csv 2>/dev/null || echo "  (no CSV files found)"

# -------------------------------------------------------------------------
# Step 6: Reminder
# -------------------------------------------------------------------------
echo ""
echo "========================================================"
echo "Experiment A complete."
echo "Run analysis:"
echo "  python3 ${SCRIPT_DIR}/analyze/analyze_A.py"
echo "========================================================"
