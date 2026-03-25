#!/bin/bash
# Experiment C: Master-Monitor Scalability Test
# Simulates 4 / 16 / 64 / 256 nodes via mock_slave and measures
# master_monitor CPU usage and P99 control-message latency.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

WARMUP_SECS=10     # wait after launching mocks before measuring
MEASURE_SECS=30    # pidstat sampling duration
NODE_COUNTS=(4 16 64 256)

echo "=== Experiment C: Scalability Test ==="
echo "Master: ${MASTER_IP}:${MASTER_PORT}"
echo "Node counts: ${NODE_COUNTS[*]}"
echo ""

# Ensure master_monitor is running
if ! pgrep -x master_monitor > /dev/null; then
    echo "Starting master_monitor..."
    "${MASTER_MONITOR}" \
        --port="${MASTER_PORT}" \
        --db-connstr="${DB_CONNSTR:-host=localhost dbname=cluster_metrics user=postgres}" \
        > "${DATA_DIR}/C/master_monitor.log" 2>&1 &
    MASTER_PID=$!
    echo "  master_monitor PID=${MASTER_PID}"
    sleep 3
else
    MASTER_PID=$(pgrep -x master_monitor)
    echo "  master_monitor already running (PID=${MASTER_PID})"
fi

for N in "${NODE_COUNTS[@]}"; do
    echo ""
    echo "--- Testing ${N} nodes ---"

    # Launch N mock_slave processes (each simulates one node)
    # mock_slave uses internal pthreads for N nodes, so one process suffices
    "${MOCK_SLAVE}" \
        --master-ip="${MASTER_IP}" \
        --master-port="${MASTER_PORT}" \
        --nodes="${N}" \
        --duration=$(( WARMUP_SECS + MEASURE_SECS + 5 )) \
        > "${DATA_DIR}/C/mock_slave_${N}nodes.log" 2>&1 &
    MOCK_PID=$!

    echo "  Waiting ${WARMUP_SECS}s for ${N} nodes to register..."
    sleep "${WARMUP_SECS}"

    # Measure master_monitor CPU and memory
    # pidstat columns: Time UID PID %usr %system %guest %wait %CPU CPU Command
    echo "  Sampling master_monitor for ${MEASURE_SECS}s..."
    pidstat -p "${MASTER_PID}" -u -r 1 "${MEASURE_SECS}" \
        > "${DATA_DIR}/C/master_${N}nodes.txt" 2>&1

    # Extract master's P99 latency from its log (last N lines)
    grep -i "p99_latency" "${DATA_DIR}/C/master_monitor.log" | tail -5 \
        > "${DATA_DIR}/C/latency_${N}nodes.txt" 2>/dev/null || true

    # Quick CPU summary
    avg_cpu=$(awk '/master_monitor/{s+=$8; n++} END{if(n>0) printf "%.3f", s/n; else print "N/A"}' \
        "${DATA_DIR}/C/master_${N}nodes.txt")
    echo "  master_monitor avg CPU: ${avg_cpu}%"

    # Stop mock_slave and wait for cleanup
    kill "${MOCK_PID}" 2>/dev/null || true
    wait "${MOCK_PID}" 2>/dev/null || true
    sleep 5
done

echo ""
echo "=== Scalability test complete ==="
echo "Run: python3 ${SCRIPT_DIR}/analyze/analyze_C.py"
