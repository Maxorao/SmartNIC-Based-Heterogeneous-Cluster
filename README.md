# SmartNIC Heterogeneous Cluster Acceleration — Experiments

This directory contains all experiment code for the master's thesis:
**"SmartNIC-Based Heterogeneous Cluster Acceleration for Distributed Systems Control Planes"**

## Hardware Platform

| Node | Role | CPU | OS | Notes |
|------|------|-----|----|-------|
| gnode1 | master + worker | Intel Xeon Gold 6526Y | Ubuntu 22.04 | Runs master_monitor |
| gnode2-4 | workers | Intel Xeon Gold 6526Y | Ubuntu 22.04 | Run slave_monitor |
| gnode1-4-bf | SmartNICs | BF3 ARM Cortex-A78 | Ubuntu 20.04 | Run forward_routine |

Software: DPDK 21.08, DOCA 2.7.0, TimescaleDB 2.x, OpenBLAS.

## Directory Layout

```
experiments/
├── common/              # Shared headers (protocol, timing)
├── tunnel/              # DOCA Comch host↔NIC tunnel
│   ├── host/            # x86 side (comch_host.c)
│   └── nic/             # ARM side (comch_nic.c)
├── control-plane/       # Main control-plane components
│   ├── slave/           # Worker resource monitor (x86)
│   ├── master/          # Cluster master + TimescaleDB writer (x86)
│   └── forwarder/       # SmartNIC-side forwarder (ARM)
├── bench/               # Benchmark programs
│   ├── latency_bench/   # Experiment A: tunnel latency
│   ├── gemm_bench/      # Experiment B: compute interference
│   └── mock_slave/      # Experiment C: scalability simulation
└── scripts/             # Orchestration + analysis
    ├── config.sh
    ├── exp_A_latency.sh
    ├── exp_B_interference.sh
    ├── exp_C_scale.sh
    ├── exp_D_fault_recovery.sh
    ├── exp_kubelet.sh
    └── analyze/
        ├── analyze_A.py
        ├── analyze_B.py
        └── analyze_C.py
```

## Build Instructions

### Prerequisites

**On x86 hosts (gnode1-4, Ubuntu 22.04):**
```bash
sudo apt-get install -y build-essential libpq-dev libopenblas-dev
# DOCA 2.7.0 must be installed at /opt/mellanox/doca
```

**On SmartNIC ARM (gnodeX-bf, Ubuntu 20.04):**
```bash
sudo apt-get install -y build-essential
# DOCA 2.7.0 ARM build must be installed at /opt/mellanox/doca
```

### Build Order

Build tunnel libraries first (they are included by other components):

```bash
# 1. Tunnel host side (on x86)
cd experiments/tunnel/host && make

# 2. Tunnel NIC side (on ARM / cross-compile)
cd experiments/tunnel/nic && make

# 3. Control plane (on respective machines)
cd experiments/control-plane/master && make        # on gnode1
cd experiments/control-plane/slave && make         # on gnode2-4
cd experiments/control-plane/forwarder && make     # on gnodeX-bf (ARM)

# 4. Benchmarks
cd experiments/bench/latency_bench && make         # bench_host on x86
# Copy bench_nic source to SmartNIC and build there
cd experiments/bench/gemm_bench && make            # on x86
cd experiments/bench/mock_slave && make            # on x86
```

### DOCA Path Override

If DOCA is installed elsewhere:
```bash
make DOCA_DIR=/path/to/doca
```

## Experiments

### Experiment A — Tunnel Latency

Measures one-way latency of DOCA Comch vs kernel TCP, across message sizes 64B–64KB.

```bash
# Edit scripts/config.sh first
vim scripts/config.sh

# Run
bash scripts/exp_A_latency.sh

# Analyze
python3 scripts/analyze/analyze_A.py
```

Expected output directory: `~/exp_data/A/`

### Experiment B — Compute Interference

Measures GEMM throughput degradation caused by the control-plane agent under three scenarios:
1. Baseline (no agent)
2. Direct TCP mode (agent on host CPU)
3. Offload mode (agent offloaded to SmartNIC)

```bash
bash scripts/exp_B_interference.sh
python3 scripts/analyze/analyze_B.py
```

### Experiment C — Scalability

Simulates 4, 16, 64, 256 slave nodes connecting to master_monitor, measuring master CPU and response latency.

```bash
bash scripts/exp_C_scale.sh
python3 scripts/analyze/analyze_C.py
```

### Experiment D — Fault Recovery

Injects faults (forward_routine kill, slave_monitor restart) and measures detection + recovery time.

```bash
bash scripts/exp_D_fault_recovery.sh
```

### Kubelet Reference Overhead

Measures idle CPU/memory of kubelet vs slave_monitor for comparison.

```bash
bash scripts/exp_kubelet.sh
```

## Database Setup

```bash
# Install TimescaleDB
sudo apt-get install -y timescaledb-2-postgresql-14
sudo timescaledb-tune --quiet --yes
sudo systemctl restart postgresql

# Create database
sudo -u postgres psql -c "CREATE DATABASE cluster_metrics;"
sudo -u postgres psql -c "CREATE USER cluster WITH PASSWORD 'cluster';"
sudo -u postgres psql -c "GRANT ALL ON DATABASE cluster_metrics TO cluster;"

# Schema is auto-created by master_monitor on startup via db_init_schema()
```

## Protocol

All control-plane messages use the binary protocol defined in `common/protocol.h`.
Message format: 16-byte fixed header + variable payload (max 4096 bytes).

Message types:
- `MSG_REGISTER` (1) — node registers with master
- `MSG_REGISTER_ACK` (2) — master acknowledges registration
- `MSG_HEARTBEAT` (3) — periodic keepalive
- `MSG_HEARTBEAT_ACK` (4)
- `MSG_RESOURCE_REPORT` (5) — CPU/mem/net metrics
- `MSG_COMMAND` (6) — master command to node
- `MSG_COMMAND_ACK` (7)
- `MSG_DEREGISTER` (8) — clean shutdown
- `MSG_BENCH_PING` (100) — latency benchmark ping
- `MSG_BENCH_PONG` (101) — latency benchmark pong
