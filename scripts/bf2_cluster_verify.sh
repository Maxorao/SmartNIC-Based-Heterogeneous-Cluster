#!/bin/bash
# bf2_cluster_verify.sh — Full-cluster connectivity and RDMA verification
#
# Run from tianjin (master node). Checks:
#   1. 100G host<->host connectivity
#   2. Host<->local BF2 tmfifo reachability
#   3. BF2<->BF2 IP connectivity (fabric IPs)
#   4. BF2<->BF2 RoCE hardware path (ib_write_lat short test)
#   5. OVS hw-offload state on each BF2
#
# Output: pass/fail matrix to stdout + ~/exp_data/cluster_verify.log

set -uo pipefail   # intentionally no -e: we want to continue through failures

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

LOG="${DATA_DIR}/cluster_verify.log"
mkdir -p "${DATA_DIR}"
: > "${LOG}"

pass_count=0
fail_count=0
total=0

check() {
    local name="$1"; shift
    local cmd="$*"
    total=$((total+1))
    if eval "$cmd" >>"${LOG}" 2>&1; then
        echo "  [OK]   ${name}"
        pass_count=$((pass_count+1))
    else
        echo "  [FAIL] ${name}"
        fail_count=$((fail_count+1))
    fi
}

echo "=========================================="
echo " BF2 Cluster Verification"
echo " Log: ${LOG}"
echo "=========================================="

# ---------------------------------------------------------------------------
# Section 1: Host<->Host 100G connectivity
# ---------------------------------------------------------------------------
echo ""
echo "[1/5] Host<->Host 100G connectivity"

check "tianjin -> fujian (100G)"  ping -c 2 -W 2 "${FUJIAN_100G}"
check "tianjin -> helong (100G)"  ping -c 2 -W 2 "${HELONG_100G}"

# ---------------------------------------------------------------------------
# Section 2: Host<->BF2 tmfifo
# ---------------------------------------------------------------------------
echo ""
echo "[2/5] Host<->BF2 tmfifo reachability"

check "tianjin -> local BF2" ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no \
    "root@${BF_IP}" "hostname >/dev/null"

check "fujian host -> local BF2" ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no \
    "${USER}@${FUJIAN_IP}" \
    "ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no root@${BF_IP} 'hostname >/dev/null'"

check "helong host -> local BF2" ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no \
    "${USER}@${HELONG_IP}" \
    "ssh -o ConnectTimeout=3 -o StrictHostKeyChecking=no root@${BF_IP} 'hostname >/dev/null'"

# ---------------------------------------------------------------------------
# Section 3: BF2<->BF2 IP connectivity (100G fabric)
# ---------------------------------------------------------------------------
echo ""
echo "[3/5] BF2<->BF2 IP connectivity (fabric)"

check "tianjin-BF2 -> fujian-BF2 fabric" ssh "root@${BF_IP}" \
    "ping -c 2 -W 2 ${FUJIAN_BF2_FABRIC}"
check "tianjin-BF2 -> helong-BF2 fabric" ssh "root@${BF_IP}" \
    "ping -c 2 -W 2 ${HELONG_BF2_FABRIC}"

# ---------------------------------------------------------------------------
# Section 4: OVS hw-offload state
# ---------------------------------------------------------------------------
echo ""
echo "[4/5] OVS hardware offload"

# run_on_bf2 HOST_SSH CMD — run CMD on BF2 via one-hop or two-hop SSH
# HOST_SSH: "direct" for tianjin (local BF2), or "user@host" for remote hosts
run_on_bf2() {
    local host_ssh="$1"
    local cmd="$2"
    if [ "${host_ssh}" = "direct" ]; then
        ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
            "root@${BF_IP}" "${cmd}" 2>/dev/null
    else
        ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
            "${host_ssh}" "ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
            root@${BF_IP} '${cmd}'" 2>/dev/null
    fi
}

hw_offload_check() {
    local host_ssh="$1"
    local state
    state=$(run_on_bf2 "${host_ssh}" \
        "ovs-vsctl get Open_vSwitch . other_config:hw-offload 2>/dev/null" || echo "")
    if [ "${state}" = '"true"' ]; then
        return 0
    fi
    return 1
}

check "tianjin-BF2 hw-offload=true" hw_offload_check "direct"
check "fujian-BF2 hw-offload=true"  hw_offload_check "${USER}@${FUJIAN_IP}"
check "helong-BF2 hw-offload=true"  hw_offload_check "${USER}@${HELONG_IP}"

# ---------------------------------------------------------------------------
# Section 5: BF2<->BF2 RoCE hardware path (ib_write_lat)
# ---------------------------------------------------------------------------
echo ""
echo "[5/5] BF2<->BF2 RoCE latency (ib_write_lat, 128 B)"

# Detect SF RDMA device on each BF2 (the device whose netdev starts with "enp3s")
detect_rdma_dev() {
    local host_ssh="$1"
    run_on_bf2 "${host_ssh}" \
        'for dev in /sys/class/infiniband/mlx5_*; do for nd in $dev/device/net/enp3s*; do if [ -e $nd ]; then basename $dev; exit 0; fi; done; done'
}

run_rdma_lat() {
    local server_host="$1"
    local client_host="$2"
    local server_ip="$3"

    local sdev cdev
    sdev=$(detect_rdma_dev "${server_host}")
    cdev=$(detect_rdma_dev "${client_host}")

    if [ -z "${sdev}" ] || [ -z "${cdev}" ]; then
        echo "no RDMA device detected (server=${sdev:-none}, client=${cdev:-none})"
        return 1
    fi

    # Server in background — setsid ensures process survives SSH disconnect
    run_on_bf2 "${server_host}" "pkill -f ib_write_lat 2>/dev/null" >/dev/null 2>&1
    sleep 1
    run_on_bf2 "${server_host}" \
        "setsid ib_write_lat -d ${sdev} -F -s 128 -n 200 </dev/null >/tmp/ibwl_server.log 2>&1 &"
    sleep 3

    # Client connects and runs
    local out
    out=$(run_on_bf2 "${client_host}" \
        "timeout 15 ib_write_lat -d ${cdev} -F -s 128 -n 200 ${server_ip} 2>&1 | tail -5")

    run_on_bf2 "${server_host}" "pkill -f ib_write_lat 2>/dev/null" >/dev/null 2>&1

    # Extract t_avg from typical output line:
    # "#bytes #iterations t_min[usec] t_max[usec] t_typical[usec] t_avg[usec] ..."
    local avg
    avg=$(echo "${out}" | awk '/^[0-9]+[[:space:]]+[0-9]+/ {print $6; exit}')

    if [ -n "${avg}" ]; then
        # sanity: avg < 20 us
        local avg_int
        avg_int=$(printf "%.0f" "${avg}")
        if [ "${avg_int}" -lt 20 ]; then
            echo "t_avg=${avg} us"
            return 0
        else
            echo "t_avg=${avg} us (>20us, likely software slow path)"
            return 1
        fi
    fi
    echo "no avg latency parsed"
    return 1
}

# tianjin-BF2 <-> fujian-BF2
echo -n "  tianjin-BF2 <-> fujian-BF2: "
out=$(run_rdma_lat "direct" "${USER}@${FUJIAN_IP}" "192.168.56.102")
rc=$?
echo "${out}"
total=$((total+1))
if [ $rc -eq 0 ]; then pass_count=$((pass_count+1)); else fail_count=$((fail_count+1)); fi

# tianjin-BF2 <-> helong-BF2
echo -n "  tianjin-BF2 <-> helong-BF2: "
out=$(run_rdma_lat "direct" "${USER}@${HELONG_IP}" "192.168.56.102")
rc=$?
echo "${out}"
total=$((total+1))
if [ $rc -eq 0 ]; then pass_count=$((pass_count+1)); else fail_count=$((fail_count+1)); fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "=========================================="
echo " Summary: ${pass_count}/${total} passed, ${fail_count} failed"
echo " Full log: ${LOG}"
echo "=========================================="

if [ "${fail_count}" -gt 0 ]; then
    exit 1
fi
exit 0
