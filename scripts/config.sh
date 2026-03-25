#!/bin/bash
# config.sh — Shared cluster configuration for all experiment scripts.
# Edit this file before running any experiment.

# -------------------------------------------------------------------------
# Cluster node IPs
# -------------------------------------------------------------------------
MASTER_IP="192.168.1.1"       # gnode1 (master_monitor runs here)
MASTER_PORT="9000"

GNODE1_IP="192.168.1.1"
GNODE2_IP="192.168.1.2"
GNODE3_IP="192.168.1.3"
GNODE4_IP="192.168.1.4"

# SmartNIC management IPs (for SSH and TCP baseline)
GNODE1_BF_IP="192.168.1.10"
GNODE2_BF_IP="192.168.1.11"
GNODE3_BF_IP="192.168.1.12"
GNODE4_BF_IP="192.168.1.13"

# BlueField-3 PCI addresses as seen from each host
GNODE1_PCI="03:00.0"
GNODE2_PCI="03:00.0"
GNODE3_PCI="03:00.0"
GNODE4_PCI="03:00.0"

# SSH user for remote nodes
SSH_USER="${SSH_USER:-$(whoami)}"
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes"

# -------------------------------------------------------------------------
# Data output directory
# -------------------------------------------------------------------------
DATA_DIR="${HOME}/exp_data"
mkdir -p "${DATA_DIR}"/{A,B,C,D,kubelet}

# -------------------------------------------------------------------------
# Binary paths (adjust after building)
# -------------------------------------------------------------------------
EXP_BASE="${HOME}/experiments"

BENCH_HOST="${EXP_BASE}/bench/latency_bench/bench_host"
BENCH_NIC="${EXP_BASE}/bench/latency_bench/bench_nic"
GEMM_BENCH="${EXP_BASE}/bench/gemm_bench/gemm_bench"
MOCK_SLAVE="${EXP_BASE}/bench/mock_slave/mock_slave"
SLAVE_MONITOR="${EXP_BASE}/control-plane/slave/slave_monitor"
MASTER_MONITOR="${EXP_BASE}/control-plane/master/master_monitor"
FORWARD_ROUTINE="${EXP_BASE}/control-plane/forwarder/forward_routine"

# Remote binary paths (on SmartNIC ARM)
NIC_BENCH_NIC="/root/experiments/bench/latency_bench/bench_nic"
NIC_FORWARD_ROUTINE="/root/experiments/control-plane/forwarder/forward_routine"

# -------------------------------------------------------------------------
# Experiment parameters
# -------------------------------------------------------------------------

# CPU cores for gemm_bench (avoid core 0 and DPDK/DOCA cores)
COMPUTE_CORES="4-7"

# Report intervals (milliseconds)
HIGH_LOAD_INTERVAL=100
NORMAL_INTERVAL=1000

# Benchmark iterations
BENCH_ITERS=10000

# Experiment durations (seconds)
GEMM_DURATION=60
SCALE_DURATION=30
FAULT_REPEAT=5

# DB connection string for master_monitor
DB_CONNSTR="host=localhost dbname=cluster_metrics user=cluster password=cluster"
