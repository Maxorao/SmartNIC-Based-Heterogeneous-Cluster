#!/bin/bash
# config.sh — tianjin single-host configuration

# This node acts as both master and the single worker for these experiments
MASTER_IP="172.28.4.75"
MASTER_PORT="9000"

HOST_IP="172.28.4.75"
BF_IP="192.168.100.2"

# BF2 PCI addresses
HOST_PCI="0000:5e:00.0"  # BF2 as seen from host (full BDF)
NIC_PCI="03:00.0"       # BF2 device as seen from ARM

# ---------------------------------------------------------------
# Data output directory
# ---------------------------------------------------------------
DATA_DIR="${HOME}/exp_data"
mkdir -p "${DATA_DIR}"/{A,B,C,kubelet}

# ---------------------------------------------------------------
# Binary paths
# ---------------------------------------------------------------
EXP_BASE="${HOME}/experiments"

BENCH_HOST="${EXP_BASE}/bench/latency_bench/bench_host"
GEMM_BENCH="${EXP_BASE}/bench/gemm_bench/gemm_bench"
MOCK_SLAVE="${EXP_BASE}/bench/mock_slave/mock_slave"
SLAVE_MONITOR="${EXP_BASE}/control-plane/slave/slave_monitor"
MASTER_MONITOR="${EXP_BASE}/control-plane/master/master_monitor"

# Remote binary paths (on BF2 ARM)
NIC_BENCH_NIC="/root/experiments/bench/latency_bench/bench_nic"
NIC_FORWARD_ROUTINE="/root/experiments/control-plane/forwarder/forward_routine"

# ---------------------------------------------------------------
# Experiment parameters
# ---------------------------------------------------------------
COMPUTE_CORES="4-7"          # CPU cores for gemm_bench (avoid core 0)
HIGH_LOAD_INTERVAL=10        # ms — slave report interval under load (aggressive: 100 reports/s to stress compute cores)
NORMAL_INTERVAL=1000         # ms — normal slave report interval
BENCH_ITERS=10000            # ping-pong iterations per size
GEMM_DURATION=60             # seconds per GEMM phase

# DB connection (only needed if TimescaleDB is installed)
DB_CONNSTR="host=localhost dbname=cluster_metrics user=postgres password=postgres"
