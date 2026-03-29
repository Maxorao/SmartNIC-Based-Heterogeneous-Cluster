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

## 6. Experiment A — Communication Path Latency

Measures RTT/2 (one-way latency) across five communication paths.
Sizes: 64B, 256B, 1024B.  10000 iterations each.

### Path ① Comch host↔BF2 (PCIe kernel-bypass)

```bash
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 'pkill bench_nic 2>/dev/null; sleep 1
    nohup ~/experiments/bench/latency_bench/bench_nic --pci=03:00.0 --mode=comch \
    > /tmp/bench_nic.log 2>&1 &'
  sleep 3
  sudo ~/experiments/bench/latency_bench/bench_host \
    --pci=0000:5e:00.0 --mode=comch --iters=10000 \
    --output-dir=~/exp_data/A 2>&1
  ssh root@192.168.100.2 'pkill bench_nic'
"
```

### Path ② TCP tmfifo host↔BF2

```bash
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 'pkill bench_nic 2>/dev/null; sleep 1
    nohup ~/experiments/bench/latency_bench/bench_nic --mode=tcp \
    > /tmp/bench_nic_tcp.log 2>&1 &'
  sleep 2
  ~/experiments/bench/latency_bench/bench_host \
    --mode=tcp --nic-ip=192.168.100.2 --iters=10000 \
    --output-dir=~/exp_data/A 2>&1
  ssh root@192.168.100.2 'pkill bench_nic'
"
```

### Path ③ TCP LAN host→host (eno1, 1G management)

```bash
# Server on tianjin:
sockperf sr --tcp --port 11111 &
SPID=$!; sleep 1

# Client from fujian:
ssh $(whoami)@172.28.4.77 "
  mkdir -p ~/exp_data/A
  for size in 64 256 1024; do
    echo \"--- TCP LAN (eno1) \${size}B ---\"
    sockperf pp --tcp -i 172.28.4.75 --port 11111 \
      -m \${size} -t 10 --full-rtt 2>&1 | tee ~/exp_data/A/tcp_lan_\${size}B.txt
  done
"
kill $SPID 2>/dev/null
```

### Path ④ TCP 100G BF2→BF2 (ARM-to-ARM via fabric)

```bash
# Server on tianjin BF2:
ssh root@192.168.100.2 "
  pkill sockperf 2>/dev/null
  which sockperf || apt-get install -y sockperf 2>/dev/null
  nohup sockperf sr --tcp --port 11111 > /tmp/sockperf.log 2>&1 &
"
sleep 2

# Client from fujian BF2:
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    which sockperf || apt-get install -y sockperf 2>/dev/null
    for size in 64 256 1024; do
      echo \"--- BF2-BF2 100G \${size}B ---\"
      sockperf pp --tcp -i 192.168.56.2 --port 11111 \
        -m \${size} -t 10 --full-rtt
    done
  '
" | tee ~/exp_data/A/tcp_bf2_100g.txt

ssh root@192.168.100.2 "pkill sockperf"
```

### Path ⑤ TCP 100G host→host (E2E via BF2 OVS, no relay)

This is the actual end-to-end path for both offloaded and non-offloaded
control-plane traffic.  Packets go: host → PCIe → BF2 OVS → physical
port → 100G switch → physical port → BF2 OVS → PCIe → host.

```bash
# Server on tianjin (bind to 100G interface):
sockperf sr --tcp --port 11111 --bind 192.168.56.10 &
SPID=$!; sleep 1

# Client from helong (via 100G):
ssh $(whoami)@172.28.4.85 "
  mkdir -p ~/exp_data/A
  for size in 64 256 1024; do
    echo \"--- 100G host-host (OVS) \${size}B ---\"
    sockperf pp --tcp -i 192.168.56.10 --port 11111 \
      -m \${size} -t 10 --full-rtt 2>&1 | tee ~/exp_data/A/tcp_100g_host_\${size}B.txt
  done
"
kill $SPID 2>/dev/null

# Also test from fujian:
sockperf sr --tcp --port 11111 --bind 192.168.56.10 &
SPID=$!; sleep 1
ssh $(whoami)@172.28.4.77 "
  mkdir -p ~/exp_data/A
  for size in 64 256 1024; do
    echo \"--- 100G host-host fujian (OVS) \${size}B ---\"
    sockperf pp --tcp -i 192.168.56.10 --port 11111 \
      -m \${size} -t 10 --full-rtt 2>&1 | tee ~/exp_data/A/tcp_100g_host_fujian_\${size}B.txt
  done
"
kill $SPID 2>/dev/null
```

### Collect results

Gather all output files and paste the summary here.

```bash
echo "=== Experiment A Summary ==="
echo "Path ①②: see bench_host CSV files"
echo "Path ③④⑤: see sockperf output below"
echo ""
for f in ~/exp_data/A/tcp_*.txt; do
  [ -f "$f" ] && echo "--- $(basename $f) ---" && grep -E "(avg-latency|percentile)" "$f"
done
```

**Data to collect**: paste the full summary here.

---

## 7. Experiment B — Interference Elimination

Core experiment.  Measures GEMM throughput on fujian under three
control-plane configurations.  All control-plane traffic flows over
the 100G fabric (192.168.56.0/24).

### Architecture

```
Scenario 1 (baseline):
  fujian host: GEMM alone (16 threads, NUMA node 0)

Scenario 2 (no offload — control plane on host):
  fujian host: GEMM + 8× slave_monitor --mode=direct → TCP 192.168.56.10:9000
  (each slave_monitor: read /proc + build msg + TCP send through enp94s0f1np1)
  Host CPU handles all control-plane work.

Scenario 3 (offloaded — control plane on BF2):
  fujian host: GEMM + 1× metric_push → Comch → BF2
  fujian BF2:  forward_routine → TCP 192.168.56.10:9000
  (metric_push: read /proc + Comch DMA, 1.25ms interval = 800 reports/s)
  Host CPU only does minimal /proc reads + PCIe DMA.  BF2 ARM handles TCP.
```

**Why N=8 for scenario 2 but N=1 for scenario 3?**  DOCA Comch supports only
one client connection per service name.  To keep total report rate equal
(800 reports/s), scenario 3 runs one metric_push at 1.25ms interval instead
of 8 at 10ms.  This is a realistic offload design — a single multiplexed
Comch channel replaces multiple TCP connections.

### Parameters

- **GEMM**: OPENBLAS_NUM_THREADS=16, numactl --cpunodebind=0 (100% CPU, socket 0)
- **Scenario 2**: 8 × slave_monitor at 10ms interval = 800 reports/s total
- **Scenario 3**: 1 × metric_push at 1ms interval = 1000 reports/s (≥ scenario 2 rate)
- **Duration**: 60 seconds per scenario

### Setup

```bash
source ~/experiments/scripts/config.sh
mkdir -p ~/exp_data/B

# Start master_monitor on tianjin host (listens on all interfaces)
"${MASTER_MONITOR}" --port="${MASTER_PORT}" \
  > ~/exp_data/B/master.log 2>&1 &
MASTER_PID=$!
sleep 2
echo "master_monitor started (PID=${MASTER_PID})"

# Start forward_routine on fujian BF2 (for scenario 3)
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
```

### Scenario 1: Baseline (GEMM only)

```bash
ssh $(whoami)@172.28.4.77 "
  source ~/experiments/scripts/config.sh
  mkdir -p ~/exp_data/B
  echo '=== Scenario 1: GEMM baseline ==='

  numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=16 \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=60 \
    > ~/exp_data/B/scenario1_gflops.txt 2>/dev/null &
  GPID=\$!; sleep 1

  sudo perf stat -p \${GPID} \
    -e LLC-load-misses,LLC-loads,context-switches,instructions \
    -I 1000 -o ~/exp_data/B/scenario1_perf.txt \
    sleep 60 &

  wait \${GPID}; sleep 2
  echo 'Scenario 1 done'
  tail -5 ~/exp_data/B/scenario1_gflops.txt
"
```

### Scenario 2: No offload (slave_monitor on host, TCP to master)

Each slave_monitor sends TCP directly through the host's 100G NIC
(enp94s0f1np1) to master_monitor on tianjin.  The host CPU handles
all protocol building and network I/O.

```bash
ssh $(whoami)@172.28.4.77 "
  source ~/experiments/scripts/config.sh
  echo '=== Scenario 2: GEMM + 8x slave_monitor (no offload) ==='

  # Start 8 slave_monitors in direct TCP mode on NUMA node 0
  PIDS=()
  for i in \$(seq 1 8); do
    numactl --cpunodebind=0 \
      ~/experiments/control-plane/slave/slave_monitor \
        --mode=direct \
        --master-ip=192.168.56.10 \
        --master-port=9000 \
        --interval=10 \
        --node-id=\"slave-\${i}-\$(hostname)\" \
      > ~/exp_data/B/scenario2_slave_\${i}.log 2>&1 &
    PIDS+=(\$!)
  done
  sleep 3

  numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=16 \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=60 \
    > ~/exp_data/B/scenario2_gflops.txt 2>/dev/null &
  GPID=\$!; sleep 1

  sudo perf stat -p \${GPID} \
    -e LLC-load-misses,LLC-loads,context-switches,instructions \
    -I 1000 -o ~/exp_data/B/scenario2_perf.txt \
    sleep 60 &

  wait \${GPID}

  for pid in \${PIDS[@]}; do kill \${pid} 2>/dev/null; done
  sleep 5
  echo 'Scenario 2 done'
  tail -5 ~/exp_data/B/scenario2_gflops.txt
"
```

### Scenario 3: Offloaded (metric_push on host, forward_routine on BF2)

Only a single lightweight metric_push runs on the host.  It reads /proc
and Comch-sends raw data to the BF2 every 1ms.  The BF2 forward_routine
handles TCP to master.

```bash
ssh $(whoami)@172.28.4.77 "
  source ~/experiments/scripts/config.sh
  echo '=== Scenario 3: GEMM + metric_push (offloaded to BF2) ==='

  # Start 1 metric_push on NUMA node 0 (1ms interval = 1000 reports/s)
  numactl --cpunodebind=0 \
    sudo ~/experiments/bench/metric_push/metric_push \
      --pci=0000:5e:00.0 \
      --interval=1 \
      --node-id=\"push-\$(hostname)\" \
    > ~/exp_data/B/scenario3_push.log 2>&1 &
  PUSH_PID=\$!
  sleep 3

  numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=16 \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=60 \
    > ~/exp_data/B/scenario3_gflops.txt 2>/dev/null &
  GPID=\$!; sleep 1

  sudo perf stat -p \${GPID} \
    -e LLC-load-misses,LLC-loads,context-switches,instructions \
    -I 1000 -o ~/exp_data/B/scenario3_perf.txt \
    sleep 60 &

  wait \${GPID}

  sudo kill \${PUSH_PID} 2>/dev/null
  sleep 5
  echo 'Scenario 3 done'
  tail -5 ~/exp_data/B/scenario3_gflops.txt
"
```

### Cleanup and collect

```bash
# Stop services
kill ${MASTER_PID} 2>/dev/null
ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'pkill -f forward_routine'"

# Fetch results
scp $(whoami)@172.28.4.77:~/exp_data/B/scenario*_gflops.txt ~/exp_data/B/
scp $(whoami)@172.28.4.77:~/exp_data/B/scenario*_perf.txt ~/exp_data/B/

# Quick summary
echo "=== Experiment B Quick Summary ==="
for s in 1 2 3; do
  f=~/exp_data/B/scenario${s}_gflops.txt
  if [ -f "$f" ]; then
    avg=$(awk 'NR>5 {s+=$1; n++} END{if(n>0) printf "%.3f", s/n}' "$f")
    echo "  Scenario ${s}: avg GFLOPS = ${avg}"
  fi
done
```

**Data to collect**: paste the full summary including GFLOPS, LLC miss rates,
context-switch counts, interference%, and recovery%.

### Reproducibility (optional)

Repeat on helong (172.28.4.85).  Replace `172.28.4.77` with `172.28.4.85`,
`enp94s0f1np1` with `enp94s0f0np0`, and `192.168.56.11` with `192.168.56.12`.

---

## 8. Experiment C — Control-Plane Scalability

Tests master_monitor scalability as node count increases from 4 to 256.
Mock nodes (pthreads in mock_slave) connect via 100G fabric to master.

### Architecture

```
tianjin (192.168.56.10)
  master_monitor :9000    ← receives TCP from all mock nodes
        ▲
        │ TCP via 100G fabric (192.168.56.x)
  ┌─────┴──────┬──────────────┐
  fujian       helong
  mock_slave   mock_slave
  (N/2 nodes)  (N/2 nodes)
```

### Parameters

- **Scale points**: 4, 16, 64, 256 nodes
- **Report interval**: 1000 ms (1 report per node per second)
- **Warmup**: 10 seconds (all nodes register before measurement)
- **Measurement**: 30 seconds (pidstat sampling)

### Run

```bash
cd ~/experiments
chmod +x scripts/exp_C_scale.sh
bash scripts/exp_C_scale.sh
```

The script will:
1. For each scale point N: start master_monitor on tianjin
2. Launch mock_slave with N/2 threads on fujian and N/2 on helong
3. Wait 10s for registration, then pidstat master_monitor for 30s
4. Collect mock_slave latency stats, stop everything, move to next N

### After completion

```bash
python3 scripts/analyze/analyze_C.py
```

Prints table: nodes, master CPU%, RSS MB, avg latency, reports/s, error rate.
Also fits a linear model for CPU% vs node count.

### Optional: high-frequency stress test

Re-run with 100ms interval (10× the report rate):

```bash
bash scripts/exp_C_scale.sh --interval=100
```

At 256 nodes × 10 reports/s = 2560 reports/s — tests master under heavy load.

**Data to collect**: paste the printed table and linear fit here

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `enp94s0f1np1: No such device` | Check `ip link show`; the interface may be named differently. Use `ls /sys/class/net/ \| grep enp` |
| Ping 192.168.56.x fails between host and BF2 | Check OVS bridge: `ssh root@192.168.100.2 "ovs-vsctl show"`. Ensure p1 and pf1hpf are in the same bridge. |
| BF2-to-BF2 ping fails | Check `ip addr show p1` on both BF2s; verify 100G switch link: `ethtool p1 \| grep Link` |
| `doca_devinfo_create_list failed` | `sudo mlnx_bf_configure` or `echo 1 > /sys/bus/pci/rescan` |
| Comch connect timeout | Check BF2 log: `ssh root@192.168.100.2 cat /tmp/forward_routine.log` |
| `libdoca_comch not found` | Run `make info` in `tunnel/host/` |
| perf permission denied | `sudo sysctl -w kernel.perf_event_paranoid=1` |
| `gemm_bench: CBLAS error` | `sudo apt-get install -y libopenblas-dev` |
| Multiple slave_monitors fail with Comch | Comch supports 1 client per service name. Scenario 2 uses `--mode=direct` (TCP) to avoid this. Scenario 3 uses 1 metric_push instance. |
| sockperf not found on BF2 | `apt-get install -y sockperf` or build from source |

---

## Reporting Back

After each experiment, paste the output here verbatim.
Include any compilation errors, runtime errors, or unexpected behavior.
