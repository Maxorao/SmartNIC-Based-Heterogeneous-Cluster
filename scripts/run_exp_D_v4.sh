#!/bin/bash
set -uo pipefail
source ~/experiments/scripts/config.sh
export PGPASSWORD=postgres
mkdir -p ~/exp_data/D

LOG=~/exp_data/D/cluster_master.log

echo "=== Experiment D: Fault Recovery ==="

# ── Helper functions ──────────────────────────────────────────────────
start_slave_agent_fujian() {
  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 '
    export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}
    nohup ~/experiments/build/control-plane/slave/slave_agent \
      --node-uuid=fujian-bf2 --master-addr=192.168.56.10:${GRPC_PORT} \
      --dev-pci=${NIC_PCI} --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
      > /tmp/slave_agent.log 2>&1 &
  '"
}

kill_slave_agent_fujian() {
  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep slave_agent) 2>/dev/null'"
}

wait_for_log() {
  local pattern="$1"
  local max_sec="$2"
  local log_start="$3"
  for a in $(seq 1 $max_sec); do
    sleep 1
    if tail -n +${log_start} $LOG | grep -qi "$pattern"; then
      return 0
    fi
  done
  return 1
}

# ── Setup ─────────────────────────────────────────────────────────────
pkill -9 cluster_master 2>/dev/null; sleep 1

${CLUSTER_MASTER} --grpc-port ${GRPC_PORT} --db-connstr "${DB_CONNSTR}" \
  > $LOG 2>&1 &
MASTER_PID=$!
sleep 3
echo "cluster_master PID=${MASTER_PID}"

# Start slave_agents
for info in "172.28.4.77 fujian-bf2" "172.28.4.85 helong-bf2"; do
  host=$(echo $info | cut -d' ' -f1)
  uuid=$(echo $info | cut -d' ' -f2)
  ssh huaz@${host} "ssh root@192.168.100.2 '
    kill -9 \$(pgrep slave_agent) 2>/dev/null; sleep 1
    export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}
    nohup ~/experiments/build/control-plane/slave/slave_agent \
      --node-uuid=${uuid} --master-addr=192.168.56.10:${GRPC_PORT} \
      --dev-pci=${NIC_PCI} --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
      > /tmp/slave_agent.log 2>&1 &
  '" &
done
wait; sleep 5

# Start metric_push on workers
for info in "172.28.4.77 fujian-host" "172.28.4.85 helong-host"; do
  host=$(echo $info | cut -d' ' -f1)
  nid=$(echo $info | cut -d' ' -f2)
  ssh huaz@${host} "
    pkill -f metric_push 2>/dev/null; sleep 1
    nohup ~/experiments/build/bench/metric_push/metric_push \
      --pci=${HOST_PCI} --interval=1000 --node-id=${nid} \
      --master-addr=192.168.56.10:${GRPC_PORT} \
      > ~/exp_data/D/metric_push.log 2>&1 &
  " &
done
wait; sleep 5

echo "=== Verify ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_uuid, state, host_status, bf2_status FROM node_registry ORDER BY node_uuid;" 2>/dev/null

echo ""
echo "Waiting 15s for stable heartbeats..."
sleep 15
tail -3 $LOG

# ── Scenario 1: slave_agent crash (5 reps) ────────────────────────────
echo ""
echo "=== Scenario 1: slave_agent crash ==="
echo "run,detection_ms,recovery_ms" > ~/exp_data/D/scenario1.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  L=$(wc -l < $LOG)
  T0=$(date +%s%3N)

  kill_slave_agent_fujian

  if wait_for_log "fujian.*suspect\|fujian.*offline\|fujian.*closed" 45 $L; then
    DET=$(( $(date +%s%3N) - T0 ))
    echo "  Detection: ${DET} ms"
  else
    DET="timeout"; echo "  WARNING: detection timeout"
  fi

  start_slave_agent_fujian

  if wait_for_log "fujian.*register" 45 $L; then
    REC=$(( $(date +%s%3N) - T0 ))
    echo "  Recovery: ${REC} ms"
  else
    REC="timeout"; echo "  WARNING: recovery timeout"
  fi

  echo "${i},${DET},${REC}" >> ~/exp_data/D/scenario1.csv
  sleep 15
done
echo ""; cat ~/exp_data/D/scenario1.csv

# ── Scenario 2: metric_push degradation (5 reps) ─────────────────────
echo ""
echo "=== Scenario 2: metric_push degradation ==="
echo "run,switch_ms" > ~/exp_data/D/scenario2.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  L=$(wc -l < $LOG)
  T0=$(date +%s%3N)

  kill_slave_agent_fujian

  if wait_for_log "DirectPush\|direct.*fujian\|fallback" 30 $L; then
    SW=$(( $(date +%s%3N) - T0 ))
    echo "  Switch: ${SW} ms"
  else
    SW="timeout"; echo "  WARNING: switch timeout"
  fi

  start_slave_agent_fujian

  echo "${i},${SW}" >> ~/exp_data/D/scenario2.csv
  sleep 15
done
echo ""; cat ~/exp_data/D/scenario2.csv

# ── Scenario 3: cluster_master crash (5 reps) ────────────────────────
echo ""
echo "=== Scenario 3: cluster_master crash ==="
echo "run,restart_ms,reconnect_ms" > ~/exp_data/D/scenario3.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  T0=$(date +%s%3N)
  kill -9 ${MASTER_PID} 2>/dev/null
  sleep 2

  ${CLUSTER_MASTER} --grpc-port ${GRPC_PORT} --db-connstr "${DB_CONNSTR}" \
    > $LOG 2>&1 &
  MASTER_PID=$!
  RESTART=$(( $(date +%s%3N) - T0 ))
  echo "  Restart: ${RESTART} ms"

  RECON="timeout"
  for a in $(seq 1 30); do
    sleep 1
    N=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
      "SELECT COUNT(*) FROM node_registry WHERE state='online';" 2>/dev/null | tr -d ' ')
    if [ "${N:-0}" -ge 2 ]; then
      RECON=$(( $(date +%s%3N) - T0 ))
      echo "  Reconnect: ${RECON} ms"
      break
    fi
  done
  [ "$RECON" = "timeout" ] && echo "  WARNING: reconnect timeout"

  echo "${i},${RESTART},${RECON}" >> ~/exp_data/D/scenario3.csv
  sleep 10
done
echo ""; cat ~/exp_data/D/scenario3.csv

# ── Cleanup ──────────────────────────────────────────────────────────
echo ""
echo "=== DB status after experiments ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_uuid, state, host_status, bf2_status FROM node_registry ORDER BY node_uuid;" 2>/dev/null
echo ""
echo "=== cluster_events ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT time, node_uuid, event_type, detail FROM cluster_events ORDER BY time DESC LIMIT 20;" 2>/dev/null

pkill cluster_master 2>/dev/null
for h in 172.28.4.77 172.28.4.85; do
  ssh huaz@${h} "pkill -f metric_push 2>/dev/null; ssh root@192.168.100.2 'kill \$(pgrep slave_agent) 2>/dev/null'" &
done
wait

echo ""
echo "=== Experiment D complete ==="
