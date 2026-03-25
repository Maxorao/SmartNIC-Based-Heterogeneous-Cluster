#!/bin/bash
# Experiment D: Fault Recovery Test (Chapter 3)
# Measures fault detection time and recovery time for two scenarios:
#   Scenario 1: forward_routine crashes on SmartNIC (kill -9)
#   Scenario 2: slave_monitor restarts on a worker node (re-registration)
# Each scenario is repeated REPEAT times; results saved to DATA_DIR/D/

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

REPEAT=5            # repetitions per scenario
HEARTBEAT_MS=1000   # configured heartbeat interval (ms) — must match slave_monitor

# Target node for fault injection
FAULT_NODE="gnode2"
FAULT_NIC="gnode2-bf"    # SmartNIC hostname

echo "=== Experiment D: Fault Recovery Test ==="
echo "Fault node: ${FAULT_NODE} / ${FAULT_NIC}"
echo "Repetitions: ${REPEAT}"
echo ""

# ── Utility: get current time in milliseconds ────────────────────────────────
now_ms() { date +%s%3N; }

# ── Utility: wait until master log shows a specific pattern, with timeout ────
wait_for_log() {
    local pattern="$1"
    local timeout_s="$2"
    local log_file="${DATA_DIR}/C/master_monitor.log"
    local deadline=$(( $(now_ms) + timeout_s * 1000 ))
    while [ $(now_ms) -lt $deadline ]; do
        if grep -q "${pattern}" "${log_file}" 2>/dev/null; then
            echo $(now_ms)
            return 0
        fi
        sleep 0.1
    done
    echo "-1"  # timeout
    return 1
}

# ── Scenario 1: forward_routine crash on SmartNIC ────────────────────────────
echo "--- Scenario 1: forward_routine crash (kill -9) ---"
echo "timestamp_ms,detection_ms,recovery_ms" > "${DATA_DIR}/D/scenario1.csv"

for i in $(seq 1 "${REPEAT}"); do
    echo "  Run ${i}/${REPEAT}:"

    # Mark log position before fault injection
    LOG_MARK=$(wc -l < "${DATA_DIR}/C/master_monitor.log" 2>/dev/null || echo 0)
    T_FAULT=$(now_ms)

    # Inject fault: kill forward_routine on SmartNIC
    ssh "${FAULT_NIC}" "kill -9 \$(pgrep forward_routine)" 2>/dev/null || true
    echo "    t=${T_FAULT}: forward_routine killed"

    # Wait for master to log gnode2 as offline (heartbeat timeout = 5×interval)
    T_DETECT=$(wait_for_log "node.*gnode2.*offline\|gnode2.*timeout\|gnode2.*lost" 30)
    if [ "${T_DETECT}" = "-1" ]; then
        echo "    WARNING: detection timeout (>30s)"
        T_DETECT="-1"
        DET_MS="-1"
    else
        DET_MS=$(( T_DETECT - T_FAULT ))
        echo "    t=${T_DETECT}: gnode2 detected offline (detection=${DET_MS}ms)"
    fi

    # Restart forward_routine on SmartNIC
    ssh "${FAULT_NIC}" "nohup ${FORWARD_ROUTINE} \
        --pci=${GNODE2_NIC_PCI:-00:00.0} \
        --master-ip=${MASTER_IP} \
        --master-port=${MASTER_PORT} \
        > /tmp/forward_routine.log 2>&1 &" 2>/dev/null

    # Wait for master to log gnode2 as back online
    T_RECOVER=$(wait_for_log "node.*gnode2.*online\|gnode2.*registered\|gnode2.*reconnected" 30)
    if [ "${T_RECOVER}" = "-1" ]; then
        echo "    WARNING: recovery timeout (>30s)"
        REC_MS="-1"
    else
        REC_MS=$(( T_RECOVER - T_FAULT ))
        echo "    t=${T_RECOVER}: gnode2 recovered (total downtime=${REC_MS}ms)"
    fi

    echo "${T_FAULT},${DET_MS},${REC_MS}" >> "${DATA_DIR}/D/scenario1.csv"
    sleep 10   # let cluster stabilize before next run
done

# ── Scenario 2: slave_monitor restart (re-registration) ─────────────────────
echo ""
echo "--- Scenario 2: slave_monitor restart on gnode3 ---"
echo "timestamp_ms,reregister_ms" > "${DATA_DIR}/D/scenario2.csv"

for i in $(seq 1 "${REPEAT}"); do
    echo "  Run ${i}/${REPEAT}:"

    T_RESTART=$(now_ms)
    ssh "gnode3" "pkill slave_monitor; sleep 1; \
        nohup ${SLAVE_MONITOR} --mode=offload --pci=${GNODE3_PCI:-03:00.0} \
        > /tmp/slave_monitor.log 2>&1 &" 2>/dev/null
    echo "    t=${T_RESTART}: slave_monitor restarted on gnode3"

    # Wait for master to log gnode3 re-registered (same node_id, not new)
    T_REREG=$(wait_for_log "gnode3.*re-registered\|gnode3.*already.*known\|gnode3.*online" 30)
    if [ "${T_REREG}" = "-1" ]; then
        echo "    WARNING: re-registration timeout"
        REREG_MS="-1"
    else
        REREG_MS=$(( T_REREG - T_RESTART ))
        echo "    t=${T_REREG}: gnode3 re-registered (${REREG_MS}ms)"
    fi

    echo "${T_RESTART},${REREG_MS}" >> "${DATA_DIR}/D/scenario2.csv"
    sleep 10
done

# ── Print summary ─────────────────────────────────────────────────────────────
echo ""
echo "=== Scenario 1 Results (forward_routine crash) ==="
awk -F',' 'NR>1 && $2!=-1 {det+=$2; rec+=$3; n++} END{
    printf "  avg detection = %.0f ms\n  avg recovery  = %.0f ms\n  n = %d\n", det/n, rec/n, n
}' "${DATA_DIR}/D/scenario1.csv"

echo ""
echo "=== Scenario 2 Results (slave_monitor restart) ==="
awk -F',' 'NR>1 && $2!=-1 {s+=$2; n++} END{
    printf "  avg re-registration = %.0f ms\n  n = %d\n", s/n, n
}' "${DATA_DIR}/D/scenario2.csv"
