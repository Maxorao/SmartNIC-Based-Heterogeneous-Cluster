# Remote Experiment Setup & Execution Guide

This document is intended for a Claude agent running on **tianjin** (172.28.4.75).
All experiments in this guide use only tianjin and its locally-attached BF2.
Follow the steps in order. Each section ends with a verification command;
do not proceed until it passes.

---

## 0. Context

| Component | Detail |
|-----------|--------|
| Host | tianjin, Ubuntu 22.04, x86, 172.28.4.75 |
| Host DOCA | 3.1.0 (`/opt/mellanox/doca`) |
| Host DPDK | 22.11 (mlnx) |
| BF2 access | `ssh root@192.168.100.2` (no password) |
| BF2 OS | Ubuntu 20.04 ARM |
| BF2 DOCA | 1.5.4003 |
| BF2 PCI (host view) | `5e:00.0` |
| BF2 PCI (ARM view) | `03:00.0` |
| BF2 representor | auto-detected (pf0hpf) |
| Repo | `git@github.com:Maxorao/SmartNIC-Based-Heterogeneous-Cluster.git` |

**Network note**: The BF2 is only reachable from tianjin via `tmfifo_net0`
(192.168.100.1 on host, 192.168.100.2 on BF2).  It cannot reach other hosts
directly.  All experiments here are self-contained to tianjin + its BF2.

---

## 1. Clone Repository

```bash
cd ~
git clone git@github.com:Maxorao/SmartNIC-Based-Heterogeneous-Cluster.git experiments
cd experiments
```

**Verify:**
```bash
ls tunnel/ control-plane/ bench/ scripts/
# Should list all four directories without error
```

---

## 2. Write Configuration

Replace `scripts/config.sh` with the following tianjin-specific values:

```bash
cat > ~/experiments/scripts/config.sh << 'EOF'
#!/bin/bash
# config.sh — tianjin single-host configuration

# This node acts as both master and the single worker for these experiments
MASTER_IP="172.28.4.75"
MASTER_PORT="9000"

HOST_IP="172.28.4.75"
BF_IP="192.168.100.2"

# BF2 PCI addresses
HOST_PCI="5e:00.0"      # BF2 as seen from host
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
HIGH_LOAD_INTERVAL=100       # ms — slave report interval under load
NORMAL_INTERVAL=1000         # ms — normal slave report interval
BENCH_ITERS=10000            # ping-pong iterations per size
GEMM_DURATION=60             # seconds per GEMM phase

# DB connection (only needed if TimescaleDB is installed)
DB_CONNSTR="host=localhost dbname=cluster_metrics user=postgres password=postgres"
EOF
```

**Verify:**
```bash
source ~/experiments/scripts/config.sh && \
  echo "OK: MASTER_IP=${MASTER_IP}  HOST_PCI=${HOST_PCI}  NIC_PCI=${NIC_PCI}"
```

---

## 3. Install Build Dependencies (host)

```bash
sudo apt-get install -y \
  libopenblas-dev \
  linux-tools-$(uname -r) \
  sysstat \
  stress-ng \
  python3-pip
pip3 install numpy pandas
```

**Verify:**
```bash
dpkg -l libopenblas-dev | grep '^ii' && echo "OpenBLAS OK"
perf stat echo test 2>&1 | grep -q "Performance counter" && echo "perf OK"
```

If `perf` reports permission denied:
```bash
sudo sysctl -w kernel.perf_event_paranoid=1
```

---

## 4. Compile: Host-side Components

### 4a. Tunnel object (host, DOCA 3.1)
```bash
cd ~/experiments/tunnel/host
make COMCH_HOST_DOCA_VER=31
# Expected: comch_host.o
```

### 4b. Slave monitor
```bash
cd ~/experiments/control-plane/slave
make
# Expected: slave_monitor
```

### 4c. Master monitor
```bash
cd ~/experiments/control-plane/master
make
# Expected: master_monitor
```

### 4d. Benchmark programs
```bash
cd ~/experiments/bench/gemm_bench    && make   # gemm_bench
cd ~/experiments/bench/latency_bench && make   # bench_host
cd ~/experiments/bench/mock_slave    && make   # mock_slave
```

**Verify (host):**
```bash
ls -la \
  ~/experiments/control-plane/slave/slave_monitor \
  ~/experiments/control-plane/master/master_monitor \
  ~/experiments/bench/gemm_bench/gemm_bench \
  ~/experiments/bench/latency_bench/bench_host \
  ~/experiments/bench/mock_slave/mock_slave
# All five files must exist
```

---

## 5. Compile: BF2-side Components

```bash
# Push sources to BF2
ssh root@192.168.100.2 "rm -rf ~/experiments && mkdir -p ~/experiments/bench"
scp -r ~/experiments/tunnel          root@192.168.100.2:~/experiments/
scp -r ~/experiments/control-plane   root@192.168.100.2:~/experiments/
scp -r ~/experiments/bench/latency_bench root@192.168.100.2:~/experiments/bench/
scp -r ~/experiments/common          root@192.168.100.2:~/experiments/

# Compile on BF2 (DOCA 1.5, ARM)
ssh root@192.168.100.2 "
  cd ~/experiments/tunnel/nic       && make COMCH_NIC_DOCA_VER=15  && \
  cd ~/experiments/control-plane/forwarder && make && \
  cd ~/experiments/bench/latency_bench     && make bench_nic
"
```

**Verify (BF2):**
```bash
ssh root@192.168.100.2 "ls -la \
  ~/experiments/tunnel/nic/comch_nic.o \
  ~/experiments/control-plane/forwarder/forward_routine \
  ~/experiments/bench/latency_bench/bench_nic"
```

---

## 6. Tunnel Compatibility Test (Critical)

This verifies that the DOCA 3.1 host Comch client can handshake with the
DOCA 1.5 BF2 server.  **If this fails, do not proceed — switch to TCP mode
(Section 6b) and report the error verbatim.**

### 6a. DOCA Comch smoke test

**Start echo server on BF2:**
```bash
ssh root@192.168.100.2 \
  "nohup ~/experiments/bench/latency_bench/bench_nic \
     --pci=03:00.0 --mode=comch > /tmp/bench_nic.log 2>&1 &"
sleep 3
```

**Run 10 ping-pong iterations from host:**
```bash
~/experiments/bench/latency_bench/bench_host \
  --pci=5e:00.0 --mode=comch --size=256 --iters=10
```

**Expected output (success):**
```
Comch ping-pong 256B x10: avg=X.XX µs  P99=X.XX µs
```

**If you see a connection error or timeout**, the DOCA versions are not
wire-compatible.  Kill bench_nic (`ssh root@192.168.100.2 pkill bench_nic`)
and proceed to **6b**.

### 6b. TCP fallback (if 6a fails)

Add to `scripts/config.sh`:
```bash
TUNNEL_MODE="tcp"
TUNNEL_TCP_IP="192.168.100.2"
TUNNEL_TCP_PORT="12345"
```

Then repeat the smoke test with TCP:
```bash
ssh root@192.168.100.2 \
  "nohup ~/experiments/bench/latency_bench/bench_nic \
     --mode=tcp > /tmp/bench_nic_tcp.log 2>&1 &"
sleep 2
~/experiments/bench/latency_bench/bench_host \
  --ip=192.168.100.2 --mode=tcp --size=256 --iters=10
```

---

## 7. Experiment A — Tunnel Latency

Run after Section 6 is confirmed working.

```bash
cd ~/experiments
chmod +x scripts/exp_A_latency.sh
bash scripts/exp_A_latency.sh
```

This will:
- Start `bench_nic` on BF2 as echo server
- Run `bench_host` on tianjin for sizes 64 / 256 / 1024 / 4096 / 65536 B
- Save per-sample CSV files to `~/exp_data/A/`

After completion:
```bash
python3 scripts/analyze/analyze_A.py
# Prints P50 / P99 / P99.9 table for all sizes × protocols (Comch and TCP)
```

**Data to collect**: paste the full printed table here.

---

## 8. Experiment B — Interference Elimination

This experiment measures how much a co-located control-plane process hurts
GEMM throughput, and how much is recovered by offloading it to the BF2.

Three phases run automatically:
1. **Baseline** — GEMM alone on tianjin host
2. **Mixed** — GEMM + slave_monitor both on tianjin host (high-frequency reporting)
3. **Offloaded** — GEMM on host; slave_monitor offloaded to BF2 ARM via Comch tunnel

```bash
cd ~/experiments
bash scripts/exp_B_interference.sh
```

After completion:
```bash
python3 scripts/analyze/analyze_B.py
# Prints T_base, T_mixed, T_offload, interference%, recovery%
# Also prints LLC-miss and context-switch deltas per phase
```

**Data to collect**: paste the full printed summary here.

---

## 9. Experiment C — Scalability (simulated)

Simulates 4 / 16 / 64 / 256 worker nodes using mock_slave Docker containers,
all running on tianjin.  Measures master_monitor CPU and P99 control-message
latency as node count scales.

> Requires Docker: `docker --version` — install with `sudo apt-get install -y docker.io` if missing.

```bash
cd ~/experiments
bash scripts/exp_C_scale.sh
python3 scripts/analyze/analyze_C.py
```

**Data to collect**: paste the node-count table here.

---

## 10. Kubelet Reference Data

Measures kubelet's idle CPU and memory footprint for comparison with
slave_monitor in the thesis.

```bash
cd ~/experiments
bash scripts/exp_kubelet.sh
# Prints: kubelet vs slave_monitor — CPU% mean, RSS MB mean (30-second window)
```

> If kubelet is not installed, the script will print a note and measure only
> slave_monitor.  That is acceptable — record whatever is printed.

**Data to collect**: paste the comparison lines here.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `doca_devinfo_create_list failed` | DOCA not loaded | `sudo mlnx_bf_configure` or check `lspci \| grep -i mellanox` |
| Comch connect timeout | BF2 not listening | `ssh root@192.168.100.2 cat /tmp/bench_nic.log` |
| `libdoca_comch not found` | Wrong lib name for DOCA version | `make info` in `tunnel/host/` to check available libs |
| `libdoca_comm_channel not found` | Wrong lib on BF2 | `make info` in `tunnel/nic/` on BF2 |
| perf permission denied | Kernel lockdown | `sudo sysctl -w kernel.perf_event_paranoid=1` |
| `gemm_bench: CBLAS error` | OpenBLAS missing | `sudo apt-get install -y libopenblas-dev` |
| Docker permission denied | User not in docker group | `sudo usermod -aG docker $USER && newgrp docker` |
| scp to BF2 fails | SSH key not on BF2 | `ssh-copy-id root@192.168.100.2` or check `~/.ssh/known_hosts` |

---

## Reporting Back

After each experiment, paste the analysis script output here.
Include any compilation errors or runtime errors verbatim.
