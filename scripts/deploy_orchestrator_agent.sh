#!/bin/bash
# deploy_orchestrator_agent.sh — Deploy orchestrator_agent to each BF2.
#
# Prereq:
#   - orchestrator_agent binary built on BF2 (via CMake, -DBUILD_TARGET=BF2)
#   - systemd on each BF2
#
# Usage:
#   bash deploy_orchestrator_agent.sh

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

AGENT_PORT=50052

deploy_one() {
    local host_ssh="$1"
    local bf2_ip="$2"

    echo ""
    echo "[deploy] → ${host_ssh} → BF2 ${bf2_ip}"

    # 1. Stop existing agent
    ssh "${host_ssh}" \
        "ssh root@${BF_IP} 'systemctl stop orch-agent 2>/dev/null; \
         pkill -f orchestrator_agent 2>/dev/null || true'"

    # 2. Install systemd unit
    ssh "${host_ssh}" \
        "ssh root@${BF_IP} 'cat > /etc/systemd/system/orch-agent.service << SYSTEMD_EOF
[Unit]
Description=Orchestrator Agent for local migration execution
After=network-online.target docker.service
Wants=network-online.target docker.service

[Service]
Type=simple
ExecStart=/root/experiments/build/control-plane/orchestrator_agent/orchestrator_agent --port=${AGENT_PORT} --bind=0.0.0.0
Restart=always
RestartSec=3
StandardOutput=append:/var/log/orch-agent.log
StandardError=append:/var/log/orch-agent.log

[Install]
WantedBy=multi-user.target
SYSTEMD_EOF
systemctl daemon-reload
systemctl enable orch-agent
systemctl restart orch-agent
sleep 1
systemctl status orch-agent --no-pager | head -5'"

    # 3. Quick verify (ping via gRPC would require grpcurl; use curl on health port if any)
    # Port check: ss -tlpn | grep 50052
    ssh "${host_ssh}" \
        "ssh root@${BF_IP} 'ss -tlpn 2>/dev/null | grep :${AGENT_PORT} || echo agent not listening'"

    echo "[deploy] done: ${host_ssh}"
}

# Deploy to tianjin (master BF2), fujian, helong
deploy_one "root@localhost"    "${TIANJIN_BF2_FABRIC}"
deploy_one "${USER}@${FUJIAN_IP}" "${FUJIAN_BF2_FABRIC}"
deploy_one "${USER}@${HELONG_IP}" "${HELONG_BF2_FABRIC}"

echo ""
echo "[deploy] All BF2s updated."
echo "Verify from master host:"
echo "  nc -zv ${FUJIAN_BF2_FABRIC} ${AGENT_PORT}"
echo "  nc -zv ${HELONG_BF2_FABRIC} ${AGENT_PORT}"
echo "  nc -zv ${TIANJIN_BF2_FABRIC} ${AGENT_PORT}"
