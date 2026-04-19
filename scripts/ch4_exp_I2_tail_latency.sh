#!/bin/bash
# ch4_exp_I2_tail_latency.sh — Nginx P50/P99/P999 tail latency on x86 vs ARM
#
# Extends Experiment I with wrk --latency at 4 concurrency levels.
# Outputs detailed percentile distribution for each config.
#
# Usage:
#   bash ch4_exp_I2_tail_latency.sh                   # run both platforms
#   bash ch4_exp_I2_tail_latency.sh x86               # only x86
#   bash ch4_exp_I2_tail_latency.sh arm               # only ARM
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

PLATFORMS="${1:-both}"
DURATION="${DURATION:-30}"
WRK_THREADS="${WRK_THREADS:-4}"
CONCURRENCIES="50 100 200 400"

OUT_DIR="${DATA_DIR}/I_tail"
mkdir -p "$OUT_DIR"

echo "=== Experiment I2: Nginx Tail Latency (x86 vs ARM) ==="
echo "Duration: ${DURATION}s × 4 concurrency levels"
echo "Output: ${OUT_DIR}"

# wrk Lua script to dump full HdrHistogram as CSV-friendly output
WRK_LUA="${OUT_DIR}/latency_dump.lua"
cat > "${WRK_LUA}" <<'EOF'
-- Dump representative percentiles from wrk's HDR histogram.
-- wrk exposes latency:percentile(p) after the run.
done = function(summary, latency, requests)
    io.write("---PCTL_BEGIN---\n")
    for _, p in ipairs({50, 75, 90, 95, 99, 99.9, 99.99}) do
        io.write(string.format("pctl %.2f %.3fms\n",
                               p, latency:percentile(p) / 1000.0))
    end
    io.write("---PCTL_END---\n")
end
EOF

run_wrk() {
    local platform="$1"
    local target="$2"
    local cores_label="$3"

    echo ""
    echo "--- Platform: ${platform} (cores: ${cores_label}) ---"

    for CONC in ${CONCURRENCIES}; do
        echo "  concurrency=${CONC} ..."
        outfile="${OUT_DIR}/${platform}_c${CONC}.txt"
        wrk -t"${WRK_THREADS}" -c"${CONC}" -d"${DURATION}s" \
            --latency \
            -s "${WRK_LUA}" \
            "http://${target}/" \
            > "${outfile}" 2>&1 || true
        rps=$(grep -E "Requests/sec:" "${outfile}" | awk '{print $2}')
        p99=$(grep -E "^pctl 99\.00" "${outfile}" | awk '{print $3}')
        p999=$(grep -E "^pctl 99\.90" "${outfile}" | awk '{print $3}')
        echo "     rps=${rps:-?} p99=${p99:-?} p99.9=${p999:-?}"
    done
}

# ---------------------------------------------------------------------------
# x86 side: Nginx on fujian host, bound to NUMA1 (16-31) to avoid DGEMM cores
# ---------------------------------------------------------------------------
if [ "${PLATFORMS}" = "both" ] || [ "${PLATFORMS}" = "x86" ]; then
    echo ""
    echo "[x86] Starting Nginx on fujian host (NUMA1 cores 16-31)..."
    ssh "${USER}@${FUJIAN_IP}" \
        "docker rm -f nginx-bench 2>/dev/null; \
         docker run -d --name nginx-bench --network=host \
         --cpuset-cpus=16-31 nginx:alpine"
    sleep 3

    run_wrk "x86" "${FUJIAN_100G}" "16-31 (NUMA1)"

    ssh "${USER}@${FUJIAN_IP}" "docker rm -f nginx-bench 2>/dev/null || true"
fi

# ---------------------------------------------------------------------------
# ARM side: Nginx on fujian BF2, using all 8 ARM cores
# ---------------------------------------------------------------------------
if [ "${PLATFORMS}" = "both" ] || [ "${PLATFORMS}" = "arm" ]; then
    echo ""
    echo "[ARM] Starting Nginx on fujian BF2 (cores 0-7)..."
    ssh "${USER}@${FUJIAN_IP}" \
        "ssh root@${BF_IP} 'docker rm -f nginx-bench 2>/dev/null; \
         docker run -d --name nginx-bench --network=host nginx:alpine'"
    sleep 3

    run_wrk "arm" "${FUJIAN_BF2_FABRIC}" "0-7 (all ARM cores)"

    ssh "${USER}@${FUJIAN_IP}" \
        "ssh root@${BF_IP} 'docker rm -f nginx-bench 2>/dev/null || true'"
fi

echo ""
echo "=== Experiment I2 Complete ==="
echo "Run: python3 ${SCRIPT_DIR}/analyze/analyze_I_tail.py"
