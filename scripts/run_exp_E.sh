#!/bin/bash
set -uo pipefail
source ~/experiments/scripts/config.sh
export PGPASSWORD=postgres
mkdir -p ~/exp_data/E

echo "=== Experiment E: Database Performance ==="

# ── 7a. Collect real data for 5 minutes ───────────────────────────────
echo "System running. Collecting real data for 5 minutes..."
sleep 300
echo "Data collection complete."
echo ""

# ── 7b. Write throughput ─────────────────────────────────────────────
echo "=== DB Write Throughput Test ==="

echo "--- 64 nodes, 60s ---"
${MOCK_SLAVE} --master-addr=192.168.56.10:${GRPC_PORT} \
  --nodes=64 --interval=1000 --duration=60 \
  > ~/exp_data/E/mock_64nodes.log 2>&1
ROW_COUNT=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT COUNT(*) FROM host_metrics WHERE time > NOW() - INTERVAL '2 minutes';" | tr -d ' ')
echo "Rows inserted (64 nodes, 60s): ${ROW_COUNT}"
RATE=$((ROW_COUNT / 60))
echo "Write rate: ~${RATE} rows/s"
echo ""

sleep 5

echo "--- 256 nodes, 60s ---"
${MOCK_SLAVE} --master-addr=192.168.56.10:${GRPC_PORT} \
  --nodes=256 --interval=1000 --duration=60 \
  > ~/exp_data/E/mock_256nodes.log 2>&1
ROW_COUNT=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT COUNT(*) FROM host_metrics WHERE time > NOW() - INTERVAL '2 minutes';" | tr -d ' ')
echo "Rows inserted (256 nodes, 60s): ${ROW_COUNT}"
RATE=$((ROW_COUNT / 60))
echo "Write rate: ~${RATE} rows/s"
echo ""

# ── 7c. Query latency ───────────────────────────────────────────────
echo "=== DB Query Latency Test ==="

echo "--- Query: Node registry status ---"
for i in $(seq 1 10); do
  psql -h localhost -U postgres -d cluster_metrics -c \
    "\\timing on
     SELECT node_uuid, state, host_status, bf2_status, last_seen FROM node_registry ORDER BY node_uuid;" \
    2>&1 | grep "Time:"
done | tee ~/exp_data/E/query_status.txt
echo ""

echo "--- Query: 5-min CPU aggregation ---"
for i in $(seq 1 10); do
  psql -h localhost -U postgres -d cluster_metrics -c \
    "\\timing on
     SELECT node_uuid, AVG(cpu_pct) as avg_cpu, MAX(cpu_pct) as max_cpu
     FROM host_metrics
     WHERE time > NOW() - INTERVAL '5 minutes'
     GROUP BY node_uuid ORDER BY node_uuid;" \
    2>&1 | grep "Time:"
done | tee ~/exp_data/E/query_5min_agg.txt
echo ""

echo "--- Query: 1-hour time-bucket aggregation ---"
for i in $(seq 1 10); do
  psql -h localhost -U postgres -d cluster_metrics -c \
    "\\timing on
     SELECT node_uuid, time_bucket('1 minute', time) AS bucket, AVG(cpu_pct) as avg_cpu
     FROM host_metrics
     WHERE time > NOW() - INTERVAL '1 hour'
     GROUP BY node_uuid, bucket ORDER BY node_uuid, bucket;" \
    2>&1 | grep "Time:"
done | tee ~/exp_data/E/query_1hour_agg.txt
echo ""

echo "--- Table statistics ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT hypertable_name, num_chunks,
          pg_size_pretty(hypertable_size(format('%I', hypertable_name)::regclass)) as total_size
   FROM timescaledb_information.hypertables
   WHERE hypertable_name IN ('host_metrics', 'bf2_metrics', 'cluster_events');" | tee ~/exp_data/E/table_stats.txt
echo ""

TOTAL_HOST=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT COUNT(*) FROM host_metrics;" | tr -d ' ')
TOTAL_BF2=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT COUNT(*) FROM bf2_metrics;" | tr -d ' ')
echo "Total rows: host_metrics=${TOTAL_HOST}  bf2_metrics=${TOTAL_BF2}"
echo ""

# ── 7d. Compression ─────────────────────────────────────────────────
echo "=== Compression Test ==="

BEFORE_SIZE=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT pg_size_pretty(hypertable_size('host_metrics'));" | tr -d ' ')
echo "Before compression: ${BEFORE_SIZE}"

psql -h localhost -U postgres -d cluster_metrics -c \
  "ALTER TABLE host_metrics SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'node_uuid'
  );" 2>/dev/null

psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT compress_chunk(c.chunk_name)
   FROM timescaledb_information.chunks c
   WHERE c.hypertable_name = 'host_metrics'
     AND NOT c.is_compressed
   ORDER BY c.range_start;" 2>/dev/null

AFTER_SIZE=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT pg_size_pretty(hypertable_size('host_metrics'));" | tr -d ' ')
echo "After compression: ${AFTER_SIZE}"

psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT chunk_name, is_compressed,
          pg_size_pretty(before_compression_total_bytes) as before,
          pg_size_pretty(after_compression_total_bytes) as after
   FROM timescaledb_information.compressed_chunk_stats
   WHERE hypertable_name = 'host_metrics'
   ORDER BY chunk_name;" | tee ~/exp_data/E/compression_stats.txt
echo ""

echo "=== Experiment E complete ==="
