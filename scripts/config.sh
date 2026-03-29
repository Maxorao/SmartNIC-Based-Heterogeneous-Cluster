#!/bin/bash
# config.sh — Multi-host cluster configuration
#
# Topology:
#   tianjin (172.28.4.75) — master node
#   fujian  (172.28.4.77) — worker node
#   helong  (172.28.4.85) — worker node
#
# BF2 SmartNIC fabric (100G switch):
#   tianjin BF2 p1: 192.168.56.2
#   fujian  BF2 p1: 192.168.56.3
#   helong  BF2 p0: 192.168.56.1
#
# Each host reaches its own BF2 via tmfifo (192.168.100.1 ↔ .2)

# ---------------------------------------------------------------
# Host IPs (management LAN — eno1)
# ---------------------------------------------------------------
TIANJIN_IP="172.28.4.75"
FUJIAN_IP="172.28.4.77"
HELONG_IP="172.28.4.85"

# ---------------------------------------------------------------
# BF2 fabric IPs (100G switch — assigned to BF2 ARM ports)
# ---------------------------------------------------------------
TIANJIN_BF2_FABRIC="192.168.56.2"   # p1
FUJIAN_BF2_FABRIC="192.168.56.3"    # p1
HELONG_BF2_FABRIC="192.168.56.1"    # p0

# ---------------------------------------------------------------
# Local BF2 access (tmfifo — same on every host)
# ---------------------------------------------------------------
BF_IP="192.168.100.2"

# ---------------------------------------------------------------
# Host-side 100G IPs (on enp94s0f* interfaces, same 56.x subnet)
# ---------------------------------------------------------------
TIANJIN_100G="192.168.56.10"    # enp94s0f1np1
FUJIAN_100G="192.168.56.11"     # enp94s0f1np1
HELONG_100G="192.168.56.12"     # enp94s0f0np0

# ---------------------------------------------------------------
# Master configuration
# ---------------------------------------------------------------
MASTER_HOST="${TIANJIN_IP}"
MASTER_100G="${TIANJIN_100G}"   # master reachable via 100G fabric
MASTER_PORT="9000"

# ---------------------------------------------------------------
# PCI addresses (same across all three hosts)
# ---------------------------------------------------------------
HOST_PCI="0000:5e:00.0"     # BF2 as seen from host (DOCA 3.1 full BDF)
NIC_PCI="03:00.0"           # BF2 device as seen from ARM

# ---------------------------------------------------------------
# Data output
# ---------------------------------------------------------------
DATA_DIR="${HOME}/exp_data"
mkdir -p "${DATA_DIR}"/{A,B,C,kubelet} 2>/dev/null

# ---------------------------------------------------------------
# Binary paths (host side)
# ---------------------------------------------------------------
EXP_BASE="${HOME}/experiments"

BENCH_HOST="${EXP_BASE}/bench/latency_bench/bench_host"
GEMM_BENCH="${EXP_BASE}/bench/gemm_bench/gemm_bench"
MOCK_SLAVE="${EXP_BASE}/bench/mock_slave/mock_slave"
SLAVE_MONITOR="${EXP_BASE}/control-plane/slave/slave_monitor"
MASTER_MONITOR="${EXP_BASE}/control-plane/master/master_monitor"
METRIC_PUSH="${EXP_BASE}/bench/metric_push/metric_push"

# Remote binary paths (on BF2 ARM)
NIC_BENCH_NIC="/root/experiments/bench/latency_bench/bench_nic"
NIC_FORWARD_ROUTINE="/root/experiments/control-plane/forwarder/forward_routine"

# ---------------------------------------------------------------
# Experiment B: Interference parameters
# ---------------------------------------------------------------
# GEMM uses one full NUMA socket (16 physical cores)
NUMA_NODE=0
GEMM_THREADS=16

# Number of slave_monitor / metric_push instances
# (simulates kubelet + metrics + logging + health checks)
N_MONITORS=8

# Report interval per instance (ms)
HIGH_LOAD_INTERVAL=10

# Duration per scenario (seconds)
GEMM_DURATION=60

# ---------------------------------------------------------------
# Experiment A: Latency parameters
# ---------------------------------------------------------------
BENCH_ITERS=10000
BENCH_WARMUP=200

# ---------------------------------------------------------------
# Experiment C: Scalability
# ---------------------------------------------------------------
SCALE_NODES="4 16 64 256"
SCALE_DURATION=30

# ---------------------------------------------------------------
# DB connection (optional — for master_monitor persistence)
# ---------------------------------------------------------------
DB_CONNSTR="host=localhost dbname=cluster_metrics user=postgres password=postgres"
