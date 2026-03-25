#!/bin/bash
# Kubelet Reference Data Collection
# Measures idle CPU% and memory of kubelet (no pods scheduled) and
# slave_monitor side-by-side, for thesis comparison table.
#
# Run this on a node that is NOT running the main cluster experiments
# (to avoid interference). gnode2 is recommended.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

MEASURE_SECS=60
OUTPUT="${DATA_DIR}/kubelet"

echo "=== Kubelet Reference Data Collection ==="
echo "Measuring for ${MEASURE_SECS}s each"
echo ""

# ── Check kubelet installation ────────────────────────────────────────────────
if ! command -v kubelet &>/dev/null; then
    echo "kubelet not found. Install with:"
    echo "  sudo apt-get install -y kubelet"
    echo "  (or use the full kubeadm bootstrap if preferred)"
    echo ""
    echo "Skipping kubelet measurement. Run again after installation."
else
    echo "--- Measuring kubelet (idle, no pods) ---"
    # Start kubelet in standalone mode (no --kubeconfig means it runs but does nothing)
    sudo systemctl start kubelet 2>/dev/null || sudo kubelet --standalone &>/dev/null &
    KUBELET_BG_PID=$!
    echo "  Waiting 30s for kubelet to initialize..."
    sleep 30

    KUBELET_PID=$(pgrep -x kubelet | head -1)
    if [ -z "${KUBELET_PID}" ]; then
        echo "  ERROR: kubelet process not found after start"
    else
        echo "  kubelet PID=${KUBELET_PID}, sampling ${MEASURE_SECS}s..."
        pidstat -p "${KUBELET_PID}" -u -r 1 "${MEASURE_SECS}" \
            > "${OUTPUT}/kubelet_idle.txt" 2>&1

        # Extract averages
        awk '/kubelet/{cpu+=$8; mem+=$12; n++} END{
            printf "kubelet: avg CPU=%.3f%%  avg MEM=%.1f MB\n", cpu/n, mem/n/1024
        }' "${OUTPUT}/kubelet_idle.txt" | tee "${OUTPUT}/kubelet_summary.txt"
    fi

    sudo systemctl stop kubelet 2>/dev/null || kill "${KUBELET_BG_PID}" 2>/dev/null || true
    sleep 3
fi

# ── Measure slave_monitor overhead ───────────────────────────────────────────
echo ""
echo "--- Measuring slave_monitor (normal operation) ---"

"${SLAVE_MONITOR}" \
    --mode=offload \
    --pci="${GNODE2_PCI}" \
    --interval="${NORMAL_INTERVAL}" \
    --node-id="kubelet-compare-$(hostname)" \
    > "${OUTPUT}/slave_monitor.log" 2>&1 &
SLAVE_PID=$!
sleep 5   # let it register and stabilize

echo "  slave_monitor PID=${SLAVE_PID}, sampling ${MEASURE_SECS}s..."
pidstat -p "${SLAVE_PID}" -u -r 1 "${MEASURE_SECS}" \
    > "${OUTPUT}/slave_monitor_idle.txt" 2>&1

awk '/slave_monitor/{cpu+=$8; mem+=$12; n++} END{
    printf "slave_monitor: avg CPU=%.4f%%  avg MEM=%.2f MB\n", cpu/n, mem/n/1024
}' "${OUTPUT}/slave_monitor_idle.txt" | tee "${OUTPUT}/slave_monitor_summary.txt"

kill "${SLAVE_PID}" 2>/dev/null || true

# ── Print comparison ──────────────────────────────────────────────────────────
echo ""
echo "=== Overhead Comparison ==="
[ -f "${OUTPUT}/kubelet_summary.txt" ] && cat "${OUTPUT}/kubelet_summary.txt"
cat "${OUTPUT}/slave_monitor_summary.txt"
echo ""
echo "Data saved to: ${OUTPUT}/"
