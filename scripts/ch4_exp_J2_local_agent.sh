#!/bin/bash
# ch4_exp_J2_local_agent.sh — Blue-green migration using the local
# orchestrator_agent (gRPC) instead of two-hop SSH.
#
# Compares 5-iteration migration timing against the SSH-based baseline
# (ch4_exp_J_orchestration.sh J.3 output). Expected: ~8.2s → ~0.8-1.5s.
#
# Prerequisites:
#   - orchestrator_agent deployed on all BF2s (deploy_orchestrator_agent.sh)
#   - VIP 192.168.56.200 free on fabric

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

DURATION=60
VIP="192.168.56.200"
OUT_DIR="${DATA_DIR}/J_local_agent"
mkdir -p "$OUT_DIR"

AGENT_PORT=50052

# Sanity: agents reachable?
for bf2 in ${FUJIAN_BF2_FABRIC} ${HELONG_BF2_FABRIC}; do
    if ! nc -z -w 2 "${bf2}" "${AGENT_PORT}" 2>/dev/null; then
        echo "[FATAL] orchestrator_agent not reachable at ${bf2}:${AGENT_PORT}"
        echo "  Run: bash ${SCRIPT_DIR}/deploy_orchestrator_agent.sh"
        exit 1
    fi
done
echo "[ok] all orchestrator_agents reachable"

echo ""
echo "=== Experiment J2: Migration via local agent (5 iterations) ==="
echo "run,container_ms,health_ms,vip_ms,total_ms" > "${OUT_DIR}/migration_times.csv"

cleanup_one() {
    # Remove nginx-new and nginx from all candidate BF2s; remove VIP
    for bf2 in ${FUJIAN_BF2_FABRIC} ${HELONG_BF2_FABRIC}; do
        ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no \
            root@${bf2} "docker rm -f nginx nginx-new 2>/dev/null; \
            ip addr del ${VIP}/24 dev p1 2>/dev/null; \
            ip addr del ${VIP}/24 dev p0 2>/dev/null" 2>/dev/null || true
    done
    ssh "${USER}@${FUJIAN_IP}" \
        "sudo ip addr del ${VIP}/24 dev enp94s0f1np1 2>/dev/null" 2>/dev/null || true
}
trap cleanup_one EXIT INT TERM

for i in $(seq 1 5); do
    echo ""
    echo "[iter ${i}/5] setting up: nginx on fujian host + VIP on host"
    cleanup_one

    # Pre-state: Nginx on fujian host with VIP
    ssh "${USER}@${FUJIAN_IP}" \
        "docker rm -f nginx 2>/dev/null; \
         docker run -d --name nginx --network=host nginx:alpine"
    ssh "${USER}@${FUJIAN_IP}" \
        "sudo ip addr add ${VIP}/24 dev enp94s0f1np1 2>/dev/null || true"
    ssh "${USER}@${FUJIAN_IP}" \
        "sudo arping -c 3 -A -I enp94s0f1np1 ${VIP} &>/dev/null &"
    sleep 2

    # Invoke orchestrator.py in one-shot agent mode
    # (We craft this via python3 -c using MigrationManager directly.)
    T_TOTAL_START=$(date +%s%N)

    python3 - <<PY 2>&1 | tee "${OUT_DIR}/iter${i}.log"
import sys, os, logging, time, json
sys.path.insert(0, '${EXP_BASE}/control-plane')
sys.path.insert(0, '${EXP_BASE}')
from orchestrator.orchestrator import (
    MigrationManager, NodeConfig, SmartNICConfig, WorkloadConfig,
    build_default_config
)
logging.basicConfig(level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s")

cfg = build_default_config()
src_node = cfg.nodes['fujian']
dst_nic  = cfg.nics['fujian-bf2']
wl       = cfg.workloads['nginx']
vip      = '${VIP}'

mgr = MigrationManager(src_node, dst_nic, wl, vip,
    src_is_host=True, log=logging.getLogger('j2'),
    use_local_agent=True, agent_port=${AGENT_PORT})
ok, timings = mgr.execute()
print("RESULT:", json.dumps({"ok": ok, "timings_ms": timings}))
PY
    T_TOTAL_END=$(date +%s%N)
    TT=$(( (T_TOTAL_END - T_TOTAL_START) / 1000000 ))

    # Parse RESULT line from log
    result_line=$(grep "^RESULT:" "${OUT_DIR}/iter${i}.log" | tail -1)
    if [ -z "${result_line}" ]; then
        echo "[iter ${i}] no result parsed"
        echo "${i},0,0,0,${TT}" >> "${OUT_DIR}/migration_times.csv"
        continue
    fi
    # Extract stage timings (lazy sed)
    cs_ms=$(echo "${result_line}" | sed -E 's/.*"container_start": ([0-9.]+).*/\1/p; d' | head -1)
    hc_ms=$(echo "${result_line}" | sed -E 's/.*"health_check": ([0-9.]+).*/\1/p; d' | head -1)
    vp_ms=$(echo "${result_line}" | sed -E 's/.*"vip_switch": ([0-9.]+).*/\1/p; d' | head -1)
    echo "${i},${cs_ms:-0},${hc_ms:-0},${vp_ms:-0},${TT}" \
        >> "${OUT_DIR}/migration_times.csv"
    echo "[iter ${i}] done: total=${TT}ms"
done

echo ""
echo "=== Experiment J2 Complete ==="
echo "Results: ${OUT_DIR}/migration_times.csv"
cat "${OUT_DIR}/migration_times.csv"
