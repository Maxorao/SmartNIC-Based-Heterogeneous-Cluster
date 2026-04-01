#!/bin/bash
# Experiment D: Fault Recovery (Chapter 3 v2 — gRPC architecture)
set -uo pipefail
source ~/experiments/scripts/config.sh
export PGPASSWORD=postgres
mkdir -p ~/exp_data/D

echo "=== Experiment D: Fault Recovery (v2) ==="

# ── 6b. Setup ──────────────────────────────────────────────────────────────
pkill -9 -f cluster_master 2>/dev/null || true; sleep 1

# Start cluster_master on tianjin
"${CLUSTER_MASTER}" --grpc-port=${GRPC_PORT} --db-connstr="${DB_CONNSTR}" \
  > ~/exp_data/D/cluster_master.log 2>&1 &
MASTER_PID=$!
sleep 3
echo "cluster_master PID=${MASTER_PID}"

# Start slave_agent on fujian BF2
ssh huaz@172.28.4.77 "ssh root@192.168.100.2 '
  pkill -9 -f slave_agent 2>/dev/null; sleep 1
  export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}
  nohup ~/experiments/build/control-plane/slave/slave_agent \
    --node-uuid=fujian-bf2 \
    --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
    --dev-pci=${NIC_PCI} \
    --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
    > /tmp/slave_agent.log 2>&1 &
'"
sleep 3
echo "slave_agent started on fujian BF2"

# Start metric_push on fujian host
ssh huaz@172.28.4.77 "
  pkill -f metric_push 2>/dev/null; sleep 1
  nohup ~/experiments/build/bench/metric_push/metric_push \
    --pci=${HOST_PCI} --interval=1000 --node-id=fujian-host \
    --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
    > ~/exp_data/D/metric_push.log 2>&1 &
"
sleep 3
echo "metric_push started on fujian"

# Start on helong too
ssh huaz@172.28.4.85 "ssh root@192.168.100.2 '
  pkill -9 -f slave_agent 2>/dev/null; sleep 1
  export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}
  nohup ~/experiments/build/control-plane/slave/slave_agent \
    --node-uuid=helong-bf2 \
    --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
    --dev-pci=${NIC_PCI} \
    --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
    > /tmp/slave_agent.log 2>&1 &
'"
sleep 3
ssh huaz@172.28.4.85 "
  pkill -f metric_push 2>/dev/null; sleep 1
  nohup ~/experiments/build/bench/metric_push/metric_push \
    --pci=${HOST_PCI} --interval=1000 --node-id=helong-host \
    --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
    > ~/exp_data/D/metric_push_helong.log 2>&1 &
"
sleep 5
echo "helong also running"

# Verify
echo "=== Verify: node_registry ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_uuid, state, host_status, bf2_status, last_seen FROM node_registry ORDER BY node_uuid;" 2>/dev/null || echo "(no table yet — check cluster_master log)"

echo ""
echo "Waiting 15s to confirm heartbeats..."
sleep 15
tail -5 ~/exp_data/D/cluster_master.log

# ── 6c. Scenario 1 — slave_agent crash (5 repetitions) ──────────────────
echo ""
echo "=== Scenario 1: slave_agent crash on fujian BF2 ==="
echo "run,t_fault_epoch_ms,detection_ms,recovery_ms" > ~/exp_data/D/scenario1.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  LOG_BEFORE=$(wc -l < ~/exp_data/D/cluster_master.log)
  T_FAULT=$(date +%s%3N)

  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep -f slave_agent) 2>/dev/null'"
  echo "  t=${T_FAULT}: slave_agent killed"

  T_DETECT=-1
  for attempt in $(seq 1 60); do
    sleep 1
    if tail -n +${LOG_BEFORE} ~/exp_data/D/cluster_master.log | grep -qi "fujian.*suspect\|fujian.*offline\|fujian.*disconnect\|fujian.*closed"; then
      T_DETECT=$(date +%s%3N)
      break
    fi
  done
  if [ "${T_DETECT}" -eq -1 ]; then DET_MS="timeout"; echo "  WARNING: detection timeout"
  else DET_MS=$(( T_DETECT - T_FAULT )); echo "  Detection: ${DET_MS} ms"; fi

  # Restart slave_agent
  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 '
    export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}
    nohup ~/experiments/build/control-plane/slave/slave_agent \
      --node-uuid=fujian-bf2 --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
      --dev-pci=${NIC_PCI} --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
      > /tmp/slave_agent.log 2>&1 &
  '"

  T_RECOVER=-1
  for attempt in $(seq 1 60); do
    sleep 1
    if tail -n +${LOG_BEFORE} ~/exp_data/D/cluster_master.log | grep -qi "fujian.*online\|fujian.*register"; then
      T_RECOVER=$(date +%s%3N)
      break
    fi
  done
  if [ "${T_RECOVER}" -eq -1 ]; then REC_MS="timeout"; echo "  WARNING: recovery timeout"
  else REC_MS=$(( T_RECOVER - T_FAULT )); echo "  Recovery: ${REC_MS} ms"; fi

  echo "${i},${T_FAULT},${DET_MS},${REC_MS}" >> ~/exp_data/D/scenario1.csv
  echo "  Stabilizing 15s..."
  sleep 15
done
echo ""
echo "=== Scenario 1 Results ==="
cat ~/exp_data/D/scenario1.csv

# ── 6d. Scenario 2 — metric_push degradation (5 repetitions) ────────────
echo ""
echo "=== Scenario 2: metric_push degradation (Comch→gRPC fallback) ==="
echo "run,t_fault_epoch_ms,switch_ms" > ~/exp_data/D/scenario2.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  LOG_BEFORE=$(wc -l < ~/exp_data/D/cluster_master.log)
  T_FAULT=$(date +%s%3N)

  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep -f slave_agent) 2>/dev/null'"
  echo "  t=${T_FAULT}: slave_agent killed (Comch broken)"

  T_SWITCH=-1
  for attempt in $(seq 1 30); do
    sleep 1
    if tail -n +${LOG_BEFORE} ~/exp_data/D/cluster_master.log | grep -qi "DirectPush\|direct.*fujian\|fallback"; then
      T_SWITCH=$(date +%s%3N)
      break
    fi
  done
  if [ "${T_SWITCH}" -eq -1 ]; then SW_MS="timeout"; echo "  WARNING: switch timeout"
  else SW_MS=$(( T_SWITCH - T_FAULT )); echo "  Switch: ${SW_MS} ms"; fi

  # Restart slave_agent
  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 '
    export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}
    nohup ~/experiments/build/control-plane/slave/slave_agent \
      --node-uuid=fujian-bf2 --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
      --dev-pci=${NIC_PCI} --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
      > /tmp/slave_agent.log 2>&1 &
  '"

  echo "${i},${T_FAULT},${SW_MS}" >> ~/exp_data/D/scenario2.csv
  sleep 15
done
echo ""
echo "=== Scenario 2 Results ==="
cat ~/exp_data/D/scenario2.csv

# ── 6e. Scenario 3 — cluster_master crash (5 repetitions) ───────────────
echo ""
echo "=== Scenario 3: cluster_master crash + restart ==="
echo "run,t_fault_epoch_ms,restart_ms,reconnect_ms" > ~/exp_data/D/scenario3.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  T_FAULT=$(date +%s%3N)
  kill -9 ${MASTER_PID} 2>/dev/null
  echo "  t=${T_FAULT}: cluster_master killed"

  # Manual restart (watchdog not running in this test)
  sleep 2
  "${CLUSTER_MASTER}" --grpc-port=${GRPC_PORT} --db-connstr="${DB_CONNSTR}" \
    > ~/exp_data/D/cluster_master.log 2>&1 &
  MASTER_PID=$!
  T_RESTART=$(date +%s%3N)
  RESTART_MS=$(( T_RESTART - T_FAULT ))
  echo "  Restart: ${RESTART_MS} ms (manual)"

  # Wait for slave_agents to reconnect
  T_RECONNECT=-1
  for attempt in $(seq 1 30); do
    sleep 1
    ONLINE=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
      "SELECT COUNT(*) FROM node_registry WHERE state='online';" 2>/dev/null | tr -d ' ')
    if [ "${ONLINE:-0}" -ge 2 ]; then
      T_RECONNECT=$(date +%s%3N)
      break
    fi
  done
  if [ "${T_RECONNECT}" -eq -1 ]; then RECON_MS="timeout"; echo "  WARNING: reconnect timeout"
  else RECON_MS=$(( T_RECONNECT - T_FAULT )); echo "  Reconnect: ${RECON_MS} ms"; fi

  echo "${i},${T_FAULT},${RESTART_MS},${RECON_MS}" >> ~/exp_data/D/scenario3.csv
  sleep 10
done
echo ""
echo "=== Scenario 3 Results ==="
cat ~/exp_data/D/scenario3.csv

# ── 6f. Cleanup ─────────────────────────────────────────────────────────
pkill -f cluster_master 2>/dev/null
for host in 172.28.4.77 172.28.4.85; do
  ssh huaz@${host} "pkill -f metric_push 2>/dev/null; ssh root@192.168.100.2 'pkill -f slave_agent 2>/dev/null'" &
done
wait
echo ""
echo "=== Experiment D complete ==="
