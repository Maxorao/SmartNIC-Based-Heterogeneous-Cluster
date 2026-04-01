#!/bin/bash
# Experiment D: Fault Recovery
set -uo pipefail
source ~/experiments/scripts/config.sh
export PGPASSWORD=postgres
mkdir -p ~/exp_data/D

echo "=== Experiment D: Fault Recovery ==="

# 6b. Setup
pkill -9 -f master_monitor 2>/dev/null; sleep 1
"${MASTER_MONITOR}" --port=${MASTER_PORT} \
  --db-connstr="${DB_CONNSTR}" \
  > ~/exp_data/D/master_monitor.log 2>&1 &
MASTER_PID=$!
sleep 3
echo "master_monitor started (PID=${MASTER_PID})"

# Start forward_routine on fujian BF2
ssh huaz@172.28.4.77 "
  ssh root@192.168.100.2 '
    pkill -f forward_routine 2>/dev/null; sleep 1
    nohup ~/experiments/control-plane/forwarder/forward_routine \
      --pci=03:00.0 --master-ip=192.168.56.10 --master-port=9000 \
      > /tmp/forward_routine.log 2>&1 &
  '
"
sleep 3
echo "forward_routine started on fujian BF2"

# Start slave_monitor on fujian host
ssh huaz@172.28.4.77 "
  sudo pkill -f slave_monitor 2>/dev/null; sleep 1
  sudo nohup ~/experiments/control-plane/slave/slave_monitor \
    --mode=offload --pci=0000:5e:00.0 --interval=1000 \
    --node-id=fujian-worker > ~/exp_data/D/slave_monitor.log 2>&1 &
"
sleep 5
echo "slave_monitor started on fujian"

# Start on helong too
ssh huaz@172.28.4.85 "
  ssh root@192.168.100.2 '
    pkill -f forward_routine 2>/dev/null; sleep 1
    nohup ~/experiments/control-plane/forwarder/forward_routine \
      --pci=03:00.0 --master-ip=192.168.56.10 --master-port=9000 \
      > /tmp/forward_routine.log 2>&1 &
  '
  sleep 3
  sudo pkill -f slave_monitor 2>/dev/null; sleep 1
  sudo nohup ~/experiments/control-plane/slave/slave_monitor \
    --mode=offload --pci=0000:5e:00.0 --interval=1000 \
    --node-id=helong-worker > ~/exp_data/D/slave_monitor.log 2>&1 &
"
sleep 5
echo "helong also running"

echo "=== Registered nodes ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_id, online, last_seen FROM node_status ORDER BY node_id;" 2>/dev/null || echo "(no DB table yet)"

echo "Waiting 15s to confirm heartbeats flowing..."
sleep 15

# 6c. Scenario 1 — forward_routine crash (5 repetitions)
echo ""
echo "=== Scenario 1: forward_routine crash ==="
echo "run,t_fault_epoch_ms,detection_ms,recovery_ms" > ~/exp_data/D/scenario1.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  LOG_LINES_BEFORE=$(wc -l < ~/exp_data/D/master_monitor.log)
  T_FAULT=$(date +%s%3N)

  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep -f forward_routine) 2>/dev/null'"
  echo "  t=${T_FAULT}: forward_routine killed on fujian BF2"

  T_DETECT=-1
  for attempt in $(seq 1 60); do
    sleep 1
    if tail -n +${LOG_LINES_BEFORE} ~/exp_data/D/master_monitor.log | grep -qi "fujian-worker.*offline\|fujian-worker.*timeout\|fujian-worker.*lost\|fujian-worker.*disconnect"; then
      T_DETECT=$(date +%s%3N)
      break
    fi
  done

  if [ "${T_DETECT}" -eq -1 ]; then
    DET_MS="timeout"
    echo "  WARNING: detection timeout (>60s)"
  else
    DET_MS=$(( T_DETECT - T_FAULT ))
    echo "  Detection time: ${DET_MS} ms"
  fi

  # Restart forward_routine
  ssh huaz@172.28.4.77 "
    ssh root@192.168.100.2 '
      nohup ~/experiments/control-plane/forwarder/forward_routine \
        --pci=03:00.0 --master-ip=192.168.56.10 --master-port=9000 \
        > /tmp/forward_routine.log 2>&1 &
    '
  "

  T_RECOVER=-1
  for attempt in $(seq 1 60); do
    sleep 1
    if tail -n +${LOG_LINES_BEFORE} ~/exp_data/D/master_monitor.log | grep -qi "fujian-worker.*online\|fujian-worker.*registered\|fujian-worker.*reconnect"; then
      T_RECOVER=$(date +%s%3N)
      break
    fi
  done

  if [ "${T_RECOVER}" -eq -1 ]; then
    REC_MS="timeout"
    echo "  WARNING: recovery timeout (>60s)"
  else
    REC_MS=$(( T_RECOVER - T_FAULT ))
    echo "  Total recovery time: ${REC_MS} ms"
  fi

  echo "${i},${T_FAULT},${DET_MS},${REC_MS}" >> ~/exp_data/D/scenario1.csv
  echo "  Stabilizing 15s..."
  sleep 15
done

echo ""
echo "=== Scenario 1 Results ==="
cat ~/exp_data/D/scenario1.csv

# 6d. Scenario 2 — slave_monitor restart (5 repetitions)
echo ""
echo "=== Scenario 2: slave_monitor restart ==="
echo "run,t_restart_epoch_ms,reregistration_ms" > ~/exp_data/D/scenario2.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  LOG_LINES_BEFORE=$(wc -l < ~/exp_data/D/master_monitor.log)
  T_RESTART=$(date +%s%3N)

  ssh huaz@172.28.4.77 "
    sudo pkill -f slave_monitor 2>/dev/null
    sleep 2
    sudo nohup ~/experiments/control-plane/slave/slave_monitor \
      --mode=offload --pci=0000:5e:00.0 --interval=1000 \
      --node-id=fujian-worker > ~/exp_data/D/slave_monitor.log 2>&1 &
  "

  T_REREG=-1
  for attempt in $(seq 1 30); do
    sleep 1
    if tail -n +${LOG_LINES_BEFORE} ~/exp_data/D/master_monitor.log | grep -qi "fujian-worker.*register\|fujian-worker.*online"; then
      T_REREG=$(date +%s%3N)
      break
    fi
  done

  if [ "${T_REREG}" -eq -1 ]; then
    REREG_MS="timeout"
    echo "  WARNING: re-registration timeout"
  else
    REREG_MS=$(( T_REREG - T_RESTART ))
    echo "  Re-registration time: ${REREG_MS} ms"
  fi

  echo "${i},${T_RESTART},${REREG_MS}" >> ~/exp_data/D/scenario2.csv
  sleep 10
done

echo ""
echo "=== Scenario 2 Results ==="
cat ~/exp_data/D/scenario2.csv

# 6e. Cleanup
pkill -f master_monitor 2>/dev/null
ssh huaz@172.28.4.77 "sudo pkill -f slave_monitor 2>/dev/null; ssh root@192.168.100.2 'pkill -f forward_routine 2>/dev/null'"
ssh huaz@172.28.4.85 "sudo pkill -f slave_monitor 2>/dev/null; ssh root@192.168.100.2 'pkill -f forward_routine 2>/dev/null'"
echo "=== Experiment D complete ==="
