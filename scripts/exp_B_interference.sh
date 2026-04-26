#!/bin/bash
# =========================================================================
# Experiment B: Control Plane Interference Elimination (new architecture)
#
# Hypothesis: routing the per-node monitoring stack through PCIe Comch +
# slave_agent on BF2 removes the kubelet/metric agent from host CPU, so
# DGEMM throughput recovers toward the baseline. In the no-offload path the
# same agent goes direct gRPC, competing with the workload.
#
# Three scenarios, each runs gemm_bench for ${DURATION}s pinned to
# ${COMPUTE_CORES}:
#
#   Scenario 1 (baseline):     gemm alone.
#   Scenario 2 (no offload):   gemm + monitoring stack on host.
#                              Stack = 1x metric_push (1s, gRPC fallback) +
#                                      N_SIM x agent_simulator.py
#                              (health-probe, log-collector, node-exporter,
#                               kube-proxy …) — together approximate a
#                              real kubelet sidecar set.
#                              slave_agent is stopped on BF2 so metric_push
#                              falls back to gRPC over the kernel TCP stack.
#   Scenario 3 (full BF2 offload): only metric_push on host (Comch path),
#                                   no agent_simulator processes. The
#                                   simulator load is conceptually absorbed
#                                   by slave_agent on the BF2 (the
#                                   architectural target state where the
#                                   entire monitoring stack runs on BF2).
#
# Note: DOCA Comch on BF2 uses a single representor, so at most one host
# process can hold the Comch service. metric_push is the canonical
# offloaded agent; the simulators always stay on host.
#
# CPU binding: cores ${COMPUTE_CORES} (default 48-63 on fujian — NUMA node1,
# socket 1). Both gemm_bench and the host-side monitoring stack are pinned
# here, forcing them to compete for the same physical cores. Cores 0-15 are
# avoided so default system processes / softirqs do not contaminate the
# measurement.
#
# Run: bash scripts/exp_B_interference.sh
#   Override: EXP_DURATION (default 60), COMPUTE_CORES (default 48-63),
#             N_SIM (default 4), EXP_INTERVAL_MS (default 1000).
# =========================================================================

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------
DURATION="${EXP_DURATION:-${GEMM_DURATION:-60}}"
WARMUP_SECS=5
COMPUTE_CORES="${COMPUTE_CORES:-48-63}"     # NUMA node1, away from system procs
EXP_INTERVAL_MS="${EXP_INTERVAL_MS:-1000}"  # metric_push at 1Hz, like a real kubelet
N_SIM="${N_SIM:-4}"                          # number of agent_simulator processes
SIM_INTERVAL_S="${SIM_INTERVAL_S:-0.1}"      # simulator loop period
SIM_SAMPLE_PIDS="${SIM_SAMPLE_PIDS:-50}"     # /proc/<pid>/stat reads per cycle

# Roles assigned to each simulator instance — purely cosmetic for logs.
SIM_ROLES=(kubelet-extra health-probe log-collector node-exporter \
           kube-proxy container-runtime ingress-controller cni-plugin)

# Use mgmt LAN (1G) for the no-offload fallback path. Host-to-host 100G is
# not configured here; in production this matches the kubelet→apiserver path.
MASTER_FALLBACK_ADDR="${MASTER_HOST}:${GRPC_PORT}"

REMOTE_SIM_SCRIPT="/home/huaz/experiments/scripts/agent_simulator.py"

OUT_DIR="${DATA_DIR}/B"
mkdir -p "$OUT_DIR"

FUJIAN_SSH="${USER}@${FUJIAN_IP}"

echo "============================================================"
echo " Experiment B: Control Plane Interference Elimination"
echo " Duration per scenario: ${DURATION}s | Cores: ${COMPUTE_CORES}"
echo " metric_push: 1x at ${EXP_INTERVAL_MS}ms"
echo " agent_simulator: ${N_SIM}x at ${SIM_INTERVAL_S}s, ${SIM_SAMPLE_PIDS} pids/cycle"
echo " Fallback master: ${MASTER_FALLBACK_ADDR}"
echo " Output: ${OUT_DIR}"
echo "============================================================"

cleanup_all() {
    echo "[cleanup] stopping all components..."
    ssh "${FUJIAN_SSH}" "pkill -f metric_push 2>/dev/null; \
                         pkill -f gemm_bench 2>/dev/null; \
                         pkill -f agent_simulator 2>/dev/null" 2>/dev/null || true
    ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} 'pkill -f slave_agent 2>/dev/null'" 2>/dev/null || true
    pkill -f cluster_master 2>/dev/null || true
    sleep 2
}

start_simulators() {
    local label="$1"
    for i in $(seq 1 "${N_SIM}"); do
        local role="${SIM_ROLES[$((i-1))]:-agent-${i}}"
        ssh "${FUJIAN_SSH}" "SIM_ROLE=${role} \
                             SIM_INTERVAL_S=${SIM_INTERVAL_S} \
                             SIM_SAMPLE_PIDS=${SIM_SAMPLE_PIDS} \
                             SIM_LOG=/tmp/expB_sim_${label}_${role}.log \
                             nohup taskset -c ${COMPUTE_CORES} \
                             python3 ${REMOTE_SIM_SCRIPT} \
                             > /tmp/expB_sim_${label}_${role}.stderr 2>&1 < /dev/null &"
    done
}

stop_simulators() {
    ssh "${FUJIAN_SSH}" "pkill -f agent_simulator 2>/dev/null" 2>/dev/null || true
}

trap cleanup_all EXIT INT TERM
cleanup_all

# ---------------------------------------------------------------------------
# Common: cluster_master on tianjin
# ---------------------------------------------------------------------------
echo "[B] Starting cluster_master on tianjin..."
nohup "${CLUSTER_MASTER}" \
    --grpc-port "${GRPC_PORT}" \
    --http-port "${HTTP_STATUS_PORT}" \
    --db-connstr "${DB_CONNSTR}" \
    > "${OUT_DIR}/master.log" 2>&1 &
MASTER_PID=$!
sleep 3
if ! kill -0 "${MASTER_PID}" 2>/dev/null; then
    echo "[B] ERROR: cluster_master died at startup. Last log:"
    tail -30 "${OUT_DIR}/master.log"
    exit 1
fi

# ---------------------------------------------------------------------------
# Helper to run gemm with perf and write outputs into ${OUT_DIR}/scenario${N}_*
# ---------------------------------------------------------------------------
run_gemm_scenario() {
    local scenario_id="$1"
    local description="$2"

    local gflops_csv="${OUT_DIR}/scenario${scenario_id}_gflops.csv"
    local gflops_txt="${OUT_DIR}/scenario${scenario_id}_gflops.txt"
    local perf_txt="${OUT_DIR}/scenario${scenario_id}_perf.txt"

    echo "--- Scenario ${scenario_id}: ${description} ---"

    ssh "${FUJIAN_SSH}" "sudo rm -f /tmp/gflops_${scenario_id}.csv /tmp/perf_${scenario_id}.txt"

    # Use the installed perf binary directly (system-wide /usr/bin/perf may
    # complain about kernel version mismatch even when a valid perf exists
    # under /usr/lib/linux-tools-*).
    local PERF_BIN
    PERF_BIN=$(ssh "${FUJIAN_SSH}" "ls /usr/lib/linux-tools-*/perf 2>/dev/null | head -1")
    [ -z "${PERF_BIN}" ] && PERF_BIN="perf"

    ssh "${FUJIAN_SSH}" "sudo ${PERF_BIN} stat -e LLC-load-misses,LLC-loads,context-switches,instructions \
        -o /tmp/perf_${scenario_id}.txt -I 1000 -- \
        env OMP_NUM_THREADS=${GEMM_THREADS} \
        taskset -c ${COMPUTE_CORES} ${GEMM_BENCH} \
        --size=1024 --duration=${DURATION} \
        --output=/tmp/gflops_${scenario_id}.csv" \
        > "${gflops_txt}" 2>&1

    ssh "${FUJIAN_SSH}" "cat /tmp/gflops_${scenario_id}.csv" > "${gflops_csv}" 2>/dev/null || true
    ssh "${FUJIAN_SSH}" "cat /tmp/perf_${scenario_id}.txt" > "${perf_txt}"   2>/dev/null || true

    sleep 3
}

# =========================================================================
# Scenario 1: Baseline (gemm only)
# =========================================================================
run_gemm_scenario 1 "gemm_bench alone (baseline)"

# =========================================================================
# Scenario 2: No-offload — N metric_push instances in gRPC-fallback mode.
# slave_agent is NOT running on BF2, so Comch init fails and metric_push
# falls back to direct gRPC to cluster_master over 100G.
# =========================================================================
echo "[S2] starting metric_push (gRPC fallback) + ${N_SIM}x agent_simulator on fujian host..."
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} 'pkill -f slave_agent 2>/dev/null'" 2>/dev/null || true
sleep 2

ssh "${FUJIAN_SSH}" "nohup taskset -c ${COMPUTE_CORES} ${METRIC_PUSH_V2} \
    --pci=${HOST_PCI} \
    --master-addr=${MASTER_FALLBACK_ADDR} \
    --interval=${EXP_INTERVAL_MS} \
    --node-id=expB-s2 \
    > /tmp/metric_push_s2.log 2>&1 &"

start_simulators s2
sleep 7   # let Comch init time out (5s) and gRPC fallback take over

run_gemm_scenario 2 "gemm + metric_push(gRPC) + ${N_SIM}x simulator (no offload)"

ssh "${FUJIAN_SSH}" "pkill -f metric_push 2>/dev/null" 2>/dev/null || true
stop_simulators
sleep 3

# =========================================================================
# Scenario 3: Comch offload — slave_agent on fujian BF2 accepts host-side
# Comch traffic and forwards to cluster_master via NodeSession (gRPC).
# metric_push uses PCIe Comch (no host TCP / gRPC stack invoked).
# =========================================================================
echo "[S3] starting slave_agent on fujian BF2..."
# slave_agent uses the 1G mgmt LAN address; fujian BF2 routes 172.28.4.x via
# tmfifo to its host. For RDMA-bridged path, see Phase 5.
SLAVE_CMD="nohup ${NIC_SLAVE_AGENT} --master-addr=${MASTER_FALLBACK_ADDR} --hostname=fujian --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} --report-ms=1000 --bf2-report-ms=${HEARTBEAT_INTERVAL_MS} > /tmp/slave_agent_B.log 2>&1 < /dev/null &"
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} \"pkill -f slave_agent 2>/dev/null\""
sleep 1
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} \"${SLAVE_CMD}\""
sleep 4
# Verify it started
agent_started=$(ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} 'grep -m1 \"comch_nic_init OK\" /tmp/slave_agent_B.log 2>/dev/null'" || echo "")
if [ -z "${agent_started}" ]; then
    echo "  WARNING: slave_agent did not initialise Comch. S3 will fall back to gRPC."
    ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} 'tail -5 /tmp/slave_agent_B.log 2>/dev/null'" || true
fi

echo "[S3] starting metric_push (Comch path) + ${N_SIM}x agent_simulator on fujian host..."
ssh "${FUJIAN_SSH}" "nohup sudo taskset -c ${COMPUTE_CORES} ${METRIC_PUSH_V2} \
    --pci=${HOST_PCI} \
    --master-addr=${MASTER_FALLBACK_ADDR} \
    --interval=${EXP_INTERVAL_MS} \
    --node-id=expB-s3 \
    > /tmp/metric_push_s3.log 2>&1 &"
sleep 4

# Verify Comch path is actually being used (not gRPC fallback)
mode_line=$(ssh "${FUJIAN_SSH}" "grep -m1 'mode=' /tmp/metric_push_s3.log 2>/dev/null" || echo "")
echo "  metric_push: ${mode_line}"
if echo "${mode_line}" | grep -q "mode=grpc"; then
    echo "  WARNING: scenario 3 ran in gRPC mode (Comch failed). Comparison invalid."
fi

# NOTE: S3 deliberately does NOT start simulators on the host. The thesis
# claim is that the BF2 absorbs the kubelet/health/log workload, so the
# host should be left with only the workload itself plus the Comch-attached
# metric_push.

run_gemm_scenario 3 "gemm + metric_push(Comch) only — full BF2 offload"

ssh "${FUJIAN_SSH}" "pkill -f metric_push 2>/dev/null" 2>/dev/null || true
ssh "${FUJIAN_SSH}" "ssh root@${BF_IP} 'pkill -f slave_agent 2>/dev/null'" 2>/dev/null || true

# =========================================================================
# Quick summary
# =========================================================================
echo ""
echo "=== Quick Summary ==="
for s in 1 2 3; do
    f="${OUT_DIR}/scenario${s}_gflops.csv"
    if [ -f "$f" ] && [ -s "$f" ]; then
        # Skip CSV header (line 1) and warmup; average remaining gflops column
        avg=$(awk -F, -v warm="${WARMUP_SECS}" \
            'NR>1 && NR>warm+1 && $2 != "" {s+=$2; n++} END{if(n>0) printf "%.2f", s/n; else print "N/A"}' \
            "$f" 2>/dev/null || echo "N/A")
        echo "  Scenario ${s}: avg GFLOPS = ${avg}"
    else
        echo "  Scenario ${s}: <no data>"
    fi
done

# Emit summary CSV for run_repeated.sh
SUMMARY_CSV="${SUMMARY_CSV:-${OUT_DIR}/summary.csv}"
python3 "${SCRIPT_DIR}/analyze/emit_summary.py" \
    --exp B --data-dir "${OUT_DIR}" --out "${SUMMARY_CSV}" 2>/dev/null || \
    echo "(emit_summary.py failed; raw outputs are in ${OUT_DIR})"

echo ""
echo "=== Experiment B Complete ==="
echo "Results: ${OUT_DIR}"
