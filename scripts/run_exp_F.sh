#!/bin/bash
set -uo pipefail
source ~/experiments/scripts/config.sh
export PGPASSWORD=postgres
mkdir -p ~/exp_data/F

echo "=== Experiment F: Functional Correctness Verification ==="

# ── 8a. Clean start — registration flow ──────────────────────────────
echo ""
echo "=== 8a. Registration Flow ==="

# Reset DB
psql -h localhost -U postgres -d cluster_metrics -c \
  "DELETE FROM node_registry; TRUNCATE host_metrics, bf2_metrics, cluster_events;" 2>/dev/null

# Start fresh cluster_master
pkill -9 cluster_master 2>/dev/null; sleep 1
${CLUSTER_MASTER} --grpc-port ${GRPC_PORT} --db-connstr "${DB_CONNSTR}" \
  > ~/exp_data/F/cluster_master.log 2>&1 &
sleep 3

# Start slave_agent on fujian BF2
ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep slave_agent) 2>/dev/null; sleep 1; export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}; nohup ~/experiments/build/control-plane/slave/slave_agent --node-uuid=fujian-bf2 --master-addr=192.168.56.10:${GRPC_PORT} --dev-pci=${NIC_PCI} --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} > /tmp/slave_agent.log 2>&1 &'"
sleep 3

# Start metric_push on fujian host
ssh huaz@172.28.4.77 "pkill -f metric_push 2>/dev/null; sleep 1; nohup ~/experiments/build/bench/metric_push/metric_push --pci=${HOST_PCI} --interval=1000 --node-id=fujian-host --master-addr=192.168.56.10:${GRPC_PORT} > /tmp/metric_push.log 2>&1 & echo started"
sleep 5

# Verify metric_push
MP_PID=$(ssh huaz@172.28.4.77 "pgrep -f metric_push" 2>/dev/null)
if [ -z "$MP_PID" ]; then
  echo "  metric_push not running, restarting..."
  ssh huaz@172.28.4.77 "nohup ~/experiments/build/bench/metric_push/metric_push --pci=${HOST_PCI} --interval=1000 --node-id=fujian-host --master-addr=192.168.56.10:${GRPC_PORT} > /tmp/metric_push.log 2>&1 & echo restarted"
  sleep 5
fi

sleep 5

echo "--- cluster_master log (first 30 lines) ---"
head -30 ~/exp_data/F/cluster_master.log | tee ~/exp_data/F/registration_log.txt
echo ""
echo "--- node_registry table ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_registry;" | tee ~/exp_data/F/node_registry_after_reg.txt
echo ""
echo "--- host_metrics (first 5 rows) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM host_metrics ORDER BY time LIMIT 5;" | tee ~/exp_data/F/first_host_metrics.txt
echo ""
echo "--- bf2_metrics (first 5 rows) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM bf2_metrics ORDER BY time LIMIT 5;" | tee ~/exp_data/F/first_bf2_metrics.txt

# ── 8b. Heartbeat and resource reporting ─────────────────────────────
echo ""
echo "=== 8b. Heartbeat & Resource Reporting ==="
echo "Letting system run for 30 seconds..."
sleep 30

echo "--- host_metrics count ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_uuid, COUNT(*) as report_count,
          MIN(time) as first_report, MAX(time) as last_report
   FROM host_metrics GROUP BY node_uuid;" | tee ~/exp_data/F/host_report_count.txt
echo ""
echo "--- bf2_metrics count ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_uuid, COUNT(*) as report_count,
          MIN(time) as first_report, MAX(time) as last_report
   FROM bf2_metrics GROUP BY node_uuid;" | tee ~/exp_data/F/bf2_report_count.txt
echo ""
echo "--- cluster_events ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM cluster_events ORDER BY time;" | tee ~/exp_data/F/events.txt
echo ""
echo "--- node_registry (should show last_seen updated) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_registry;" | tee ~/exp_data/F/node_registry_heartbeat.txt

# ── 8c. Re-registration with same UUID ──────────────────────────────
echo ""
echo "=== 8c. Re-registration ==="

ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'kill \$(pgrep slave_agent) 2>/dev/null'"
echo "slave_agent killed. Waiting 20s for suspect/offline..."
sleep 20

echo "--- node_registry after kill ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_registry;" | tee ~/exp_data/F/node_registry_offline.txt

# Restart with SAME UUID
ssh huaz@172.28.4.77 "ssh root@192.168.100.2 'export LD_LIBRARY_PATH=/opt/mellanox/grpc/lib:\${LD_LIBRARY_PATH:-}; nohup ~/experiments/build/control-plane/slave/slave_agent --node-uuid=fujian-bf2 --master-addr=192.168.56.10:${GRPC_PORT} --dev-pci=${NIC_PCI} --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} > /tmp/slave_agent.log 2>&1 &'"
sleep 10

echo "--- node_registry after re-registration ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_registry;" | tee ~/exp_data/F/node_registry_reregistered.txt
echo ""
echo "--- cluster_events showing lifecycle ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM cluster_events ORDER BY time;" | tee ~/exp_data/F/lifecycle_events.txt
echo ""
echo "--- cluster_master log re-registration ---"
grep -i "fujian" ~/exp_data/F/cluster_master.log | tail -20 | tee ~/exp_data/F/reregistration_log.txt

# ── 8d. Cleanup ──────────────────────────────────────────────────────
pkill cluster_master 2>/dev/null
ssh huaz@172.28.4.77 "pkill -f metric_push 2>/dev/null; ssh root@192.168.100.2 'kill \$(pgrep slave_agent) 2>/dev/null'"

echo ""
echo "=== Experiment F complete ==="
