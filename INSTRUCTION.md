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
"${CLUSTER_MASTER}" --grpc-port ${GRPC_PORT} \
  --db-connstr "${DB_CONNSTR}" \
  > ~/exp_data/D/cluster_master.log 2>&1 &
MASTER_PID=$!
sleep 3
echo "cluster_master started (PID=${MASTER_PID})"

# Start master_watchdog on tianjin BF2
ssh root@192.168.100.2 "
  pkill -f master_watchdog 2>/dev/null; sleep 1
  nohup ~/experiments/build/control-plane/watchdog/master_watchdog \
    --dev-pci=${NIC_PCI} \
    --master-grpc-addr=192.168.100.1:${GRPC_PORT} \
    --check-interval-ms=3000 \
    > /tmp/master_watchdog.log 2>&1 &
"
sleep 2
echo "master_watchdog started on tianjin BF2"

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
ssh root@192.168.100.2 "pkill -f master_watchdog 2>/dev/null"
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

# Start master_watchdog on tianjin BF2
ssh root@192.168.100.2 "
  pkill -f master_watchdog 2>/dev/null; sleep 1
  nohup ~/experiments/build/control-plane/watchdog/master_watchdog \
    --dev-pci=${NIC_PCI} \
    --master-grpc-addr=192.168.100.1:${GRPC_PORT} \
    > /tmp/master_watchdog.log 2>&1 &
"
sleep 2

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
ssh root@192.168.100.2 "pkill -f master_watchdog 2>/dev/null"
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

# Start master_watchdog on tianjin BF2
ssh root@192.168.100.2 "
  pkill -f master_watchdog 2>/dev/null; sleep 1
  nohup ~/experiments/build/control-plane/watchdog/master_watchdog \
    --dev-pci=${NIC_PCI} \
    --master-grpc-addr=192.168.100.1:${GRPC_PORT} \
    > /tmp/master_watchdog.log 2>&1 &
"
sleep 2

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
ssh root@192.168.100.2 "pkill -f master_watchdog 2>/dev/null"
ssh $(whoami)@172.28.4.77 "
  pkill -f metric_push 2>/dev/null
  ssh root@192.168.100.2 'pkill -f slave_agent 2>/dev/null'
"
echo "Cleanup done"
```

---

## 9. Setup: BF2 Docker Environment (Chapter 4 prerequisite)

```bash
source ~/experiments/scripts/config.sh

# Install Docker on all BF2s and pull Nginx arm64 image
bash ~/experiments/scripts/setup_bf2_docker.sh

# Install wrk on tianjin (load generator)
sudo apt-get install -y -qq wrk 2>/dev/null || \
  (git clone https://github.com/wg/wrk.git /tmp/wrk && cd /tmp/wrk && make -j$(nproc) && sudo cp wrk /usr/local/bin/)

# Install Python deps for orchestrator on tianjin
pip3 install -q psycopg2-binary
```

**Verify:**
```bash
ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'docker images nginx:alpine --format \"{{.Repository}}:{{.Tag}}\"'"
which wrk && echo "wrk OK"
python3 -c "import psycopg2; print('psycopg2 OK')"
```

---

## 10. Experiment G — Workload Feature Profiling (Chapter 4)

Measures Nginx and DGEMM characteristics on x86 host vs BF2 ARM.
Collects: CPU%, LLC miss rate, network I/O, context switches, req/s, GFLOPS.

### 10a. Nginx on x86 host

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/ch4_G

# Start Nginx on fujian host
ssh $(whoami)@172.28.4.77 "
  docker rm -f nginx-profile 2>/dev/null
  docker run -d --name nginx-profile --network=host nginx:alpine
  sleep 3
"

# Run wrk from tianjin (30s, 100 connections)
wrk -t4 -c100 -d30s "http://${FUJIAN_100G}/" 2>&1 | tee ~/exp_data/ch4_G/wrk_nginx_x86.txt

# Collect perf stat on fujian during a second wrk run
ssh $(whoami)@172.28.4.77 "
  sudo perf stat -e LLC-load-misses,LLC-loads,context-switches -a \
    -o /tmp/perf_nginx_x86.txt sleep 30 &
  PERF_PID=\$!
  sleep 30
  wait \${PERF_PID}
" &

wrk -t4 -c100 -d30s "http://${FUJIAN_100G}/" > /dev/null 2>&1
wait

scp $(whoami)@172.28.4.77:/tmp/perf_nginx_x86.txt ~/exp_data/ch4_G/
ssh $(whoami)@172.28.4.77 "docker rm -f nginx-profile"

echo "=== Nginx x86 Results ==="
cat ~/exp_data/ch4_G/wrk_nginx_x86.txt
echo ""
cat ~/exp_data/ch4_G/perf_nginx_x86.txt
```

### 10b. Nginx on BF2 ARM

```bash
# Start Nginx on fujian BF2
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    docker rm -f nginx-profile 2>/dev/null
    docker run -d --name nginx-profile --network=host nginx:alpine
    sleep 3
  '
"

# Run wrk against BF2 IP
wrk -t4 -c100 -d30s "http://${FUJIAN_BF2_FABRIC}/" 2>&1 | tee ~/exp_data/ch4_G/wrk_nginx_bf2.txt

ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'docker rm -f nginx-profile'"

echo "=== Nginx BF2 Results ==="
cat ~/exp_data/ch4_G/wrk_nginx_bf2.txt
```

### 10c. DGEMM on x86 host (with perf stat)

```bash
ssh $(whoami)@172.28.4.77 "
  sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_gemm_x86.txt \
    numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=16 \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=30 \
    > /tmp/gemm_x86.txt 2>&1
"
scp $(whoami)@172.28.4.77:/tmp/gemm_x86.txt ~/exp_data/ch4_G/
scp $(whoami)@172.28.4.77:/tmp/perf_gemm_x86.txt ~/exp_data/ch4_G/

echo "=== DGEMM x86 Results ==="
tail -5 ~/exp_data/ch4_G/gemm_x86.txt
cat ~/exp_data/ch4_G/perf_gemm_x86.txt
```

---

## 11. Experiment H — Co-location Interference (Chapter 4)

Measures DGEMM throughput + LLC miss rate under three scenarios.
Compare with Experiment B (Ch.2): interference source = monitoring agents (5.3%).
This experiment: interference source = Nginx (expected > 5.3%).

### 11a. Scenario 1 — DGEMM baseline (no co-location)

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/ch4_H

# Ensure no Nginx running
ssh $(whoami)@172.28.4.77 "docker rm -f nginx-exp 2>/dev/null" || true

ssh $(whoami)@172.28.4.77 "
  sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_s1.txt \
    numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=16 \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=60 \
    > /tmp/gemm_s1.txt 2>&1
"
scp $(whoami)@172.28.4.77:/tmp/gemm_s1.txt ~/exp_data/ch4_H/
scp $(whoami)@172.28.4.77:/tmp/perf_s1.txt ~/exp_data/ch4_H/

echo "=== Scenario 1: DGEMM baseline ==="
tail -3 ~/exp_data/ch4_H/gemm_s1.txt
grep -E "LLC|context" ~/exp_data/ch4_H/perf_s1.txt
```

### 11b. Scenario 2 — DGEMM + Nginx co-located (interference)

```bash
# Start Nginx on fujian host, pinned to same NUMA node as DGEMM
ssh $(whoami)@172.28.4.77 "
  docker rm -f nginx-exp 2>/dev/null
  docker run -d --name nginx-exp --network=host --cpuset-cpus=0-15 nginx:alpine
  sleep 3
"

# Start sustained wrk load in background (from tianjin)
wrk -t4 -c200 -d60s "http://${FUJIAN_100G}/" > ~/exp_data/ch4_H/wrk_s2.txt 2>&1 &
WRK_PID=$!

# Run DGEMM with perf stat (concurrent with wrk)
ssh $(whoami)@172.28.4.77 "
  sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_s2.txt \
    numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=16 \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=60 \
    > /tmp/gemm_s2.txt 2>&1
"

wait ${WRK_PID} 2>/dev/null
scp $(whoami)@172.28.4.77:/tmp/gemm_s2.txt ~/exp_data/ch4_H/
scp $(whoami)@172.28.4.77:/tmp/perf_s2.txt ~/exp_data/ch4_H/

echo "=== Scenario 2: DGEMM + Nginx co-located ==="
tail -3 ~/exp_data/ch4_H/gemm_s2.txt
grep -E "LLC|context" ~/exp_data/ch4_H/perf_s2.txt
echo "Nginx wrk:"
grep "Requests/sec" ~/exp_data/ch4_H/wrk_s2.txt

ssh $(whoami)@172.28.4.77 "docker rm -f nginx-exp"
```

### 11c. Scenario 3 — Nginx migrated to BF2 (after orchestration)

```bash
# Start Nginx on fujian BF2
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    docker rm -f nginx-exp 2>/dev/null
    docker run -d --name nginx-exp --network=host nginx:alpine
    sleep 3
  '
"

# Start sustained wrk against BF2 Nginx
wrk -t4 -c200 -d60s "http://${FUJIAN_BF2_FABRIC}/" > ~/exp_data/ch4_H/wrk_s3.txt 2>&1 &
WRK_PID=$!

# Run DGEMM on host (no Nginx interference)
ssh $(whoami)@172.28.4.77 "
  sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_s3.txt \
    numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=16 \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=60 \
    > /tmp/gemm_s3.txt 2>&1
"

wait ${WRK_PID} 2>/dev/null
scp $(whoami)@172.28.4.77:/tmp/gemm_s3.txt ~/exp_data/ch4_H/
scp $(whoami)@172.28.4.77:/tmp/perf_s3.txt ~/exp_data/ch4_H/

echo "=== Scenario 3: DGEMM alone, Nginx on BF2 ==="
tail -3 ~/exp_data/ch4_H/gemm_s3.txt
grep -E "LLC|context" ~/exp_data/ch4_H/perf_s3.txt
echo "BF2 Nginx wrk:"
grep "Requests/sec" ~/exp_data/ch4_H/wrk_s3.txt

ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'docker rm -f nginx-exp'"

echo ""
echo "=== Experiment H Summary ==="
for s in 1 2 3; do
  f=~/exp_data/ch4_H/gemm_s${s}.txt
  if [ -f "$f" ]; then
    avg=$(awk 'NR>5 {s+=$1; n++} END{if(n>0) printf "%.1f", s/n}' "$f")
    echo "  Scenario ${s}: avg GFLOPS = ${avg}"
  fi
done
echo "Compare with Exp B Scenario 2 (monitoring agents): 383.7 GFLOPS (-5.3%)"
```

---

## 12. Experiment I — BF2 Nginx Performance (Chapter 4)

Measures Nginx throughput on BF2 ARM at multiple concurrency levels,
as reference data for workload placement decisions.

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/ch4_I

CONCURRENCIES="10 50 100 200 400"

# --- Nginx on x86 host ---
echo "=== Nginx on x86 host ==="
ssh $(whoami)@172.28.4.77 "
  docker rm -f nginx-bench 2>/dev/null
  docker run -d --name nginx-bench --network=host nginx:alpine
  sleep 3
"

echo "conns,req_per_sec,avg_latency,transfer" > ~/exp_data/ch4_I/nginx_x86.csv
for c in ${CONCURRENCIES}; do
  result=$(wrk -t4 -c${c} -d30s "http://${FUJIAN_100G}/" 2>&1)
  rps=$(echo "$result" | grep "Requests/sec" | awk '{print $2}')
  lat=$(echo "$result" | grep "Latency" | awk '{print $2}')
  xfer=$(echo "$result" | grep "Transfer/sec" | awk '{print $2}')
  echo "  c=${c}: ${rps} req/s, ${lat} latency"
  echo "${c},${rps},${lat},${xfer}" >> ~/exp_data/ch4_I/nginx_x86.csv
done
ssh $(whoami)@172.28.4.77 "docker rm -f nginx-bench"

# --- Nginx on BF2 ARM ---
echo ""
echo "=== Nginx on BF2 ARM ==="
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    docker rm -f nginx-bench 2>/dev/null
    docker run -d --name nginx-bench --network=host nginx:alpine
    sleep 3
  '
"

echo "conns,req_per_sec,avg_latency,transfer" > ~/exp_data/ch4_I/nginx_bf2.csv
for c in ${CONCURRENCIES}; do
  result=$(wrk -t4 -c${c} -d30s "http://${FUJIAN_BF2_FABRIC}/" 2>&1)
  rps=$(echo "$result" | grep "Requests/sec" | awk '{print $2}')
  lat=$(echo "$result" | grep "Latency" | awk '{print $2}')
  xfer=$(echo "$result" | grep "Transfer/sec" | awk '{print $2}')
  echo "  c=${c}: ${rps} req/s, ${lat} latency"
  echo "${c},${rps},${lat},${xfer}" >> ~/exp_data/ch4_I/nginx_bf2.csv
done
ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'docker rm -f nginx-bench'"

echo ""
echo "=== Experiment I Results ==="
echo "--- x86 ---"
cat ~/exp_data/ch4_I/nginx_x86.csv
echo ""
echo "--- BF2 ---"
cat ~/exp_data/ch4_I/nginx_bf2.csv
```

---

## 13. Experiment J — Orchestration Strategy (Chapter 4)

Compares cluster performance with/without workload orchestration,
and measures blue-green migration overhead with VIP switch.

### 13a. Scenario 1 — No orchestration (Nginx + DGEMM on host)

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/ch4_J
VIP="${VIP_FUJIAN}"
HOST_IFACE="enp94s0f1np1"
BF2_IFACE="p1"

# Assign VIP to host, start Nginx pinned to NUMA 0
ssh $(whoami)@172.28.4.77 "
  sudo ip addr add ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
  docker rm -f nginx-exp 2>/dev/null
  docker run -d --name nginx-exp --network=host --cpuset-cpus=0-15 nginx:alpine
  sleep 3
"

# Start wrk against VIP (60s sustained load)
wrk -t4 -c200 -d60s "http://${VIP}/" > ~/exp_data/ch4_J/wrk_s1.txt 2>&1 &
WRK_PID=$!

# Run DGEMM
ssh $(whoami)@172.28.4.77 "
  sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_orch_s1.txt \
    numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=16 \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=60 \
    > /tmp/gemm_orch_s1.txt 2>&1
"

wait ${WRK_PID} 2>/dev/null
scp $(whoami)@172.28.4.77:/tmp/gemm_orch_s1.txt ~/exp_data/ch4_J/
scp $(whoami)@172.28.4.77:/tmp/perf_orch_s1.txt ~/exp_data/ch4_J/

echo "=== Scenario 1 Results ==="
tail -3 ~/exp_data/ch4_J/gemm_orch_s1.txt
grep "Requests/sec" ~/exp_data/ch4_J/wrk_s1.txt

# Cleanup
ssh $(whoami)@172.28.4.77 "
  docker rm -f nginx-exp
  sudo ip addr del ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
"
sleep 5
```

### 13b. Blue-green migration overhead (5 repetitions)

```bash
echo "=== Blue-green Migration Overhead ==="
echo "run,container_ms,health_ms,vip_ms,stop_ms,total_ms" > ~/exp_data/ch4_J/migration.csv

for run in $(seq 1 5); do
  # Setup: Nginx on host with VIP
  ssh $(whoami)@172.28.4.77 "
    docker run -d --name nginx-exp --network=host nginx:alpine
    sudo ip addr add ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
    sleep 2
  "

  T0=$(date +%s%3N)

  # Step 1: Start new container on BF2
  ssh $(whoami)@172.28.4.77 "
    ssh root@192.168.100.2 '
      docker rm -f nginx-new 2>/dev/null
      docker run -d --name nginx-new --network=host nginx:alpine
    '
  "
  T1=$(date +%s%3N)

  # Step 2: Health check
  until ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'curl -sf http://localhost/ >/dev/null'" 2>/dev/null; do
    sleep 0.2
  done
  T2=$(date +%s%3N)

  # Step 3: VIP switch
  ssh $(whoami)@172.28.4.77 "
    sudo ip addr del ${VIP}/24 dev ${HOST_IFACE} 2>/dev/null || true
    ssh root@192.168.100.2 '
      ip addr add ${VIP}/24 dev ${BF2_IFACE} 2>/dev/null || true
      arping -c 2 -A -I ${BF2_IFACE} ${VIP} &>/dev/null &
    '
  "
  T3=$(date +%s%3N)

  # Step 4: Stop old container
  ssh $(whoami)@172.28.4.77 "docker rm -f nginx-exp 2>/dev/null"
  T4=$(date +%s%3N)

  C_MS=$((T1 - T0)); H_MS=$((T2 - T1)); V_MS=$((T3 - T2)); S_MS=$((T4 - T3)); TOTAL=$((T4 - T0))
  echo "  Run ${run}: container=${C_MS}ms health=${H_MS}ms vip=${V_MS}ms stop=${S_MS}ms total=${TOTAL}ms"
  echo "${run},${C_MS},${H_MS},${V_MS},${S_MS},${TOTAL}" >> ~/exp_data/ch4_J/migration.csv

  # Reset: move back to host
  ssh $(whoami)@172.28.4.77 "
    ssh root@192.168.100.2 '
      ip addr del ${VIP}/24 dev ${BF2_IFACE} 2>/dev/null || true
      docker rm -f nginx-new 2>/dev/null
    '
  "
  sleep 2
done

echo ""
cat ~/exp_data/ch4_J/migration.csv
```

### 13c. Scenario 2 — Static orchestration (Nginx on BF2, DGEMM on host)

```bash
# Start Nginx on BF2 with VIP
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    docker rm -f nginx-exp 2>/dev/null
    docker run -d --name nginx-exp --network=host nginx:alpine
    ip addr add ${VIP}/24 dev ${BF2_IFACE} 2>/dev/null || true
    arping -c 2 -A -I ${BF2_IFACE} ${VIP} &>/dev/null &
    sleep 3
  '
"

# Start wrk against VIP (routed to BF2)
wrk -t4 -c200 -d60s "http://${VIP}/" > ~/exp_data/ch4_J/wrk_s2.txt 2>&1 &
WRK_PID=$!

# Run DGEMM on host (Nginx now on BF2, no interference)
ssh $(whoami)@172.28.4.77 "
  sudo perf stat -e LLC-load-misses,LLC-loads,context-switches \
    -o /tmp/perf_orch_s2.txt \
    numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=16 \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=60 \
    > /tmp/gemm_orch_s2.txt 2>&1
"

wait ${WRK_PID} 2>/dev/null
scp $(whoami)@172.28.4.77:/tmp/gemm_orch_s2.txt ~/exp_data/ch4_J/
scp $(whoami)@172.28.4.77:/tmp/perf_orch_s2.txt ~/exp_data/ch4_J/

echo "=== Scenario 2 Results ==="
tail -3 ~/exp_data/ch4_J/gemm_orch_s2.txt
grep "Requests/sec" ~/exp_data/ch4_J/wrk_s2.txt

# Cleanup
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    docker rm -f nginx-exp 2>/dev/null
    ip addr del ${VIP}/24 dev ${BF2_IFACE} 2>/dev/null || true
  '
"
```

### 13d. Summary

```bash
echo "============================================"
echo "  Experiment J: Orchestration Summary"
echo "============================================"
for s in 1 2; do
  f=~/exp_data/ch4_J/gemm_orch_s${s}.txt
  if [ -f "$f" ]; then
    avg=$(awk 'NR>5 {s+=$1; n++} END{if(n>0) printf "%.1f", s/n}' "$f")
    echo "  Scenario ${s}: DGEMM avg GFLOPS = ${avg}"
  fi
done
echo ""
echo "Scenario 1 Nginx (host):"
grep "Requests/sec" ~/exp_data/ch4_J/wrk_s1.txt 2>/dev/null
echo "Scenario 2 Nginx (BF2):"
grep "Requests/sec" ~/exp_data/ch4_J/wrk_s2.txt 2>/dev/null
echo ""
echo "Migration overhead:"
cat ~/exp_data/ch4_J/migration.csv
echo ""
echo "Baseline reference (Exp B): 405.1 GFLOPS"
```

---

## 14. Experiment A — Hierarchical Kernel-Bypass Tunnel Latency (Chapter 2)

> **Goal**: Validate the three-layer hierarchical hardware-bypass tunnel architecture (Thesis §2.2.2) by measuring the single-way latency of seven distinct communication paths. The key data point is Path 7 (NIC ARM↔ARM RDMA), which represents the thesis's core contribution to Chapter 2.

### Architecture

```
Worker node                                 Master node
+--------------+                           +--------------+
| host process |                           | host process |
+------+-------+                           +------^-------+
       | L1 PCIe Comch (~29μs)                    | L1 PCIe Comch (~29μs)
       v                                          |
+------+-------+   L2 RDMA (~3.44μs, 85.8Gbps)   +------+-------+
| BF-2 ARM     +---------------------------------+ BF-2 ARM     |
+--------------+                                 +--------------+
(L3 eSwitch    (data flows ASIC-to-ASIC inside NIC, <1μs)
 hardware offload inside each NIC)

End-to-end: ~62 μs  (vs existing OVS-TCP path ~105 μs, α_tunnel ≈ 1.7x)
```

Experiment A measures the following 7 paths plus a bandwidth verification:

| # | Path | Type | Tool | Expected |
|---|------|------|------|----------|
| 1 | Host ↔ local NIC ARM (PCIe Comch) | L1 kernel-bypass | `bench/latency_bench` | ~29 μs |
| 2 | Host ↔ remote host via eno1 (1G mgmt) | Kernel TCP | sockperf | ~200 μs |
| 3 | Host ↔ remote host via 100G VF | Kernel TCP | sockperf | ~80-100 μs |
| 4 | ARM ↔ remote ARM via OVS | Kernel TCP | sockperf | ~80 μs |
| 5 | Host ↔ remote host via OVS bridge | Kernel TCP + OVS | sockperf | ~105 μs |
| 6 | Host VF ↔ remote host VF (RDMA control) | Hardware RDMA | `ib_write_lat` | ~2 μs |
| 7 | ARM ↔ remote ARM (RDMA, thesis) | Hardware RDMA | `ib_write_lat`/`ib_read_lat` | ~3.44 / 5.04 μs |

Path E2E (synthesized): $T_{e2e} = T_{L1} + T_{L2} + T_{L1} \approx 62$ μs. No chained tool is needed; this value is computed from Paths 1 and 7.

### 14a. Pre-flight: RDMA environment setup on BF-2 ARMs

Both BF-2 ARMs need a Scalable Function (SF) with a RoCE GID configured, and the SF representor must be in `ovsbr1`. The setup script `bf2_rdma_setup.sh` automates this.

```bash
# First, copy the script to both BF-2 ARMs from the Mac side:
scp ~/Documents/final\ thesis/experiments/scripts/bf2_rdma_setup.sh \
    tianjin:/tmp/bf2_rdma_setup.sh
scp ~/Documents/final\ thesis/experiments/scripts/bf2_rdma_setup.sh \
    fujian:/tmp/bf2_rdma_setup.sh

# From the hosts, forward to BF-2 ARMs:
# tianjin
scp /tmp/bf2_rdma_setup.sh root@192.168.100.2:/root/
# fujian
scp /tmp/bf2_rdma_setup.sh root@192.168.100.2:/root/

# Run on tianjin BF-2 (role A)
ssh root@<tianjin-bf2-ip> "BF_ROLE=A bash /root/bf2_rdma_setup.sh"

# Run on fujian BF-2 (role B)
ssh root@<fujian-bf2-ip> "BF_ROLE=B bash /root/bf2_rdma_setup.sh"
```

**Verify** (on either BF-2 ARM):

```bash
ssh root@<bf2-ip> '
  show_gids
  echo "---"
  ovs-vsctl show
  echo "---"
  ovs-ofctl dump-flows ovsbr1
'
```

Expected: `show_gids` should list an IPv4 GID for the SF netdev (e.g., `192.168.56.12` on `enp3s0f1s0`). `ovs-vsctl show` should include the SF representor (e.g., `en3f1pf1sf0`) inside `ovsbr1`. `ovs-ofctl dump-flows ovsbr1` should have `actions=NORMAL`.

**Sanity test** (both ARMs, basic RDMA should work):

```bash
# On tianjin BF-2 ARM (server)
ib_write_bw -d <device> -F -s 4096 -n 1000 &
sleep 2

# On fujian BF-2 ARM (client)
ib_write_bw -d <device> -F -s 4096 -n 1000 192.168.56.12
```

Replace `<device>` with the RDMA device that corresponds to the SF on port 1. Check it with `ls /sys/class/infiniband/*/device/net/` — the device whose subdirectory contains the SF netdev is the right one. Expected average bandwidth ~100 Gbps; if it is less than 10 Gbps, something is wrong (usually OVS flow table or hw-offload).

### 14b. Setup common environment

```bash
source ~/thesis-experiments/scripts/config.sh
mkdir -p ~/exp_data/A
```

Take 3 measurement runs per path per message size and record the **median of the 3 runs** as the final number. Each run should use at least 1000 iterations (10000 preferred).

### 14c. Path 1: Host ↔ Local NIC ARM (PCIe Comch)

This path measures the L1 PCIe kernel-bypass tunnel using the existing latency benchmark (`bench/latency_bench/`). This is the same tool used for the original Experiment A (paths 1-2).

> **Note**: If you already have the original Experiment A data for Comch latency (~29 μs), you can **reuse that data** for Path 1 without re-running. The PCIe Comch latency is a hardware characteristic that does not change.

The tool consists of two binaries:
- `bench_host` (runs on x86 host, client side)
- `bench_nic` (runs on BF-2 ARM, server side)

```bash
# Step 1: Build (if not already compiled)
# On tianjin host (x86):
cd ~/thesis-experiments/bench/latency_bench
make host     # produces bench_host

# On tianjin BF-2 ARM (must compile ON the ARM):
cd ~/thesis-experiments/bench/latency_bench
make nic      # produces bench_nic

# Step 2: Find the PCI address of the BF-2
PCI_ADDR=$(lspci | grep -i mellanox | head -1 | awk '{print $1}')
echo "PCI address: ${PCI_ADDR}"

# Step 3: Run the benchmark
# On tianjin BF-2 ARM (server, start first):
ssh root@<tianjin-bf2-ip> "
  cd ~/thesis-experiments/bench/latency_bench
  pkill -f bench_nic 2>/dev/null; sleep 1
  nohup ./bench_nic --pci=${PCI_ADDR} > /tmp/bench_nic.log 2>&1 &
"
sleep 3

# On tianjin host (client):
cd ~/thesis-experiments/bench/latency_bench
./bench_host --pci=${PCI_ADDR} --mode=comch --size=all --iters=10000 \
  --output-dir=$HOME/exp_data/A/ \
  > ~/exp_data/A/path1_comch.log 2>&1

# This measures Comch ping-pong for sizes 64, 256, 1024 bytes × 10000 iterations each
```

**Record**: min, median (typical), avg, max latency for each message size. Expected typical ≈ 29 μs regardless of size.

**Cleanup**:
```bash
ssh root@<tianjin-bf2-ip> "pkill -f bench_nic 2>/dev/null"
```

### 14d. Install sockperf (if not installed)

```bash
# On all hosts and BF-2 ARMs
which sockperf || sudo apt-get install -y sockperf
# If apt has no sockperf package, build from source:
# git clone https://github.com/Mellanox/sockperf.git
# cd sockperf && ./autogen.sh && ./configure && make -j4 && sudo make install
```

### 14e. Path 2: Host ↔ remote host via eno1 (1G management)

```bash
# On tianjin host (server)
# Get eno1 IP first
TIANJIN_ENO1=$(ip -4 addr show eno1 | grep -oP '(?<=inet\s)\d+(\.\d+){3}')
echo "tianjin eno1 IP: ${TIANJIN_ENO1}"
sockperf server -i ${TIANJIN_ENO1} -p 12345 &
SERVER_PID=$!
sleep 1

# On fujian host (client)
for size in 64 256 1024; do
  sockperf ping-pong --tcp -i <tianjin-eno1-ip> -p 12345 \
    -t 30 -m ${size} --full-rtt \
    > ~/exp_data/A/path2_${size}.log 2>&1
  sleep 2
done

# On tianjin host, stop the server
kill $SERVER_PID 2>/dev/null
```

**Record**: sockperf prints latency percentiles (50th = median). Expected median ~200 μs.

### 14f. Path 3: Host ↔ remote host via 100G VF (kernel TCP)

First identify the host-side 100G VF interface:

```bash
# On tianjin host (and fujian)
ip -br addr | grep -v 'eno\|docker\|tmfifo'
# Look for an interface like enp94s0f1np1 with an IP in 192.168.56.x range
# Note the IP as <tianjin-100g-vf-ip>
```

```bash
# Server (tianjin)
sockperf server -i <tianjin-100g-vf-ip> -p 12346 &
sleep 1

# Client (fujian)
for size in 64 256 1024; do
  sockperf ping-pong --tcp -i <tianjin-100g-vf-ip> -p 12346 \
    -t 30 -m ${size} --full-rtt \
    > ~/exp_data/A/path3_${size}.log 2>&1
  sleep 2
done

# Kill server
pkill -f "sockperf server.*12346"
```

### 14g. Path 4: ARM ↔ remote ARM via OVS (kernel TCP)

```bash
# On tianjin BF-2 (server, SF IP 192.168.56.12)
ssh root@<tianjin-bf2-ip> "
  pkill -f sockperf 2>/dev/null
  sockperf server -i 192.168.56.12 -p 12347 > /tmp/sockperf_server.log 2>&1 &
"
sleep 2

# On fujian BF-2 (client)
for size in 64 256 1024; do
  ssh root@<fujian-bf2-ip> "
    sockperf ping-pong --tcp -i 192.168.56.12 -p 12347 \
      -t 30 -m ${size} --full-rtt
  " > ~/exp_data/A/path4_${size}.log 2>&1
  sleep 2
done

# Stop server
ssh root@<tianjin-bf2-ip> "pkill -f sockperf"
```

### 14h. Path 5: Host ↔ remote host via OVS bridge (existing control baseline)

This path is the current pre-RDMA baseline — host traffic goes through the OVS bridge on BF-2.

```bash
# Note: the "ovsbr1 IP" on BF-2 is the management IP for the OVS bridge
# If host traffic is routed through BF-2 ovsbr1, use that IP path
# Otherwise, use the same enp94s0f1np1 IP as Path 3 but via OVS bridge routing

# Server (tianjin) - listen on ovsbr1 reachable IP
sockperf server -i <tianjin-ovs-path-ip> -p 12348 &
sleep 1

# Client (fujian)
for size in 64 256 1024; do
  sockperf ping-pong --tcp -i <tianjin-ovs-path-ip> -p 12348 \
    -t 30 -m ${size} --full-rtt \
    > ~/exp_data/A/path5_${size}.log 2>&1
  sleep 2
done

pkill -f "sockperf server.*12348"
```

**Note**: If Paths 3 and 5 produce the same numbers on your topology (because the 100G VF already goes through OVS hw-offload), you may record them as equivalent and mention this in the paper.

### 14i. Path 6: Host VF RDMA (hardware lower bound, control)

This path provides the ASIC hardware latency lower bound, used as a reference for measuring ARM libibverbs overhead.

```bash
# Identify host RDMA device (usually mlx5_1 for port 1)
# On tianjin
ls /sys/class/infiniband/
# Typical output: mlx5_0, mlx5_1 — mlx5_1 is the port 1 PF, use this

# Server (tianjin host)
ib_write_lat -d mlx5_1 -F -s 64 -n 10000 &
sleep 1

# Client (fujian host)
ib_write_lat -d mlx5_1 -F -s 64 -n 10000 <tianjin-100g-vf-ip> \
  > ~/exp_data/A/path6_write_64.log 2>&1

# Also measure READ latency
pkill -f "ib_write_lat" 2>/dev/null
ib_read_lat -d mlx5_1 -F -s 64 -n 10000 &
sleep 1
ib_read_lat -d mlx5_1 -F -s 64 -n 10000 <tianjin-100g-vf-ip> \
  > ~/exp_data/A/path6_read_64.log 2>&1

# Repeat for sizes 256 and 1024 (optional, usually latency is size-independent for small messages)
```

**Expected**: `t_typical ≈ 2 μs` for WRITE, `~3 μs` for READ. If significantly higher (e.g., > 10 μs), the hardware path is not fast — check OVS hw-offload state.

### 14j. Path 7: NIC ARM RDMA (thesis main proposal)

This is the key measurement for Chapter 2's contribution. The prerequisite (Section 14a) must have completed successfully.

```bash
# Identify the correct RDMA device on each BF-2 ARM
# The SF netdev (e.g., enp3s0f1s0) lives under its associated mlx5_X
# Find it:
ssh root@<tianjin-bf2-ip> "
  for dev in /sys/class/infiniband/mlx5_*; do
    devname=\$(basename \$dev)
    if [ -d \$dev/device/net/enp3s0f1s0 ]; then
      echo \"tianjin: RDMA device for enp3s0f1s0 = \$devname\"
    fi
  done
"

ssh root@<fujian-bf2-ip> "
  for dev in /sys/class/infiniband/mlx5_*; do
    devname=\$(basename \$dev)
    if [ -d \$dev/device/net/enp3s0f1s2 ]; then
      echo \"fujian: RDMA device for enp3s0f1s2 = \$devname\"
    fi
  done
"

# Use the reported device names below (may differ on each BF-2)
TIANJIN_RDMA=mlx5_3    # example, replace with actual
FUJIAN_RDMA=mlx5_2     # example, replace with actual
```

Run the WRITE latency test:

```bash
# Server (tianjin BF-2)
ssh root@<tianjin-bf2-ip> "
  pkill -f ib_write_lat 2>/dev/null
  ib_write_lat -d ${TIANJIN_RDMA} -F -s 64 -n 10000 > /tmp/path7_write_server.log 2>&1 &
"
sleep 2

# Client (fujian BF-2)
ssh root@<fujian-bf2-ip> "
  ib_write_lat -d ${FUJIAN_RDMA} -F -s 64 -n 10000 192.168.56.12
" > ~/exp_data/A/path7_write_64.log 2>&1

# Cleanup
ssh root@<tianjin-bf2-ip> "pkill -f ib_write_lat 2>/dev/null"
```

Run the READ latency test (same pattern):

```bash
ssh root@<tianjin-bf2-ip> "
  pkill -f ib_read_lat 2>/dev/null
  ib_read_lat -d ${TIANJIN_RDMA} -F > /tmp/path7_read_server.log 2>&1 &
"
sleep 2
ssh root@<fujian-bf2-ip> "
  ib_read_lat -d ${FUJIAN_RDMA} -F 192.168.56.12
" > ~/exp_data/A/path7_read.log 2>&1
ssh root@<tianjin-bf2-ip> "pkill -f ib_read_lat 2>/dev/null"
```

**Expected**:
- WRITE `t_typical` ≈ 3.44 μs
- READ `t_typical` ≈ 5.04 μs

Note: p99 tail latencies are expected to be higher (~100+ μs) because of ARM OS scheduling jitter. Record but do not optimize for tail latency in this experiment.

### 14k. Bandwidth verification (Paths 6 and 7)

Beyond latency, confirm that hardware offload is active by measuring bandwidth. Both paths should reach near-100 Gbps.

```bash
# Path 6: Host VF bandwidth
# Server (tianjin)
ib_write_bw -d mlx5_1 -F -s 65536 -D 10 &
sleep 1
# Client (fujian)
ib_write_bw -d mlx5_1 -F -s 65536 -D 10 -x 3 <tianjin-100g-vf-ip> \
  > ~/exp_data/A/path6_bw.log 2>&1
pkill -f "ib_write_bw.*-D 10"
sleep 2

# Path 7: NIC ARM bandwidth
ssh root@<tianjin-bf2-ip> "
  pkill -f ib_write_bw 2>/dev/null
  ib_write_bw -d ${TIANJIN_RDMA} -F -s 65536 -D 10 > /tmp/path7_bw_server.log 2>&1 &
"
sleep 2
ssh root@<fujian-bf2-ip> "
  ib_write_bw -d ${FUJIAN_RDMA} -F -s 65536 -D 10 -x 3 192.168.56.12
" > ~/exp_data/A/path7_bw.log 2>&1
ssh root@<tianjin-bf2-ip> "pkill -f ib_write_bw 2>/dev/null"
```

**Expected**:
- Path 6: ~100 Gbps
- Path 7: ~85 Gbps (ARM libibverbs has slightly higher per-packet overhead)

### 14l. Troubleshooting Experiment A

| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| Path 1 bench_nic hangs | BF-2 Comch server not running | Check BF-2 log; ensure DOCA service running |
| Path 1 typical ≠ ~29 μs | Non-release DOCA build or different PCIe gen | Verify `ibv_devinfo` shows PCIe Gen4; rebuild bench_nic in release mode |
| Path 6/7 "Failed to modify QP to RTR" | SF mismatch or GID missing | Re-run `bf2_rdma_setup.sh`; check `show_gids` |
| Path 7 only gets < 1 MB/s bandwidth | OVS NORMAL not hw-offloaded | Check `tc -s filter show dev p1 ingress \| grep in_hw`; if missing, restart OVS after setting `hw-offload=true` |
| Path 7 IPv4 GID missing | SF netdev IP not assigned | `ip addr add 192.168.56.12/24 dev enp3s0f1s0` then `ip link set enp3s0f1s0 up` |
| p99 tail latency >> typical | ARM OS scheduler jitter | Normal; record `t_typical` (median) as the key number, not avg |

### 14m. Data consolidation

Create a simple consolidation script that extracts the key numbers from all logs:

```bash
cd ~/exp_data/A
cat > consolidate.sh << 'EOF'
#!/bin/bash
echo "Path  Size  t_min  t_typical  t_avg  t_max  p99  p99.9"

# Path 1 (comch)
for size in 64 256 1024; do
  if [ -f path1_${size}.log ]; then
    # Adjust grep patterns based on actual bench_nic output format
    grep -E "min|median|avg|max|p99" path1_${size}.log | \
      awk -v p="1" -v s="${size}" 'NR==1{printf "%s  %s  ", p, s} {printf "%s  ", $NF} END{print ""}'
  fi
done

# Paths 2-5 (sockperf): sockperf prints a percentiles table
for p in 2 3 4 5; do
  for size in 64 256 1024; do
    if [ -f path${p}_${size}.log ]; then
      t_typical=$(grep "percentile 50.000" path${p}_${size}.log | awk '{print $NF}')
      t_avg=$(grep "avg-latency" path${p}_${size}.log | awk '{print $NF}')
      t_p99=$(grep "percentile 99.000" path${p}_${size}.log | awk '{print $NF}')
      t_p999=$(grep "percentile 99.900" path${p}_${size}.log | awk '{print $NF}')
      t_min=$(grep "min-latency" path${p}_${size}.log | awk '{print $NF}')
      t_max=$(grep "max-latency" path${p}_${size}.log | awk '{print $NF}')
      echo "${p}  ${size}  ${t_min}  ${t_typical}  ${t_avg}  ${t_max}  ${t_p99}  ${t_p999}"
    fi
  done
done

# Paths 6, 7 (perftest): output columns are t_min, t_max, t_typical, t_avg, t_stdev, p99, p999
for p in 6 7; do
  for type in write read; do
    f=path${p}_${type}_64.log
    if [ -f $f ]; then
      line=$(awk '/^ [0-9]+ +[0-9]+ +[0-9]+\./ {print; exit}' $f)
      echo "${p}_${type}  64  ${line}"
    fi
  done
  # Bandwidth
  fbw=path${p}_bw.log
  if [ -f $fbw ]; then
    bw=$(awk '/^ +[0-9]+ +[0-9]+ +[0-9]+\./ {print $4}' $fbw | tail -1)
    echo "${p}  bw  -  -  -  -  -  ${bw} MB/s"
  fi
done
EOF
chmod +x consolidate.sh
./consolidate.sh | tee path_summary.txt
```

**Expected findings table** (rough):

```
Path         Path description                    t_typical   Notes
path1_64     Host↔local NIC (Comch)              ~29 μs     L1 baseline
path2_64     Host↔host TCP eno1                  ~200 μs    1G mgmt network
path3_64     Host↔host TCP 100G VF               ~80 μs     Kernel TCP over 100G
path4_64     ARM↔ARM TCP OVS                     ~80 μs     Kernel TCP on ARM
path5_64     Host↔host TCP OVS bridge            ~105 μs    Current baseline
path6_write  Host VF RDMA write                  ~2 μs      Hardware lower bound
path6_read   Host VF RDMA read                   ~3 μs      Hardware lower bound
path7_write  ARM RDMA write (thesis)             ~3.44 μs   L2 core measurement
path7_read   ARM RDMA read (thesis)              ~5.04 μs   L2 core measurement
path6_bw     Host VF write bandwidth             ~95 Gbps
path7_bw     ARM write bandwidth                 ~85 Gbps
```

**Derived end-to-end latency**: $T_{e2e} = 2 \times T_{Path1} + T_{Path7} \approx 2 \times 29 + 3.44 \approx 62\,\mu s$.

**Speedup over Path 5 (existing baseline)**: $\alpha = 105 / 62 \approx 1.69x$.

**Paste `path_summary.txt` back** for inclusion in Chapter 2's Experiment A analysis.

### 14n. Cleanup

```bash
# Stop any lingering servers on hosts
pkill -f sockperf 2>/dev/null
pkill -f "ib_write_lat" 2>/dev/null
pkill -f "ib_read_lat" 2>/dev/null
pkill -f "ib_write_bw" 2>/dev/null
pkill -f bench_nic 2>/dev/null

# Stop any lingering servers on BF-2 ARMs
for bf2 in <tianjin-bf2-ip> <fujian-bf2-ip>; do
  ssh root@${bf2} "
    pkill -f sockperf 2>/dev/null
    pkill -f 'ib_write_lat\|ib_read_lat\|ib_write_bw' 2>/dev/null
    pkill -f bench_nic 2>/dev/null
  "
done
```

---

## 15. Data Collection Summary

After all experiments complete (Ch.3 + Ch.4), run this to collect all results:

```bash
echo "============================================"
echo "  FULL EXPERIMENT RESULTS SUMMARY"
echo "============================================"

echo ""
echo "===== CHAPTER 2 ====="
echo ""
echo "=== Experiment A: Hierarchical Kernel-Bypass Tunnel Latency ==="
if [ -f ~/exp_data/A/path_summary.txt ]; then
  cat ~/exp_data/A/path_summary.txt
else
  echo "(path_summary.txt not found; listing raw logs)"
  ls ~/exp_data/A/ 2>/dev/null | head
fi

echo ""
echo "===== CHAPTER 3 ====="

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

echo ""
echo "===== CHAPTER 4 ====="

echo ""
echo "=== Experiment G: Workload Profiling ==="
for f in ~/exp_data/ch4_G/*.txt; do
  echo "--- $(basename $f) ---"
  cat "$f"
  echo ""
done

echo ""
echo "=== Experiment H: Co-location Interference ==="
for s in 1 2 3; do
  f=~/exp_data/ch4_H/gemm_s${s}.txt
  [ -f "$f" ] && avg=$(awk 'NR>5 {s+=$1; n++} END{if(n>0) printf "%.1f", s/n}' "$f") && echo "Scenario ${s}: ${avg} GFLOPS"
  [ -f ~/exp_data/ch4_H/perf_s${s}.txt ] && grep -E "LLC|context" ~/exp_data/ch4_H/perf_s${s}.txt
  echo ""
done

echo ""
echo "=== Experiment I: BF2 Nginx Performance ==="
echo "--- x86 ---"
cat ~/exp_data/ch4_I/nginx_x86.csv 2>/dev/null
echo "--- BF2 ---"
cat ~/exp_data/ch4_I/nginx_bf2.csv 2>/dev/null

echo ""
echo "=== Experiment J: Orchestration Strategy ==="
for s in 1 2; do
  f=~/exp_data/ch4_J/gemm_orch_s${s}.txt
  [ -f "$f" ] && avg=$(awk 'NR>5 {s+=$1; n++} END{if(n>0) printf "%.1f", s/n}' "$f") && echo "Scenario ${s}: ${avg} GFLOPS"
done
echo "--- Migration overhead ---"
cat ~/exp_data/ch4_J/migration.csv 2>/dev/null

echo ""
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
| `mock_slave: connection refused` | Ensure cluster_master is running: `pgrep -f cluster_master` |
| Docker not found on BF2 | Run `bash scripts/setup_bf2_docker.sh` |
| `wrk: command not found` | `sudo apt-get install wrk` or build from source |
| VIP unreachable after migration | Check `ip addr show` on both host and BF2; verify OVS bridge |
| Nginx container fails on BF2 | Check `docker logs nginx-exp` on BF2; verify arm64 image |

---

## Reporting Back

After all experiments, paste the output of Section 15 here verbatim.
Include any compilation errors, runtime errors, or unexpected behavior.
