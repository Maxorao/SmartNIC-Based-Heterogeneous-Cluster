#!/bin/bash
# exp_A_latency.sh — Experiment A: Tunnel Latency Measurement
#
# Measures one-way latency of:
#   1. DOCA Comch (PCIe kernel-bypass)  — for each message size in {64,256,1024,4096,65536}
#   2. Kernel TCP                        — same sizes
#
# Steps:
#   1. SSH to gnode2-bf and start bench_nic in both modes
#   2. On gnode2, run bench_host for all sizes and both modes
#   3. Collect CSV output files into DATA_DIR/A/
#   4. Print analysis reminder

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

OUT_DIR="${DATA_DIR}/A"
mkdir -p "${OUT_DIR}"

# -------------------------------------------------------------------------
# Cleanup on exit
# -------------------------------------------------------------------------
NIC_COMCH_PID=""
NIC_TCP_PID=""
cleanup() {
    echo "[exp_A] Cleaning up background processes..."
    # Kill bench_nic processes on SmartNIC
    if [[ -n "${NIC_COMCH_PID}" ]]; then
        ssh ${SSH_OPTS} "${SSH_USER}@${GNODE2_BF_IP}" \
            "kill ${NIC_COMCH_PID} 2>/dev/null || true" || true
    fi
    if [[ -n "${NIC_TCP_PID}" ]]; then
        ssh ${SSH_OPTS} "${SSH_USER}@${GNODE2_BF_IP}" \
            "kill ${NIC_TCP_PID} 2>/dev/null || true" || true
    fi
    echo "[exp_A] Done."
}
trap cleanup EXIT INT TERM

echo "========================================================"
echo "Experiment A: Tunnel Latency"
echo "Host:  gnode2 (${GNODE2_IP})"
echo "NIC:   gnode2-bf (${GNODE2_BF_IP})"
echo "PCI:   ${GNODE2_PCI}"
echo "Iters: ${BENCH_ITERS}"
echo "========================================================"

# -------------------------------------------------------------------------
# Step 1: Start bench_nic on gnode2-bf in Comch mode
# -------------------------------------------------------------------------
echo ""
echo "[exp_A] Starting bench_nic (comch mode) on gnode2-bf..."
ssh ${SSH_OPTS} "${SSH_USER}@${GNODE2_BF_IP}" \
    "nohup ${NIC_BENCH_NIC} --pci=${GNODE2_PCI} --mode=comch \
     > /tmp/bench_nic_comch.log 2>&1 & echo \$!" > /tmp/nic_comch.pid
NIC_COMCH_PID="$(cat /tmp/nic_comch.pid)"
echo "[exp_A] bench_nic (comch) started: PID ${NIC_COMCH_PID}"
sleep 2  # let the listener start

# -------------------------------------------------------------------------
# Step 2: Run bench_host (Comch mode) — all sizes
# -------------------------------------------------------------------------
echo ""
echo "[exp_A] Running bench_host --mode=comch --size=all ..."
"${BENCH_HOST}" \
    --pci="${GNODE2_PCI}" \
    --mode=comch \
    --size=all \
    --iters="${BENCH_ITERS}" \
    --nic-ip="${GNODE2_BF_IP}" \
    --output-dir="${OUT_DIR}"

echo "[exp_A] Comch measurements complete."

# Kill Comch bench_nic before starting TCP mode
ssh ${SSH_OPTS} "${SSH_USER}@${GNODE2_BF_IP}" \
    "kill ${NIC_COMCH_PID} 2>/dev/null || true" || true
NIC_COMCH_PID=""
sleep 1

# -------------------------------------------------------------------------
# Step 3: Start bench_nic on gnode2-bf in TCP mode
# -------------------------------------------------------------------------
echo ""
echo "[exp_A] Starting bench_nic (tcp mode) on gnode2-bf..."
ssh ${SSH_OPTS} "${SSH_USER}@${GNODE2_BF_IP}" \
    "nohup ${NIC_BENCH_NIC} --pci=${GNODE2_PCI} --mode=tcp \
     > /tmp/bench_nic_tcp.log 2>&1 & echo \$!" > /tmp/nic_tcp.pid
NIC_TCP_PID="$(cat /tmp/nic_tcp.pid)"
echo "[exp_A] bench_nic (tcp) started: PID ${NIC_TCP_PID}"
sleep 2

# -------------------------------------------------------------------------
# Step 4: Run bench_host (TCP mode) — all sizes
# -------------------------------------------------------------------------
echo ""
echo "[exp_A] Running bench_host --mode=tcp --size=all ..."
"${BENCH_HOST}" \
    --pci="${GNODE2_PCI}" \
    --mode=tcp \
    --size=all \
    --iters="${BENCH_ITERS}" \
    --nic-ip="${GNODE2_BF_IP}" \
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
