#!/bin/bash
# ch4_bf2_nginx.sh — Experiment H: SmartNIC Nginx execution performance
#
# Measures Nginx performance on BF2 ARM vs x86 host as reference data.
# Tests multiple concurrency levels to characterize the performance envelope.

set -e
source "$(dirname "$0")/config.sh"

WORKER="${FUJIAN_IP}"
WORKER_100G="${FUJIAN_100G}"
BF2_100G="${FUJIAN_BF2_FABRIC}"
BF2_SSH="root@192.168.100.2"
DURATION=30
WRK_THREADS=4
DATA="${DATA_DIR}/ch4_H"
mkdir -p "${DATA}"

CONCURRENCIES="10 50 100 200 400"

echo "============================================"
echo "  Experiment H: BF2 Nginx Performance"
echo "============================================"

# -------------------------------------------------------------------
# H.1: Nginx on x86 host (baseline reference)
# -------------------------------------------------------------------
echo ""
echo "--- H.1: Nginx on x86 host ---"

ssh "$(whoami)@${WORKER}" "
    docker rm -f nginx-bench 2>/dev/null
    docker run -d --name nginx-bench --network=host nginx:alpine
    sleep 3
"

echo "conns,req_per_sec,avg_latency_ms,p99_latency_ms,transfer_per_sec" \
    > "${DATA}/nginx_x86.csv"

for conns in ${CONCURRENCIES}; do
    echo "  wrk -c${conns} -d${DURATION}s → ${WORKER_100G}"
    result=$(wrk -t${WRK_THREADS} -c${conns} -d${DURATION}s \
        "http://${WORKER_100G}/" 2>&1)

    rps=$(echo "$result" | grep "Requests/sec" | awk '{print $2}')
    avg_lat=$(echo "$result" | grep "Latency" | awk '{print $2}' | sed 's/[a-z]//g')
    transfer=$(echo "$result" | grep "Transfer/sec" | awk '{print $2}')

    echo "  → ${rps} req/s, ${avg_lat} avg latency"
    echo "${conns},${rps},${avg_lat},,${transfer}" >> "${DATA}/nginx_x86.csv"
done

ssh "$(whoami)@${WORKER}" "docker rm -f nginx-bench"

# -------------------------------------------------------------------
# H.2: Nginx on BF2 ARM
# -------------------------------------------------------------------
echo ""
echo "--- H.2: Nginx on BF2 ARM ---"

ssh "$(whoami)@${WORKER}" "
    ssh ${BF2_SSH} '
        docker rm -f nginx-bench 2>/dev/null
        docker run -d --name nginx-bench --network=host nginx:alpine
        sleep 3
    '
"

echo "conns,req_per_sec,avg_latency_ms,p99_latency_ms,transfer_per_sec" \
    > "${DATA}/nginx_bf2.csv"

for conns in ${CONCURRENCIES}; do
    echo "  wrk -c${conns} -d${DURATION}s → ${BF2_100G}"
    result=$(wrk -t${WRK_THREADS} -c${conns} -d${DURATION}s \
        "http://${BF2_100G}/" 2>&1)

    rps=$(echo "$result" | grep "Requests/sec" | awk '{print $2}')
    avg_lat=$(echo "$result" | grep "Latency" | awk '{print $2}' | sed 's/[a-z]//g')
    transfer=$(echo "$result" | grep "Transfer/sec" | awk '{print $2}')

    echo "  → ${rps} req/s, ${avg_lat} avg latency"
    echo "${conns},${rps},${avg_lat},,${transfer}" >> "${DATA}/nginx_bf2.csv"
done

# Capture BF2 resource usage during peak load
echo ""
echo "--- H.3: BF2 resource usage under load ---"
ssh "$(whoami)@${WORKER}" "
    ssh ${BF2_SSH} '
        echo \"=== BF2 CPU/Memory during Nginx ===\"
        top -bn1 | head -5
        echo \"\"
        echo \"=== Docker stats ===\"
        docker stats --no-stream nginx-bench 2>/dev/null || true
        echo \"\"
        echo \"=== Slave agent coexistence ===\"
        ps aux | grep -E \"slave_agent|nginx\" | grep -v grep || echo \"(no slave_agent running)\"
    '
" | tee "${DATA}/bf2_resources.txt"

ssh "$(whoami)@${WORKER}" "ssh ${BF2_SSH} 'docker rm -f nginx-bench'"

# -------------------------------------------------------------------
# Summary
# -------------------------------------------------------------------
echo ""
echo "============================================"
echo "  Experiment H Summary"
echo "============================================"
echo "--- x86 Nginx ---"
cat "${DATA}/nginx_x86.csv"
echo ""
echo "--- BF2 Nginx ---"
cat "${DATA}/nginx_bf2.csv"
echo ""
echo "Data saved to: ${DATA}/"
