#!/bin/bash
set -uo pipefail
source ~/experiments/scripts/config.sh
export PGPASSWORD=postgres
LOG=~/exp_data/D/cluster_master.log

echo "=== Experiment D: Fault Recovery (v3 — gRPC) ==="
echo ""

# Helper: restart fujian slave_agent
restart_fujian_sa() {
  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 '
    export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}
    nohup ~/experiments/build/control-plane/slave/slave_agent \
      --node-uuid=fujian-bf2 --master-addr=192.168.56.10:50051 \
      --dev-pci=03:00.0 --heartbeat-ms=3000 \
      > /tmp/slave_agent.log 2>&1 &
  '"
}

# ── Scenario 1: slave_agent crash (5 reps) ─────────────────────────────
echo "=== Scenario 1: slave_agent crash on fujian BF2 ==="
echo "run,detection_ms,recovery_ms" > ~/exp_data/D/scenario1.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  LOG_BEFORE=$(wc -l < $LOG)
  T_FAULT=$(date +%s%3N)

  ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep slave_agent) 2>/dev/null'"

  # Wait for detection
  DET_MS="timeout"
  for a in $(seq 1 45); do
    sleep 1
    if tail -n +${LOG_BEFORE} $LOG | grep -qi "fujian.*suspect\|fujian.*offline\|fujian.*closed"; then
      DET_MS=$(( $(date +%s%3N) - T_FAULT ))
      echo "  Detection: ${DET_MS} ms"
      break
    fi
  done
  [ "$DET_MS" = "timeout" ] && echo "  WARNING: detection timeout"

  # Restart and wait for recovery
  restart_fujian_sa
  REC_MS="timeout"
  for a in $(seq 1 45); do
    sleep 1
    if tail -n +${LOG_BEFORE} $LOG | grep -qi "fujian.*register\|fujian.*online"; then
      REC_MS=$(( $(date +%s%3N) - T_FAULT ))
      echo "  Recovery: ${REC_MS} ms"
      break
    fi
  done
  [ "$REC_MS" = "timeout" ] && echo "  WARNING: recovery timeout"

  echo "${i},${DET_MS},${REC_MS}" >> ~/exp_data/D/scenario1.csv
  sleep 15
done

echo ""
cat ~/exp_data/D/scenario1.csv

# ── Scenario 3: cluster_master crash (5 reps) ──────────────────────────
echo ""
echo "=== Scenario 3: cluster_master crash + manual restart ==="
echo "run,restart_ms,reconnect_ms" > ~/exp_data/D/scenario3.csv

MASTER_PID=$(pgrep cluster_master)

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  T_FAULT=$(date +%s%3N)
  kill -9 ${MASTER_PID} 2>/dev/null

  sleep 2
  ${CLUSTER_MASTER} --grpc-port 50051 --db-connstr "${DB_CONNSTR}" \
    > ~/exp_data/D/cluster_master.log 2>&1 &
  MASTER_PID=$!
  T_RESTART=$(date +%s%3N)
  RESTART_MS=$(( T_RESTART - T_FAULT ))
  echo "  Restart: ${RESTART_MS} ms"

  # Wait for both nodes to reconnect
  RECON_MS="timeout"
  for a in $(seq 1 30); do
    sleep 1
    ONLINE=$(PGPASSWORD=postgres psql -h localhost -U postgres -d cluster_metrics -t -c \
      "SELECT COUNT(*) FROM node_registry WHERE state='online';" 2>/dev/null | tr -d ' ')
    if [ "${ONLINE:-0}" -ge 2 ]; then
      RECON_MS=$(( $(date +%s%3N) - T_FAULT ))
      echo "  Reconnect (2 nodes): ${RECON_MS} ms"
      break
    fi
  done
  [ "$RECON_MS" = "timeout" ] && echo "  WARNING: reconnect timeout"

  echo "${i},${RESTART_MS},${RECON_MS}" >> ~/exp_data/D/scenario3.csv
  sleep 10
done

echo ""
cat ~/exp_data/D/scenario3.csv

# ── Cleanup ─────────────────────────────────────────────────────────────
echo ""
echo "=== Experiment D complete ==="
