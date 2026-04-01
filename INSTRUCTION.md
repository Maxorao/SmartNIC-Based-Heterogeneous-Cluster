# Multi-Host Experiment Setup & Execution Guide

This document is for a Claude agent running on **tianjin** (172.28.4.75).
You will orchestrate experiments across three hosts and their BF2 SmartNICs.
Follow the steps in order.  Each section ends with a verification; do not
proceed until it passes.

---

## 0. Topology

```
              172.28.4.x management LAN (eno1, 1G)
    ┌─────────────────┬─────────────────┬─────────────────┐
    │  tianjin .75    │  fujian .77     │  helong .85     │
    │  MASTER         │  WORKER         │  WORKER         │
    │                 │                 │                 │
    │  enp94s0f1np1   │  enp94s0f1np1   │  enp94s0f0np0   │
    │  192.168.56.10  │  192.168.56.11  │  192.168.56.12  │
    │  ↕ PCIe+OVS     │  ↕ PCIe+OVS     │  ↕ PCIe+OVS     │
    │  BF2 p1: .56.2  │  BF2 p1: .56.3  │  BF2 p0: .56.1  │
    └────────┬────────┘────────┬────────┘────────┬────────┘
             └─────────────────┴─────────────────┘
                  192.168.56.0/24 (100G switch)
```

All devices (hosts via enp94s0f* AND BF2 ARMs via p0/p1) share the same
192.168.56.0/24 subnet.  OVS on each BF2 bridges the host representor
(pf*hpf) to the physical port (p0/p1), so all devices can reach each
other transparently.  **No relay or socat needed.**

### Key facts

- Each host: 2 sockets × 16 cores × 2 HT = 64 logical CPUs (Xeon Gold 5218 @ 2.30 GHz)
- Each BF2 ARM: 8 cores (Cortex-A72)
- Host DOCA 3.1 (`libdoca_comch`), BF2 DOCA 1.5 (`libdoca_comm_channel`)
- BF2 PCI from host: `0000:5e:00.0`; from ARM: `03:00.0`
- SSH to local BF2: `ssh root@192.168.100.2` (no password, via tmfifo)

### Communication paths

| # | Path | From → To | Use |
|---|------|-----------|-----|
| ① | Comch (PCIe) | Host ↔ local BF2 ARM | Tunnel: metric_push → BF2 |
| ② | TCP tmfifo | Host ↔ local BF2 ARM | SSH management, baseline |
| ③ | TCP eno1 LAN | Host ↔ Host (1G) | SSH between hosts, baseline |
| ④ | TCP 100G BF2↔BF2 | BF2 ARM ↔ BF2 ARM | SmartNIC fabric latency |
| ⑤ | TCP 100G host↔host | Host ↔ Host (via BF2 OVS) | E2E 100G fabric, control plane |

---

## 1. Network Setup (100G fabric)

### 1a. BF2 ARM port IPs

helong BF2 p0 already has 192.168.56.1.  Configure tianjin and fujian:

```bash
# tianjin BF2:
ssh root@192.168.100.2 "ip addr add 192.168.56.2/24 dev p1 2>/dev/null; ip link set p1 up"

# fujian BF2 (via fujian host):
ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'ip addr add 192.168.56.3/24 dev p1 2>/dev/null; ip link set p1 up'"
```

### 1b. Host-side 100G interface IPs

These interfaces connect to the same switch via the BF2 OVS bridge:

```bash
# tianjin:
sudo ip link set enp94s0f1np1 up
sudo ip addr add 192.168.56.10/24 dev enp94s0f1np1 2>/dev/null

# fujian:
ssh $(whoami)@172.28.4.77 "
  sudo ip link set enp94s0f1np1 up
  sudo ip addr add 192.168.56.11/24 dev enp94s0f1np1 2>/dev/null
"

# helong:
ssh $(whoami)@172.28.4.85 "
  sudo ip link set enp94s0f0np0 up
  sudo ip addr add 192.168.56.12/24 dev enp94s0f0np0 2>/dev/null
"
```

### 1c. Verify connectivity

```bash
# From tianjin host — can reach other hosts via 100G?
ping -c 2 -I enp94s0f1np1 192.168.56.11 && echo "fujian host OK"
ping -c 2 -I enp94s0f1np1 192.168.56.12 && echo "helong host OK"

# From tianjin BF2 — can reach tianjin host via OVS?
ssh root@192.168.100.2 "ping -c 2 192.168.56.10 && echo 'host via OVS OK'"

# From tianjin BF2 — can reach other BF2s via 100G?
ssh root@192.168.100.2 "
  ping -c 2 192.168.56.1 && echo 'helong BF2 OK'
  ping -c 2 192.168.56.3 && echo 'fujian BF2 OK'
"

# Critical: can fujian BF2 reach tianjin host?
ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'ping -c 2 192.168.56.10 && echo fujian-BF2-to-tianjin-host OK'"
```

All pings must succeed.  If any fail, check `ip addr show` on the relevant
interface and verify OVS bridge state with `ovs-vsctl show` on the BF2.

---

## 2. Clone Repository (all hosts)

```bash
# tianjin:
cd ~ && git clone git@github.com:Maxorao/SmartNIC-Based-Heterogeneous-Cluster.git experiments 2>/dev/null || (cd ~/experiments && git pull)

# fujian + helong:
for host in 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "cd ~ && git clone git@github.com:Maxorao/SmartNIC-Based-Heterogeneous-Cluster.git experiments 2>/dev/null || (cd ~/experiments && git pull)"
done
```

**Verify:**
```bash
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "ls ~/experiments/tunnel/ ~/experiments/scripts/ >/dev/null 2>&1 && echo '${host}: OK' || echo '${host}: FAIL'"
done
```

---

## 3. Install Dependencies (all hosts)

```bash
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    sudo apt-get update -qq &&
    sudo apt-get install -y -qq libopenblas-dev linux-tools-\$(uname -r) \
      sysstat stress-ng python3-pip sockperf 2>/dev/null &&
    pip3 install -q numpy pandas &&
    sudo sysctl -w kernel.perf_event_paranoid=1
  " &
done
wait
echo "All hosts done"
```

**Verify (on any host):**
```bash
dpkg -l libopenblas-dev | grep '^ii' && echo "OpenBLAS OK"
which sockperf && echo "sockperf OK"
```

---

## 4. Install Dependencies (all hosts + BF2s)

```bash
# Host dependencies (OpenBLAS for benchmarks + gRPC++ for cluster management)
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    sudo apt-get update -qq &&
    sudo apt-get install -y -qq libopenblas-dev linux-tools-\$(uname -r) \
      sysstat stress-ng sockperf python3-pip \
      cmake libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc libpq-dev \
      2>/dev/null &&
    pip3 install -q numpy pandas &&
    sudo sysctl -w kernel.perf_event_paranoid=1 &&
    echo '${host}: deps OK'
  " &
done
wait

# BF2 ARM dependencies (gRPC++ for slave_agent)
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    ssh root@192.168.100.2 '
      apt-get update -qq &&
      apt-get install -y -qq cmake libgrpc++-dev libprotobuf-dev \
        protobuf-compiler-grpc 2>/dev/null &&
      echo BF2_DEPS_OK
    '
  " &
done
wait
```

**Verify:**
```bash
pkg-config --modversion grpc++ && echo "Host gRPC OK"
ssh root@192.168.100.2 "pkg-config --modversion grpc++ && echo 'BF2 gRPC OK'"
```

---

## 5. Compile (all hosts + BF2s)

### 5a. Compile: Standalone benchmarks (Make)

```bash
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    cd ~/experiments/tunnel/host          && make COMCH_HOST_DOCA_VER=31 &&
    cd ~/experiments/bench/gemm_bench     && make &&
    cd ~/experiments/bench/latency_bench  && make &&
    echo 'BENCH BUILD OK'
  " &
done
wait

# BF2-side: tunnel + latency bench
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    BF=192.168.100.2
    scp -r ~/experiments/tunnel ~/experiments/common ~/experiments/bench/latency_bench root@\${BF}:~/experiments/
    ssh root@\${BF} '
      cd ~/experiments/tunnel/nic          && make COMCH_NIC_DOCA_VER=15 &&
      cd ~/experiments/bench/latency_bench && make bench_nic &&
      echo BF2_BENCH_OK
    '
  " &
done
wait
```

### 5b. Compile: Cluster management (CMake)

```bash
# tianjin (master): cluster_master + mock_slave + metric_push
cd ~/experiments
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
echo "Master build done"

# fujian + helong (workers): metric_push only
for host in 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    cd ~/experiments &&
    cmake -B build -DCMAKE_BUILD_TYPE=Release &&
    cmake --build build --target metric_push -j\$(nproc) &&
    echo 'Worker build OK'
  " &
done
wait

# All BF2 ARMs: slave_agent + master_watchdog
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    BF=192.168.100.2
    scp -r ~/experiments/proto ~/experiments/control-plane ~/experiments/CMakeLists.txt root@\${BF}:~/experiments/
    ssh root@\${BF} '
      cd ~/experiments &&
      cmake -B build -DCMAKE_BUILD_TYPE=Release -DCOMCH_NIC_DOCA_VER=15 &&
      cmake --build build --target slave_agent master_watchdog -j\$(nproc) &&
      echo BF2_CLUSTER_OK
    '
  " &
done
wait
```

**Verify:**
```bash
source ~/experiments/scripts/config.sh
ls "${CLUSTER_MASTER}" "${MOCK_SLAVE}" && echo "Master binaries OK"
ssh $(whoami)@172.28.4.77 "ls ~/experiments/build/bench/metric_push/metric_push && echo 'Worker OK'"
ssh root@192.168.100.2 "ls ~/experiments/build/control-plane/slave/slave_agent && echo 'BF2 OK'"
```

---

## 6. Experiment D — Fault Recovery (Chapter 3)

Measures fault detection and recovery time under three failure scenarios.

### Architecture

```
tianjin (master):
  cluster_master :50051 (gRPC)  ← receives heartbeats from all slave_agents
  master_watchdog (BF2)         ← monitors cluster_master via Comch + gRPC

fujian (worker, fault injection target):
  metric_push (host) → Comch → slave_agent (BF2) → gRPC → cluster_master
       ↑                              ↑
  kill here (Scenario 2)         kill here (Scenario 1)

  metric_push ··· fallback gRPC (direct) ··→ cluster_master  (Scenario 3)
```

### 6a. Pre-flight: Install TimescaleDB (if not already installed)

```bash
psql -h localhost -U postgres -d cluster_metrics -c "SELECT 1;" 2>/dev/null && echo "DB OK — skip to 6b"

# If not OK:
sudo apt-get install -y gnupg postgresql-common apt-transport-https lsb-release wget
sudo /usr/share/postgresql-common/pgdg/apt.postgresql.org.sh -y
echo "deb https://packagecloud.io/timescale/timescaledb/ubuntu/ $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/timescaledb.list
wget --quiet -O - https://packagecloud.io/timescale/timescaledb/gpgkey | sudo gpg --dearmor -o /etc/apt/trusted.gpg.d/timescaledb.gpg
sudo apt-get update
sudo apt-get install -y timescaledb-2-postgresql-14
sudo timescaledb-tune --quiet --yes
sudo systemctl restart postgresql
sudo -u postgres psql -c "CREATE DATABASE cluster_metrics;" 2>/dev/null
sudo -u postgres psql -d cluster_metrics -c "CREATE EXTENSION IF NOT EXISTS timescaledb;"
sudo -u postgres psql -c "ALTER USER postgres PASSWORD 'postgres';"
psql -h localhost -U postgres -d cluster_metrics -c "SELECT default_version FROM pg_extension WHERE extname='timescaledb';"
```

### 6b. Setup

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/D

# Start cluster_master on tianjin
pkill -f cluster_master 2>/dev/null; sleep 1
"${CLUSTER_MASTER}" --grpc-port=${GRPC_PORT} \
  --db-connstr="${DB_CONNSTR}" \
  > ~/exp_data/D/cluster_master.log 2>&1 &
MASTER_PID=$!
sleep 3
echo "cluster_master started (PID=${MASTER_PID})"

# Start slave_agent on fujian BF2
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    pkill -f slave_agent 2>/dev/null; sleep 1
    nohup ~/experiments/build/control-plane/slave/slave_agent \
      --node-uuid=fujian-bf2 \
      --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
      --dev-pci=${NIC_PCI} \
      --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
      > /tmp/slave_agent.log 2>&1 &
  '
"
sleep 3
echo "slave_agent started on fujian BF2"

# Start metric_push on fujian host
ssh $(whoami)@172.28.4.77 "
  pkill -f metric_push 2>/dev/null; sleep 1
  nohup ~/experiments/build/bench/metric_push/metric_push \
    --pci=${HOST_PCI} \
    --interval=1000 \
    --node-id=fujian-host \
    --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
    > ~/exp_data/D/metric_push.log 2>&1 &
"
sleep 3
echo "metric_push started on fujian host"

# Also start on helong (healthy node for comparison)
ssh $(whoami)@172.28.4.85 "
  ssh root@192.168.100.2 '
    pkill -f slave_agent 2>/dev/null; sleep 1
    nohup ~/experiments/build/control-plane/slave/slave_agent \
      --node-uuid=helong-bf2 \
      --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
      --dev-pci=${NIC_PCI} \
      --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
      > /tmp/slave_agent.log 2>&1 &
  '
  sleep 3
  pkill -f metric_push 2>/dev/null; sleep 1
  nohup ~/experiments/build/bench/metric_push/metric_push \
    --pci=${HOST_PCI} \
    --interval=1000 \
    --node-id=helong-host \
    --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
    > ~/exp_data/D/metric_push.log 2>&1 &
"
sleep 5
echo "helong also running"

# Verify: all nodes registered
echo "=== Registered nodes ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_uuid, state, host_status, bf2_status, last_seen FROM node_registry ORDER BY node_uuid;"
```

**Verification**: You should see fujian-bf2 and helong-bf2 with `state=online`.
Wait at least 10 seconds to confirm heartbeats are flowing before proceeding.

### 6c. Scenario 1 — slave_agent crash on BF2 (5 repetitions)

```bash
echo "=== Scenario 1: slave_agent crash on BF2 ==="
echo "run,t_fault_epoch_ms,detection_ms,recovery_ms" > ~/exp_data/D/scenario1.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  LOG_LINES_BEFORE=$(wc -l < ~/exp_data/D/cluster_master.log)
  T_FAULT=$(date +%s%3N)

  # Kill slave_agent on fujian BF2
  ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep -f slave_agent) 2>/dev/null'"
  echo "  t=${T_FAULT}: slave_agent killed on fujian BF2"

  # Wait for master to detect (poll log for suspect/offline)
  T_DETECT=-1
  for attempt in $(seq 1 60); do
    sleep 1
    if tail -n +${LOG_LINES_BEFORE} ~/exp_data/D/cluster_master.log | grep -qi "fujian.*suspect\|fujian.*offline\|fujian.*disconnect"; then
      T_DETECT=$(date +%s%3N)
      break
    fi
  done

  if [ "${T_DETECT}" -eq -1 ]; then
    DET_MS="timeout"
    echo "  WARNING: detection timeout (>60s)"
  else
    DET_MS=$(( T_DETECT - T_FAULT ))
    echo "  Detection time: ${DET_MS} ms"
  fi

  # Restart slave_agent on fujian BF2
  ssh $(whoami)@172.28.4.77 "
    ssh root@192.168.100.2 '
      nohup ~/experiments/build/control-plane/slave/slave_agent \
        --node-uuid=fujian-bf2 \
        --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
        --dev-pci=${NIC_PCI} \
        --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
        > /tmp/slave_agent.log 2>&1 &
    '
  "

  # Wait for recovery (node back online)
  T_RECOVER=-1
  for attempt in $(seq 1 60); do
    sleep 1
    if tail -n +${LOG_LINES_BEFORE} ~/exp_data/D/cluster_master.log | grep -qi "fujian.*online\|fujian.*register"; then
      T_RECOVER=$(date +%s%3N)
      break
    fi
  done

  if [ "${T_RECOVER}" -eq -1 ]; then
    REC_MS="timeout"
    echo "  WARNING: recovery timeout (>60s)"
  else
    REC_MS=$(( T_RECOVER - T_FAULT ))
    echo "  Total recovery time: ${REC_MS} ms"
  fi

  echo "${i},${T_FAULT},${DET_MS},${REC_MS}" >> ~/exp_data/D/scenario1.csv
  echo "  Stabilizing 15s..."
  sleep 15
done

echo ""
echo "=== Scenario 1 Results ==="
cat ~/exp_data/D/scenario1.csv
```

### 6d. Scenario 2 — metric_push graceful degradation (5 repetitions)

When Comch/BF2 fails, metric_push auto-switches to direct gRPC to cluster_master.

```bash
echo "=== Scenario 2: metric_push degradation ==="
echo "run,t_fault_epoch_ms,switch_ms" > ~/exp_data/D/scenario2.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  LOG_LINES_BEFORE=$(wc -l < ~/exp_data/D/cluster_master.log)
  T_FAULT=$(date +%s%3N)

  # Kill slave_agent (breaks Comch path) but keep metric_push running
  ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep -f slave_agent) 2>/dev/null'"
  echo "  t=${T_FAULT}: slave_agent killed, Comch path broken"

  # Wait for metric_push to switch to direct gRPC (should see DirectPush in master log)
  T_SWITCH=-1
  for attempt in $(seq 1 30); do
    sleep 1
    if tail -n +${LOG_LINES_BEFORE} ~/exp_data/D/cluster_master.log | grep -qi "DirectPush\|direct.*fujian\|fallback"; then
      T_SWITCH=$(date +%s%3N)
      break
    fi
  done

  if [ "${T_SWITCH}" -eq -1 ]; then
    SW_MS="timeout"
    echo "  WARNING: switch timeout (>30s)"
  else
    SW_MS=$(( T_SWITCH - T_FAULT ))
    echo "  Degradation switch time: ${SW_MS} ms"
  fi

  # Restart slave_agent to restore normal path
  ssh $(whoami)@172.28.4.77 "
    ssh root@192.168.100.2 '
      nohup ~/experiments/build/control-plane/slave/slave_agent \
        --node-uuid=fujian-bf2 \
        --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
        --dev-pci=${NIC_PCI} \
        --heartbeat-ms=${HEARTBEAT_INTERVAL_MS} \
        > /tmp/slave_agent.log 2>&1 &
    '
  "

  echo "${i},${T_FAULT},${SW_MS}" >> ~/exp_data/D/scenario2.csv
  sleep 15
done

echo ""
echo "=== Scenario 2 Results ==="
cat ~/exp_data/D/scenario2.csv
```

### 6e. Scenario 3 — cluster_master crash + watchdog restart (5 repetitions)

```bash
echo "=== Scenario 3: cluster_master crash ==="
echo "run,t_fault_epoch_ms,restart_ms,reconnect_ms" > ~/exp_data/D/scenario3.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  T_FAULT=$(date +%s%3N)

  # Kill cluster_master
  kill -9 ${MASTER_PID} 2>/dev/null
  echo "  t=${T_FAULT}: cluster_master killed"

  # Wait for master_watchdog to detect and restart (check if process comes back)
  T_RESTART=-1
  for attempt in $(seq 1 30); do
    sleep 1
    NEW_PID=$(pgrep -f cluster_master)
    if [ -n "${NEW_PID}" ] && [ "${NEW_PID}" != "${MASTER_PID}" ]; then
      T_RESTART=$(date +%s%3N)
      MASTER_PID=${NEW_PID}
      break
    fi
  done

  if [ "${T_RESTART}" -eq -1 ]; then
    # Watchdog didn't restart — manually restart
    "${CLUSTER_MASTER}" --grpc-port=${GRPC_PORT} --db-connstr="${DB_CONNSTR}" \
      > ~/exp_data/D/cluster_master.log 2>&1 &
    MASTER_PID=$!
    T_RESTART=$(date +%s%3N)
    echo "  WARNING: manual restart needed"
  fi
  RESTART_MS=$(( T_RESTART - T_FAULT ))
  echo "  Restart time: ${RESTART_MS} ms"

  # Wait for slave_agents to reconnect
  T_RECONNECT=-1
  for attempt in $(seq 1 30); do
    sleep 1
    ONLINE=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
      "SELECT COUNT(*) FROM node_registry WHERE state='online';" 2>/dev/null | tr -d ' ')
    if [ "${ONLINE}" -ge 2 ]; then
      T_RECONNECT=$(date +%s%3N)
      break
    fi
  done

  if [ "${T_RECONNECT}" -eq -1 ]; then
    RECON_MS="timeout"
  else
    RECON_MS=$(( T_RECONNECT - T_FAULT ))
  fi
  echo "  Full reconnect time: ${RECON_MS} ms"

  echo "${i},${T_FAULT},${RESTART_MS},${RECON_MS}" >> ~/exp_data/D/scenario3.csv
  sleep 10
done

echo ""
echo "=== Scenario 3 Results ==="
cat ~/exp_data/D/scenario3.csv
```

### 6f. Cleanup

```bash
pkill -f cluster_master 2>/dev/null
for host in 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    pkill -f metric_push 2>/dev/null
    ssh root@192.168.100.2 'pkill -f slave_agent 2>/dev/null'
  " &
done
wait
echo "All processes stopped"
```

---

## 7. Experiment E — Database Performance (Chapter 3)

Measures TimescaleDB performance: write throughput, query latency, compression ratio.

### 7a. Populate the database with realistic data

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/E

# Start cluster_master
pkill -f cluster_master 2>/dev/null; sleep 1
"${CLUSTER_MASTER}" --grpc-port=${GRPC_PORT} \
  --db-connstr="${DB_CONNSTR}" \
  > ~/exp_data/E/cluster_master.log 2>&1 &
sleep 3

# Start slave_agents on both worker BF2s
for host_ip in 172.28.4.77 172.28.4.85; do
  NODE_NAME=$([ "${host_ip}" = "172.28.4.77" ] && echo "fujian" || echo "helong")
  ssh $(whoami)@${host_ip} "
    ssh root@192.168.100.2 '
      pkill -f slave_agent 2>/dev/null; sleep 1
      nohup ~/experiments/build/control-plane/slave/slave_agent \
        --node-uuid=${NODE_NAME}-bf2 \
        --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
        --dev-pci=${NIC_PCI} \
        > /tmp/slave_agent.log 2>&1 &
    '
  " &
done
wait; sleep 3

# Start metric_push on both worker hosts
for host_ip in 172.28.4.77 172.28.4.85; do
  NODE_NAME=$([ "${host_ip}" = "172.28.4.77" ] && echo "fujian" || echo "helong")
  ssh $(whoami)@${host_ip} "
    pkill -f metric_push 2>/dev/null; sleep 1
    nohup ~/experiments/build/bench/metric_push/metric_push \
      --pci=${HOST_PCI} --interval=1000 --node-id=${NODE_NAME}-host \
      --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
      > /tmp/metric_push.log 2>&1 &
  " &
done
wait

echo "System running. Collecting data for 5 minutes..."
sleep 300
echo "Data collection complete."
```

### 7b. Measure write throughput

```bash
echo "=== DB Write Throughput Test ==="

# Use mock_slave to generate high write load (64 simulated nodes, 60s)
"${MOCK_SLAVE}" \
  --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
  --nodes=64 --interval=1000 --duration=60 \
  > ~/exp_data/E/mock_64nodes.log 2>&1

ROW_COUNT=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT COUNT(*) FROM host_metrics WHERE time > NOW() - INTERVAL '2 minutes';")
echo "Rows inserted (64 nodes, 60s): ${ROW_COUNT}"
echo "Write rate: approximately $((ROW_COUNT / 60)) rows/s"

sleep 5

"${MOCK_SLAVE}" \
  --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
  --nodes=256 --interval=1000 --duration=60 \
  > ~/exp_data/E/mock_256nodes.log 2>&1

ROW_COUNT=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT COUNT(*) FROM host_metrics WHERE time > NOW() - INTERVAL '2 minutes';")
echo "Rows inserted (256 nodes, 60s): ${ROW_COUNT}"
echo "Write rate: approximately $((ROW_COUNT / 60)) rows/s"
```

### 7c. Measure query latency

```bash
echo "=== DB Query Latency Test ==="

echo "--- Query: Node registry status ---"
for i in $(seq 1 10); do
  psql -h localhost -U postgres -d cluster_metrics -c \
    "\\timing on
     SELECT node_uuid, state, host_status, bf2_status, last_seen FROM node_registry ORDER BY node_uuid;" \
    2>&1 | grep "Time:"
done | tee ~/exp_data/E/query_status.txt

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

echo "--- Table statistics ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT hypertable_name, num_chunks,
          pg_size_pretty(hypertable_size(format('%I', hypertable_name)::regclass)) as total_size
   FROM timescaledb_information.hypertables
   WHERE hypertable_name IN ('host_metrics', 'bf2_metrics');" | tee ~/exp_data/E/table_stats.txt
```

### 7d. Measure compression ratio

```bash
echo "=== Compression Test ==="

BEFORE_SIZE=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT pg_size_pretty(hypertable_size('host_metrics'));")
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
  "SELECT pg_size_pretty(hypertable_size('host_metrics'));")
echo "After compression: ${AFTER_SIZE}"

psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT chunk_name, is_compressed,
          pg_size_pretty(before_compression_total_bytes) as before,
          pg_size_pretty(after_compression_total_bytes) as after
   FROM timescaledb_information.compressed_chunk_stats
   WHERE hypertable_name = 'host_metrics'
   ORDER BY chunk_name;" | tee ~/exp_data/E/compression_stats.txt
```

### 7e. Cleanup

```bash
pkill -f cluster_master 2>/dev/null
for host_ip in 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host_ip} "
    pkill -f metric_push 2>/dev/null
    ssh root@192.168.100.2 'pkill -f slave_agent 2>/dev/null'
  " &
done
wait
echo "All processes stopped"
```

---

## 8. Experiment F — Functional Correctness Verification (Chapter 3)

Captures log evidence for the thesis demonstrating node lifecycle management.

### 8a. Clean start — capture registration flow

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/F

# Reset DB tables
psql -h localhost -U postgres -d cluster_metrics -c \
  "DROP TABLE IF EXISTS node_registry, host_metrics, bf2_metrics, cluster_events CASCADE;" 2>/dev/null

# Start cluster_master (fresh DB schema)
pkill -f cluster_master 2>/dev/null; sleep 1
"${CLUSTER_MASTER}" --grpc-port=${GRPC_PORT} \
  --db-connstr="${DB_CONNSTR}" \
  > ~/exp_data/F/cluster_master.log 2>&1 &
sleep 3

# Start slave_agent on fujian BF2
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    pkill -f slave_agent 2>/dev/null; sleep 1
    nohup ~/experiments/build/control-plane/slave/slave_agent \
      --node-uuid=fujian-bf2 \
      --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
      --dev-pci=${NIC_PCI} \
      > /tmp/slave_agent.log 2>&1 &
  '
"
sleep 3

# Start metric_push on fujian host
ssh $(whoami)@172.28.4.77 "
  pkill -f metric_push 2>/dev/null; sleep 1
  nohup ~/experiments/build/bench/metric_push/metric_push \
    --pci=${HOST_PCI} --interval=1000 --node-id=fujian-host \
    --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
    > /tmp/metric_push.log 2>&1 &
"
sleep 10

# Capture evidence
echo "=== Registration Evidence ==="
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
```

### 8b. Capture heartbeat and resource reporting

```bash
echo "=== Heartbeat & Resource Reporting Evidence ==="
echo "Let the system run for 30 seconds..."
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
```

### 8c. Capture node re-registration with same UUID

```bash
echo "=== Re-registration Evidence ==="

# Kill slave_agent on fujian BF2
ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'pkill -f slave_agent'"
echo "slave_agent killed. Waiting 20s for suspect/offline transition..."
sleep 20

echo "--- node_registry after kill (should show suspect or offline) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_registry;" | tee ~/exp_data/F/node_registry_offline.txt

# Restart with SAME UUID
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    nohup ~/experiments/build/control-plane/slave/slave_agent \
      --node-uuid=fujian-bf2 \
      --master-addr=${TIANJIN_100G}:${GRPC_PORT} \
      --dev-pci=${NIC_PCI} \
      > /tmp/slave_agent.log 2>&1 &
  '
"
sleep 10

echo "--- node_registry after re-registration (should show online, same UUID) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_registry;" | tee ~/exp_data/F/node_registry_reregistered.txt
echo ""
echo "--- cluster_events showing lifecycle ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM cluster_events ORDER BY time;" | tee ~/exp_data/F/lifecycle_events.txt
echo ""
echo "--- cluster_master log showing re-registration ---"
grep -i "fujian" ~/exp_data/F/cluster_master.log | tail -20 | tee ~/exp_data/F/reregistration_log.txt
```

### 8d. Cleanup

```bash
pkill -f cluster_master 2>/dev/null
ssh $(whoami)@172.28.4.77 "
  pkill -f metric_push 2>/dev/null
  ssh root@192.168.100.2 'pkill -f slave_agent 2>/dev/null'
"
echo "Cleanup done"
```

---

## 9. Data Collection Summary

After all experiments complete, run this to collect all results:

```bash
echo "============================================"
echo "  CHAPTER 3 EXPERIMENT RESULTS SUMMARY"
echo "============================================"

echo ""
echo "=== Experiment D: Fault Recovery ==="
echo "--- Scenario 1: slave_agent crash ---"
cat ~/exp_data/D/scenario1.csv
echo ""
echo "--- Scenario 2: metric_push degradation ---"
cat ~/exp_data/D/scenario2.csv
echo ""
echo "--- Scenario 3: cluster_master crash ---"
cat ~/exp_data/D/scenario3.csv

echo ""
echo "=== Experiment E: Database Performance ==="
echo "--- Write throughput ---"
grep -A2 "Write rate" ~/exp_data/E/mock_*.log 2>/dev/null
echo "--- Query latency (status) ---"
cat ~/exp_data/E/query_status.txt 2>/dev/null
echo "--- Query latency (5min agg) ---"
cat ~/exp_data/E/query_5min_agg.txt 2>/dev/null
echo "--- Query latency (1hour agg) ---"
cat ~/exp_data/E/query_1hour_agg.txt 2>/dev/null
echo "--- Table stats ---"
cat ~/exp_data/E/table_stats.txt 2>/dev/null
echo "--- Compression ---"
cat ~/exp_data/E/compression_stats.txt 2>/dev/null

echo ""
echo "=== Experiment F: Functional Correctness ==="
for f in ~/exp_data/F/*.txt; do
  echo "--- $(basename $f) ---"
  cat "$f"
  echo ""
done

echo "============================================"
echo "  END OF RESULTS"
echo "============================================"
```

**Copy and paste the entire output above and send it back.**

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `enp94s0f1np1: No such device` | Check `ip link show`; use `ls /sys/class/net/ \| grep enp` |
| Ping 192.168.56.x fails | Check OVS bridge: `ssh root@192.168.100.2 "ovs-vsctl show"` |
| `doca_devinfo_create_list failed` | `sudo mlnx_bf_configure` or `echo 1 > /sys/bus/pci/rescan` |
| Comch connect timeout | Check BF2 log: `ssh root@192.168.100.2 cat /tmp/slave_agent.log` |
| `psql: connection refused` | `sudo systemctl start postgresql` |
| `CREATE EXTENSION timescaledb fails` | Check `shared_preload_libraries = 'timescaledb'` in `postgresql.conf` |
| `cluster_master: db connect failed` | Verify DB_CONNSTR in config.sh |
| `gRPC: connection refused` | Ensure cluster_master is running on port ${GRPC_PORT} |
| Log shows no state transitions | Check heartbeat_interval and suspect_threshold in node_state.h |
| `mock_slave: connection refused` | Ensure cluster_master is running: `pgrep -f cluster_master` |

---

## Reporting Back

After all experiments, paste the output of Section 9 here verbatim.
Include any compilation errors, runtime errors, or unexpected behavior.
