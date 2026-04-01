#!/bin/bash
set -uo pipefail
source ~/experiments/scripts/config.sh
export PGPASSWORD=postgres
mkdir -p ~/exp_data/D

LOG=~/exp_data/D/cluster_master.log

echo "=== Experiment D: Fault Recovery ==="

# ── Helpers ───────────────────────────────────────────────────────────
start_sa_fujian() {
  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}; nohup ~/experiments/build/control-plane/slave/slave_agent --node-uuid=fujian-bf2 --master-addr=192.168.56.10:${GRPC_PORT} --dev-pci=${NIC_PCI} --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} > /tmp/slave_agent.log 2>&1 &'" 
}
kill_sa_fujian() {
  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep slave_agent) 2>/dev/null'" 
}
wait_log() {
  local pat="$1" max="$2" start="$3"
  for a in $(seq 1 $max); do
    sleep 1
    tail -n +${start} $LOG 2>/dev/null | grep -qi "$pat" && return 0
  done
  return 1
}

# ── Setup (sequential to avoid SSH hang) ─────────────────────────────
pkill -9 cluster_master 2>/dev/null; sleep 1

${CLUSTER_MASTER} --grpc-port ${GRPC_PORT} --db-connstr "${DB_CONNSTR}" \
  > $LOG 2>&1 &
MASTER_PID=$!
sleep 3
echo "cluster_master PID=${MASTER_PID}"

# Start slave_agents (sequential)
for info in "172.28.4.77 fujian-bf2" "172.28.4.85 helong-bf2"; do
  host=$(echo $info | cut -d' ' -f1)
  uuid=$(echo $info | cut -d' ' -f2)
  ssh huaz@${host} "ssh root@192.168.100.2 'kill -9 \$(pgrep slave_agent) 2>/dev/null; sleep 1; export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}; nohup ~/experiments/build/control-plane/slave/slave_agent --node-uuid=${uuid} --master-addr=192.168.56.10:${GRPC_PORT} --dev-pci=${NIC_PCI} --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} > /tmp/slave_agent.log 2>&1 &'" 
  echo "  started slave_agent on ${host} (${uuid})"
  sleep 2
done

# Start metric_push (sequential)
for info in "172.28.4.77 fujian-host" "172.28.4.85 helong-host"; do
  host=$(echo $info | cut -d' ' -f1)
  nid=$(echo $info | cut -d' ' -f2)
  ssh huaz@${host} "pkill -f metric_push 2>/dev/null; sleep 1; nohup ~/experiments/build/bench/metric_push/metric_push --pci=${HOST_PCI} --interval=1000 --node-id=${nid} --master-addr=192.168.56.10:${GRPC_PORT} > ~/exp_data/D/metric_push.log 2>&1 &" 
  echo "  started metric_push on ${host} (${nid})"
  sleep 2
done

sleep 10

# ── Verify metric_push running on workers ─────────────────────────────
echo ""
echo "=== Verify metric_push ==="
for info in "172.28.4.77 fujian" "172.28.4.85 helong"; do
  host=$(echo $info | cut -d' ' -f1)
  name=$(echo $info | cut -d' ' -f2)
  MP_PID=$(ssh huaz@${host} "pgrep -f metric_push" 2>/dev/null)
  if [ -n "$MP_PID" ]; then
    echo "  ${name}: metric_push running (PID=${MP_PID})"
  else
    echo "  ${name}: metric_push NOT RUNNING — restarting..."
    nid="${name}-host"
    ssh huaz@${host} "nohup ~/experiments/build/bench/metric_push/metric_push --pci=${HOST_PCI} --interval=1000 --node-id=${nid} --master-addr=192.168.56.10:${GRPC_PORT} > ~/exp_data/D/metric_push.log 2>&1 & echo restarted_pid=\$!"
    sleep 3
    MP_PID=$(ssh huaz@${host} "pgrep -f metric_push" 2>/dev/null)
    if [ -n "$MP_PID" ]; then
      echo "  ${name}: metric_push restarted OK (PID=${MP_PID})"
    else
      echo "  ${name}: metric_push FAILED TO START — check log:"
      ssh huaz@${host} "cat ~/exp_data/D/metric_push.log 2>/dev/null | tail -5"
    fi
  fi
done

echo ""
echo "=== Verify slave_agent ==="
for info in "172.28.4.77 fujian" "172.28.4.85 helong"; do
  host=$(echo $info | cut -d' ' -f1)
  name=$(echo $info | cut -d' ' -f2)
  SA_PID=$(ssh huaz@${host} "ssh root@192.168.100.2 'pgrep -f slave_agent'" 2>/dev/null)
  if [ -n "$SA_PID" ]; then
    echo "  ${name} BF2: slave_agent running (PID=${SA_PID})"
  else
    echo "  ${name} BF2: slave_agent NOT RUNNING"
  fi
done

echo ""
echo "=== Node registry ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_uuid, state, host_status, bf2_status FROM node_registry ORDER BY node_uuid;"

sleep 5

# ── Scenario 1: slave_agent crash (5 reps) ────────────────────────────
echo ""
echo "=== Scenario 1: slave_agent crash ==="
echo "run,detection_ms,recovery_ms" > ~/exp_data/D/scenario1.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  L=$(wc -l < $LOG)
  T0=$(date +%s%3N)

  kill_sa_fujian

  if wait_log "fujian.*suspect\|fujian.*offline\|fujian.*closed" 45 $L; then
    DET=$(( $(date +%s%3N) - T0 ))
    echo "  Detection: ${DET} ms"
  else DET="timeout"; echo "  WARNING: detection timeout"; fi

  sleep 2
  start_sa_fujian

  if wait_log "fujian.*DOMAIN_OK\|fujian.*register.*from\|fujian.*Comch connection restored" 60 $L; then
    REC=$(( $(date +%s%3N) - T0 ))
    echo "  Recovery: ${REC} ms"
  else REC="timeout"; echo "  WARNING: recovery timeout"; fi

  echo "${i},${DET},${REC}" >> ~/exp_data/D/scenario1.csv
  sleep 15
done
echo ""; cat ~/exp_data/D/scenario1.csv

# ── Scenario 2: metric_push degradation (5 reps) ────────────────────
# Kill slave_agent → metric_push Comch fails 5 times → switches to gRPC direct
# Master log: "DirectPush received" confirms fallback working
# Then restart slave_agent, wait for metric_push to recover Comch
echo ""
echo "=== Scenario 2: metric_push degradation ==="
echo "run,fallback_ms,recovery_ms" > ~/exp_data/D/scenario2.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  L=$(wc -l < $LOG)
  T0=$(date +%s%3N)

  # Kill slave_agent — breaks Comch path, metric_push should fallback to gRPC
  kill_sa_fujian
  echo "  slave_agent killed, waiting for DirectPush in master log..."

  # Wait for master to receive DirectPush (= metric_push switched to gRPC)
  if wait_log "DirectPush received" 30 $L; then
    SW=$(( $(date +%s%3N) - T0 ))
    echo "  Fallback to gRPC: ${SW} ms"
  else SW="timeout"; echo "  WARNING: fallback timeout"; fi

  # Restart slave_agent so Comch can recover
  sleep 2
  start_sa_fujian

  # Wait for Comch to restore (master log: DOMAIN_OK or Comch connection restored)
  if wait_log "fujian.*DOMAIN_OK\|fujian.*Comch connection restored" 90 $L; then
    REC=$(( $(date +%s%3N) - T0 ))
    echo "  Comch recovered: ${REC} ms"
  else REC="timeout"; echo "  WARNING: Comch recovery timeout"; fi

  echo "${i},${SW},${REC}" >> ~/exp_data/D/scenario2.csv
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

# ── Summary ──────────────────────────────────────────────────────────
echo ""
echo "=== Final DB state ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_uuid, state, host_status, bf2_status FROM node_registry ORDER BY node_uuid;"
echo ""
echo "=== Recent events ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT time, node_uuid, event_type, detail FROM cluster_events ORDER BY time DESC LIMIT 15;"

# Cleanup
kill ${MASTER_PID} 2>/dev/null; sleep 1
for h in 172.28.4.77 172.28.4.85; do
  ssh huaz@${h} "pkill -f metric_push 2>/dev/null; ssh root@192.168.100.2 'kill \$(pgrep slave_agent) 2>/dev/null'" 
done

echo ""
echo "=== Experiment D complete ==="
