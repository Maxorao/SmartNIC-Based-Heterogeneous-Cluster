#!/bin/bash
# Experiment C: Control-Plane Scalability Test (multi-host)
#
# Topology:
#   tianjin (192.168.56.10) — master_monitor
#   fujian  (192.168.56.11) — mock_slave (N/2 threads)
#   helong  (192.168.56.12) — mock_slave (N/2 threads)
#
# All mock nodes connect via 100G fabric (192.168.56.x).
# Measures master_monitor CPU%, RSS, and mock_slave report latency
# at scale points: 4, 16, 64, 256 nodes.
#
# Usage:
#   bash scripts/exp_C_scale.sh [--interval=1000]
#
# Run from tianjin.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

# ── Parameters ──────────────────────────────────────────────────────────────
WARMUP=10               # seconds — let all nodes register
MEASURE=30              # seconds — pidstat sampling window
SETTLE=5                # seconds — between scale points
REPORT_INTERVAL="${1:-1000}"   # ms — default 1s per node

# Strip --interval= prefix if passed
REPORT_INTERVAL="${REPORT_INTERVAL#--interval=}"

NODE_COUNTS=(4 16 64 256)
WORKERS=("172.28.4.77" "172.28.4.85")   # fujian, helong
MASTER_ADDR="${MASTER_100G}"             # 192.168.56.10

SSH_USER="$(whoami)"
SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10"
TOTAL_DURATION=$(( WARMUP + MEASURE + 10 ))  # mock_slave runtime per point

echo "=== Experiment C: Scalability Test ==="
echo "Master:   ${MASTER_ADDR}:${MASTER_PORT}"
echo "Workers:  ${WORKERS[*]}"
echo "Scales:   ${NODE_COUNTS[*]}"
echo "Interval: ${REPORT_INTERVAL} ms"
echo "Duration: warmup=${WARMUP}s measure=${MEASURE}s"
echo ""

mkdir -p "${DATA_DIR}/C"

# ── Helper: cleanup between runs ────────────────────────────────────────────
cleanup_all() {
    # Kill mock_slave on workers
    for w in "${WORKERS[@]}"; do
        ${SSH} ${SSH_USER}@${w} "pkill -f mock_slave 2>/dev/null" || true
    done
    # Kill master_monitor locally
    pkill -f master_monitor 2>/dev/null || true
    sleep 2
}

# ── Main loop ───────────────────────────────────────────────────────────────
for N in "${NODE_COUNTS[@]}"; do
    echo "──────────────────────────────────────────────"
    echo "  Scale: ${N} nodes (interval=${REPORT_INTERVAL}ms)"
    echo "──────────────────────────────────────────────"

    # Clean slate
    cleanup_all

    # Start master_monitor
    "${MASTER_MONITOR}" --port="${MASTER_PORT}" \
        > "${DATA_DIR}/C/master_${N}nodes.log" 2>&1 &
    MPID=$!
    sleep 2
    echo "  master_monitor PID=${MPID}"

    # Split N across 2 workers
    N_PER_WORKER=$(( N / 2 ))
    N_REMAINDER=$(( N % 2 ))

    # Launch mock_slave on each worker
    WORKER_IDX=0
    for w in "${WORKERS[@]}"; do
        THIS_N=${N_PER_WORKER}
        # Give remainder to first worker
        if [ ${WORKER_IDX} -eq 0 ] && [ ${N_REMAINDER} -gt 0 ]; then
            THIS_N=$(( THIS_N + N_REMAINDER ))
        fi

        echo "  Starting ${THIS_N} mock nodes on ${w}..."
        ${SSH} ${SSH_USER}@${w} "
            ~/experiments/bench/mock_slave/mock_slave \
                --master-ip=${MASTER_ADDR} \
                --master-port=${MASTER_PORT} \
                --nodes=${THIS_N} \
                --interval=${REPORT_INTERVAL} \
                --duration=${TOTAL_DURATION}
        " > "${DATA_DIR}/C/mock_${N}nodes_${w}.txt" 2>&1 &

        WORKER_IDX=$(( WORKER_IDX + 1 ))
    done

    # Wait for all nodes to register
    echo "  Warming up ${WARMUP}s..."
    sleep ${WARMUP}

    # Measure master_monitor CPU + memory
    echo "  Measuring master_monitor for ${MEASURE}s..."
    pidstat -p ${MPID} -u -r 1 ${MEASURE} \
        > "${DATA_DIR}/C/pidstat_${N}nodes.txt" 2>&1 || true

    # Wait for mock_slaves to finish
    echo "  Waiting for mock_slaves to exit..."
    wait 2>/dev/null || true

    # ── Extract quick stats ─────────────────────────────────────────────
    # CPU average from pidstat
    AVG_CPU=$(awk '/%CPU/ && !/^#/ {next} /master_monitor/ && NF>=8 {s+=$8; n++}
              END{if(n>0) printf "%.2f", s/n; else print "N/A"}' \
              "${DATA_DIR}/C/pidstat_${N}nodes.txt" 2>/dev/null || echo "N/A")

    # RSS average from pidstat (KB → MB)
    AVG_RSS=$(awk '/RSS/ && !/^#/ {next} /master_monitor/ && NF>=7 {s+=$7; n++}
              END{if(n>0) printf "%.1f", s/n/1024; else print "N/A"}' \
              "${DATA_DIR}/C/pidstat_${N}nodes.txt" 2>/dev/null || echo "N/A")

    # Aggregate latency from mock_slave outputs
    TOTAL_LINE=""
    for w in "${WORKERS[@]}"; do
        f="${DATA_DIR}/C/mock_${N}nodes_${w}.txt"
        [ -f "$f" ] && TOTAL_LINE="${TOTAL_LINE}$(grep '^TOTAL:' "$f" 2>/dev/null || true) "
    done

    echo ""
    echo "  Results for ${N} nodes:"
    echo "    master CPU: ${AVG_CPU}%"
    echo "    master RSS: ${AVG_RSS} MB"
    echo "    mock_slave: ${TOTAL_LINE}"
    echo ""

    # Stop master_monitor
    kill ${MPID} 2>/dev/null || true
    wait ${MPID} 2>/dev/null || true
    sleep ${SETTLE}
done

echo "=== Experiment C complete ==="
echo ""
echo "Data files in ${DATA_DIR}/C/:"
ls -la "${DATA_DIR}/C/"
echo ""
echo "Run analysis: python3 ${SCRIPT_DIR}/analyze/analyze_C.py"
