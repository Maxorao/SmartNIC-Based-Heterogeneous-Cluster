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

## 4. Compile: Host-side (all hosts)

```bash
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    cd ~/experiments/tunnel/host          && make COMCH_HOST_DOCA_VER=31 &&
    cd ~/experiments/control-plane/slave  && make &&
    cd ~/experiments/control-plane/master && make &&
    cd ~/experiments/bench/gemm_bench     && make &&
    cd ~/experiments/bench/latency_bench  && make &&
    cd ~/experiments/bench/mock_slave     && make &&
    cd ~/experiments/bench/metric_push    && make &&
    echo 'HOST BUILD OK'
  " &
done
wait
```

**Verify:**
```bash
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    ls ~/experiments/bench/gemm_bench/gemm_bench \
       ~/experiments/bench/metric_push/metric_push \
       ~/experiments/control-plane/slave/slave_monitor \
       ~/experiments/control-plane/master/master_monitor \
       ~/experiments/bench/latency_bench/bench_host \
    >/dev/null 2>&1 && echo '${host}: OK' || echo '${host}: FAIL'
  "
done
```

---

## 5. Compile: BF2-side (all BF2s)

```bash
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    BF=192.168.100.2
    ssh root@\${BF} 'rm -rf ~/experiments && mkdir -p ~/experiments/bench'
    scp -r ~/experiments/tunnel          root@\${BF}:~/experiments/
    scp -r ~/experiments/control-plane   root@\${BF}:~/experiments/
    scp -r ~/experiments/bench/latency_bench root@\${BF}:~/experiments/bench/
    scp -r ~/experiments/common          root@\${BF}:~/experiments/
    ssh root@\${BF} '
      cd ~/experiments/tunnel/nic              && make COMCH_NIC_DOCA_VER=15 &&
      cd ~/experiments/control-plane/forwarder && make &&
      cd ~/experiments/bench/latency_bench     && make bench_nic &&
      echo BF2_BUILD_OK
    '
  " &
done
wait
```

**Verify:**
```bash
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    ssh root@192.168.100.2 'ls ~/experiments/control-plane/forwarder/forward_routine \
      ~/experiments/bench/latency_bench/bench_nic >/dev/null 2>&1 && echo BF2_OK || echo BF2_FAIL'
  "
done
```

---

## 5a. Install gRPC++ Dependencies (all hosts + BF2s)

Required for the Chapter 3 v2 gRPC-based architecture.

```bash
# On all x86 hosts:
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    sudo apt-get install -y -qq cmake libgrpc++-dev libprotobuf-dev \
      protobuf-compiler-grpc libpq-dev 2>/dev/null &&
    echo '${host}: gRPC deps OK'
  " &
done
wait

# On all BF2 ARMs:
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    ssh root@192.168.100.2 '
      apt-get update -qq &&
      apt-get install -y -qq cmake libgrpc++-dev libprotobuf-dev \
        protobuf-compiler-grpc 2>/dev/null &&
      echo BF2_GRPC_DEPS_OK
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

## 5b. Compile: gRPC Components (CMake build)

```bash
# On tianjin (master — builds cluster_master, mock_slave_grpc, metric_push_v2):
cd ~/experiments
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
echo "Master build done"

# On fujian + helong (workers — only need metric_push_v2):
for host in 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    cd ~/experiments &&
    cmake -B build -DCMAKE_BUILD_TYPE=Release &&
    cmake --build build --target metric_push_v2 -j\$(nproc) &&
    echo 'Worker build OK'
  " &
done
wait

# On all BF2 ARMs (builds slave_agent, master_watchdog):
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    ssh root@192.168.100.2 '
      cd ~/experiments &&
      cmake -B build -DCMAKE_BUILD_TYPE=Release -DCOMCH_NIC_DOCA_VER=15 &&
      cmake --build build --target slave_agent master_watchdog -j\$(nproc) &&
      echo BF2_BUILD_V2_OK
    '
  " &
done
wait
```

**Verify:**
```bash
source ~/experiments/scripts/config.sh
ls "${CLUSTER_MASTER}" "${MOCK_SLAVE_GRPC}" && echo "Master binaries OK"
ssh $(whoami)@172.28.4.77 "ls ~/experiments/build/bench/metric_push/metric_push_v2 && echo 'Worker binary OK'"
ssh root@192.168.100.2 "ls ~/experiments/build/control-plane/slave/slave_agent && echo 'BF2 binary OK'"
```

---

## 6. Experiment D — Fault Recovery (Chapter 3)

This experiment measures the system's fault detection and recovery time
under two failure scenarios.

### Architecture

```
tianjin (master):
  master_monitor :9000  ← monitors all node heartbeats

fujian (worker, fault injection target):
  slave_monitor → Comch → BF2 forward_routine → TCP → master_monitor
                                ↑
                           kill -9 here (Scenario 1)
         ↑
    kill + restart here (Scenario 2)
```

### 6a. Pre-flight: Install TimescaleDB (if not already installed)

```bash
# Check if DB is ready
psql -h localhost -U postgres -d cluster_metrics -c "SELECT 1;" 2>/dev/null && echo "DB OK — skip to 6b" && exit 0

# Install TimescaleDB (Ubuntu 22.04)
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

# Verify
psql -h localhost -U postgres -d cluster_metrics -c "SELECT default_version FROM pg_extension WHERE extname='timescaledb';"
```

### 6b. Setup

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/D

# Start master_monitor with DB persistence on tianjin
pkill -f master_monitor 2>/dev/null; sleep 1
"${MASTER_MONITOR}" --port=${MASTER_PORT} \
  --db-connstr="${DB_CONNSTR}" \
  > ~/exp_data/D/master_monitor.log 2>&1 &
MASTER_PID=$!
sleep 3
echo "master_monitor started (PID=${MASTER_PID})"

# Start forward_routine on fujian BF2
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    pkill -f forward_routine 2>/dev/null; sleep 1
    nohup ~/experiments/control-plane/forwarder/forward_routine \
      --pci=03:00.0 \
      --master-ip=192.168.56.10 \
      --master-port=9000 \
      > /tmp/forward_routine.log 2>&1 &
  '
"
sleep 3
echo "forward_routine started on fujian BF2"

# Start slave_monitor on fujian host (offload mode)
ssh $(whoami)@172.28.4.77 "
  pkill -f slave_monitor 2>/dev/null; sleep 1
  sudo nohup ~/experiments/control-plane/slave/slave_monitor \
    --mode=offload \
    --pci=0000:5e:00.0 \
    --interval=1000 \
    --node-id=fujian-worker \
    > ~/exp_data/D/slave_monitor.log 2>&1 &
"
sleep 5
echo "slave_monitor started on fujian"

# Also start on helong (healthy node for comparison)
ssh $(whoami)@172.28.4.85 "
  ssh root@192.168.100.2 '
    pkill -f forward_routine 2>/dev/null; sleep 1
    nohup ~/experiments/control-plane/forwarder/forward_routine \
      --pci=03:00.0 \
      --master-ip=192.168.56.10 \
      --master-port=9000 \
      > /tmp/forward_routine.log 2>&1 &
  '
  sleep 3
  pkill -f slave_monitor 2>/dev/null; sleep 1
  sudo nohup ~/experiments/control-plane/slave/slave_monitor \
    --mode=offload \
    --pci=0000:5e:00.0 \
    --interval=1000 \
    --node-id=helong-worker \
    > ~/exp_data/D/slave_monitor.log 2>&1 &
"
sleep 5
echo "helong also running"

# Verify: all nodes registered
echo "=== Registered nodes ==="
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_id, online, last_seen FROM node_status ORDER BY node_id;"
```

**Verification**: You should see fujian-worker and helong-worker with `online=true`.
Wait at least 10 seconds to confirm heartbeats are flowing before proceeding.

### 6c. Scenario 1 — forward_routine crash (5 repetitions)

```bash
echo "=== Scenario 1: forward_routine crash ==="
echo "run,t_fault_epoch_ms,detection_ms,recovery_ms" > ~/exp_data/D/scenario1.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  LOG_LINES_BEFORE=$(wc -l < ~/exp_data/D/master_monitor.log)
  T_FAULT=$(date +%s%3N)

  ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'kill -9 \$(pgrep -f forward_routine) 2>/dev/null'"
  echo "  t=${T_FAULT}: forward_routine killed on fujian BF2"

  T_DETECT=-1
  for attempt in $(seq 1 60); do
    sleep 1
    if tail -n +${LOG_LINES_BEFORE} ~/exp_data/D/master_monitor.log | grep -qi "fujian-worker.*offline\|fujian-worker.*timeout\|fujian-worker.*lost\|fujian-worker.*disconnect"; then
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

  ssh $(whoami)@172.28.4.77 "
    ssh root@192.168.100.2 '
      nohup ~/experiments/control-plane/forwarder/forward_routine \
        --pci=03:00.0 \
        --master-ip=192.168.56.10 \
        --master-port=9000 \
        > /tmp/forward_routine.log 2>&1 &
    '
  "

  T_RECOVER=-1
  for attempt in $(seq 1 60); do
    sleep 1
    if tail -n +${LOG_LINES_BEFORE} ~/exp_data/D/master_monitor.log | grep -qi "fujian-worker.*online\|fujian-worker.*registered\|fujian-worker.*reconnect"; then
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

### 6d. Scenario 2 — slave_monitor restart (5 repetitions)

```bash
echo "=== Scenario 2: slave_monitor restart ==="
echo "run,t_restart_epoch_ms,reregistration_ms" > ~/exp_data/D/scenario2.csv

for i in $(seq 1 5); do
  echo "--- Run ${i}/5 ---"
  LOG_LINES_BEFORE=$(wc -l < ~/exp_data/D/master_monitor.log)
  T_RESTART=$(date +%s%3N)

  ssh $(whoami)@172.28.4.77 "
    sudo pkill -f slave_monitor 2>/dev/null
    sleep 2
    sudo nohup ~/experiments/control-plane/slave/slave_monitor \
      --mode=offload \
      --pci=0000:5e:00.0 \
      --interval=1000 \
      --node-id=fujian-worker \
      > ~/exp_data/D/slave_monitor.log 2>&1 &
  "

  T_REREG=-1
  for attempt in $(seq 1 30); do
    sleep 1
    if tail -n +${LOG_LINES_BEFORE} ~/exp_data/D/master_monitor.log | grep -qi "fujian-worker.*register\|fujian-worker.*online"; then
      T_REREG=$(date +%s%3N)
      break
    fi
  done

  if [ "${T_REREG}" -eq -1 ]; then
    REREG_MS="timeout"
    echo "  WARNING: re-registration timeout"
  else
    REREG_MS=$(( T_REREG - T_RESTART ))
    echo "  Re-registration time: ${REREG_MS} ms"
  fi

  echo "${i},${T_RESTART},${REREG_MS}" >> ~/exp_data/D/scenario2.csv
  sleep 10
done

echo ""
echo "=== Scenario 2 Results ==="
cat ~/exp_data/D/scenario2.csv
```

### 6e. Cleanup

```bash
pkill -f master_monitor 2>/dev/null
ssh $(whoami)@172.28.4.77 "sudo pkill -f slave_monitor 2>/dev/null; ssh root@192.168.100.2 'pkill -f forward_routine 2>/dev/null'"
ssh $(whoami)@172.28.4.85 "sudo pkill -f slave_monitor 2>/dev/null; ssh root@192.168.100.2 'pkill -f forward_routine 2>/dev/null'"
echo "All processes stopped"
```

---

## 7. Experiment E — Database Performance (Chapter 3)

Measures TimescaleDB performance: write throughput, query latency, compression ratio.

### 7a. Populate the database with realistic data

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/E

pkill -f master_monitor 2>/dev/null; sleep 1
"${MASTER_MONITOR}" --port=${MASTER_PORT} \
  --db-connstr="${DB_CONNSTR}" \
  > ~/exp_data/E/master_monitor.log 2>&1 &
MASTER_PID=$!
sleep 3

for host_ip in 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host_ip} "
    ssh root@192.168.100.2 '
      pkill -f forward_routine 2>/dev/null; sleep 1
      nohup ~/experiments/control-plane/forwarder/forward_routine \
        --pci=03:00.0 --master-ip=192.168.56.10 --master-port=9000 \
        > /tmp/forward_routine.log 2>&1 &
    '
  " &
done
wait; sleep 3

ssh $(whoami)@172.28.4.77 "
  sudo pkill -f slave_monitor 2>/dev/null; sleep 1
  sudo nohup ~/experiments/control-plane/slave/slave_monitor \
    --mode=offload --pci=0000:5e:00.0 --interval=1000 \
    --node-id=fujian-worker > /tmp/slave_monitor.log 2>&1 &
"
ssh $(whoami)@172.28.4.85 "
  sudo pkill -f slave_monitor 2>/dev/null; sleep 1
  sudo nohup ~/experiments/control-plane/slave/slave_monitor \
    --mode=offload --pci=0000:5e:00.0 --interval=1000 \
    --node-id=helong-worker > /tmp/slave_monitor.log 2>&1 &
"

echo "System running. Collecting data for 5 minutes..."
sleep 300
echo "Data collection complete."
```

### 7b. Measure write throughput

```bash
echo "=== DB Write Throughput Test ==="

"${MOCK_SLAVE}" \
  --master-ip=192.168.56.10 --master-port=9000 \
  --nodes=64 --interval=1000 --duration=60 \
  > ~/exp_data/E/mock_64nodes.log 2>&1

ROW_COUNT=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT COUNT(*) FROM node_metrics WHERE time > NOW() - INTERVAL '2 minutes';")
echo "Rows inserted (64 nodes, 60s): ${ROW_COUNT}"
echo "Write rate: approximately $((ROW_COUNT / 60)) rows/s"

sleep 5

"${MOCK_SLAVE}" \
  --master-ip=192.168.56.10 --master-port=9000 \
  --nodes=256 --interval=1000 --duration=60 \
  > ~/exp_data/E/mock_256nodes.log 2>&1

ROW_COUNT=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT COUNT(*) FROM node_metrics WHERE time > NOW() - INTERVAL '2 minutes';")
echo "Rows inserted (256 nodes, 60s): ${ROW_COUNT}"
echo "Write rate: approximately $((ROW_COUNT / 60)) rows/s"
```

### 7c. Measure query latency

```bash
echo "=== DB Query Latency Test ==="

echo "--- Query: Latest node status ---"
for i in $(seq 1 10); do
  psql -h localhost -U postgres -d cluster_metrics -c \
    "\\timing on
     SELECT node_id, online, last_seen FROM node_status ORDER BY node_id;" \
    2>&1 | grep "Time:"
done | tee ~/exp_data/E/query_status.txt

echo "--- Query: 5-min CPU aggregation ---"
for i in $(seq 1 10); do
  psql -h localhost -U postgres -d cluster_metrics -c \
    "\\timing on
     SELECT node_id, AVG(cpu_pct) as avg_cpu, MAX(cpu_pct) as max_cpu
     FROM node_metrics
     WHERE time > NOW() - INTERVAL '5 minutes'
     GROUP BY node_id ORDER BY node_id;" \
    2>&1 | grep "Time:"
done | tee ~/exp_data/E/query_5min_agg.txt

echo "--- Query: 1-hour CPU aggregation ---"
for i in $(seq 1 10); do
  psql -h localhost -U postgres -d cluster_metrics -c \
    "\\timing on
     SELECT node_id, time_bucket('1 minute', time) AS bucket, AVG(cpu_pct) as avg_cpu
     FROM node_metrics
     WHERE time > NOW() - INTERVAL '1 hour'
     GROUP BY node_id, bucket ORDER BY node_id, bucket;" \
    2>&1 | grep "Time:"
done | tee ~/exp_data/E/query_1hour_agg.txt

echo "--- Table statistics ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT hypertable_name, num_chunks,
          pg_size_pretty(hypertable_size('node_metrics')) as total_size,
          (SELECT COUNT(*) FROM node_metrics) as total_rows
   FROM timescaledb_information.hypertables
   WHERE hypertable_name = 'node_metrics';" | tee ~/exp_data/E/table_stats.txt
```

### 7d. Measure compression ratio

```bash
echo "=== Compression Test ==="

BEFORE_SIZE=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT pg_size_pretty(hypertable_size('node_metrics'));")
BEFORE_ROWS=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT COUNT(*) FROM node_metrics;")
echo "Before compression: ${BEFORE_SIZE}, ${BEFORE_ROWS} rows"

psql -h localhost -U postgres -d cluster_metrics -c \
  "ALTER TABLE node_metrics SET (
    timescaledb.compress,
    timescaledb.compress_segmentby = 'node_id'
  );" 2>/dev/null

psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT compress_chunk(c.chunk_name)
   FROM timescaledb_information.chunks c
   WHERE c.hypertable_name = 'node_metrics'
     AND NOT c.is_compressed
   ORDER BY c.range_start;" 2>/dev/null

AFTER_SIZE=$(psql -h localhost -U postgres -d cluster_metrics -t -c \
  "SELECT pg_size_pretty(hypertable_size('node_metrics'));")
echo "After compression: ${AFTER_SIZE}"

psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT chunk_name, is_compressed,
          pg_size_pretty(before_compression_total_bytes) as before,
          pg_size_pretty(after_compression_total_bytes) as after
   FROM timescaledb_information.compressed_chunk_stats
   WHERE hypertable_name = 'node_metrics'
   ORDER BY chunk_name;" | tee ~/exp_data/E/compression_stats.txt
```

### 7e. Cleanup

```bash
pkill -f master_monitor 2>/dev/null
for host_ip in 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host_ip} "
    sudo pkill -f slave_monitor 2>/dev/null
    ssh root@192.168.100.2 'pkill -f forward_routine 2>/dev/null'
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

psql -h localhost -U postgres -d cluster_metrics -c \
  "DELETE FROM node_status; DROP TABLE IF EXISTS node_metrics CASCADE;" 2>/dev/null

pkill -f master_monitor 2>/dev/null; sleep 1
"${MASTER_MONITOR}" --port=${MASTER_PORT} \
  --db-connstr="${DB_CONNSTR}" \
  > ~/exp_data/F/master_monitor.log 2>&1 &
sleep 3

ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    pkill -f forward_routine 2>/dev/null; sleep 1
    nohup ~/experiments/control-plane/forwarder/forward_routine \
      --pci=03:00.0 --master-ip=192.168.56.10 --master-port=9000 \
      > /tmp/forward_routine.log 2>&1 &
  '
"
sleep 3

ssh $(whoami)@172.28.4.77 "
  sudo pkill -f slave_monitor 2>/dev/null; sleep 1
  sudo nohup ~/experiments/control-plane/slave/slave_monitor \
    --mode=offload --pci=0000:5e:00.0 --interval=1000 \
    --node-id=fujian-worker > /tmp/slave_monitor.log 2>&1 &
"
sleep 10

echo "=== Registration Evidence ==="
echo "--- master_monitor log (first 30 lines) ---"
head -30 ~/exp_data/F/master_monitor.log | tee ~/exp_data/F/registration_log.txt
echo ""
echo "--- node_status table ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_status;" | tee ~/exp_data/F/node_status_after_reg.txt
echo ""
echo "--- node_metrics (first 5 rows) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_metrics ORDER BY time LIMIT 5;" | tee ~/exp_data/F/first_metrics.txt
```

### 8b. Capture heartbeat and resource reporting

```bash
echo "=== Heartbeat & Resource Reporting Evidence ==="
echo "Let the system run for 30 seconds..."
sleep 30

echo "--- node_metrics count ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT node_id, COUNT(*) as report_count,
          MIN(time) as first_report, MAX(time) as last_report
   FROM node_metrics GROUP BY node_id;" | tee ~/exp_data/F/report_count.txt
echo ""
echo "--- Recent resource reports (last 5) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT time, node_id, cpu_pct, mem_used_kb, mem_total_kb, net_rx_bps, net_tx_bps
   FROM node_metrics ORDER BY time DESC LIMIT 5;" | tee ~/exp_data/F/recent_reports.txt
echo ""
echo "--- node_status (should show last_seen updated) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_status;" | tee ~/exp_data/F/node_status_heartbeat.txt
```

### 8c. Capture node re-registration with same ID

```bash
echo "=== Re-registration Evidence ==="
ssh $(whoami)@172.28.4.77 "sudo pkill -f slave_monitor"
echo "slave_monitor killed. Waiting 10s for timeout..."
sleep 10

echo "--- node_status after kill (should show offline) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_status;" | tee ~/exp_data/F/node_status_offline.txt

ssh $(whoami)@172.28.4.77 "
  sudo nohup ~/experiments/control-plane/slave/slave_monitor \
    --mode=offload --pci=0000:5e:00.0 --interval=1000 \
    --node-id=fujian-worker > /tmp/slave_monitor.log 2>&1 &
"
sleep 10

echo "--- node_status after re-registration (should show online, same ID) ---"
psql -h localhost -U postgres -d cluster_metrics -c \
  "SELECT * FROM node_status;" | tee ~/exp_data/F/node_status_reregistered.txt
echo ""
echo "--- master_monitor log showing re-registration ---"
grep -i "fujian-worker" ~/exp_data/F/master_monitor.log | tail -20 | tee ~/exp_data/F/reregistration_log.txt
```

### 8d. Cleanup

```bash
pkill -f master_monitor 2>/dev/null
ssh $(whoami)@172.28.4.77 "
  sudo pkill -f slave_monitor 2>/dev/null
  ssh root@192.168.100.2 'pkill -f forward_routine 2>/dev/null'
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
echo "--- Scenario 1: forward_routine crash ---"
cat ~/exp_data/D/scenario1.csv
echo ""
echo "--- Scenario 2: slave_monitor restart ---"
cat ~/exp_data/D/scenario2.csv

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
| Comch connect timeout | Check BF2 log: `ssh root@192.168.100.2 cat /tmp/forward_routine.log` |
| `psql: connection refused` | `sudo systemctl start postgresql` |
| `CREATE EXTENSION timescaledb fails` | Check `shared_preload_libraries = 'timescaledb'` in `postgresql.conf` |
| `master_monitor: db connect failed` | Verify DB_CONNSTR in config.sh |
| Log shows no timeout/offline detection | Check master_monitor heartbeat timeout config; grep log for actual patterns |
| `mock_slave: connection refused` | Ensure master_monitor is running on port 9000 |

---

## Reporting Back

After all experiments, paste the output of Section 9 here verbatim.
Include any compilation errors, runtime errors, or unexpected behavior.
