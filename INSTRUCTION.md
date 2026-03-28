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
    │  tmfifo .100.1  │  tmfifo .100.1  │  tmfifo .100.1  │
    │  ↕ PCIe         │  ↕ PCIe         │  ↕ PCIe         │
    │  BF2 .100.2     │  BF2 .100.2     │  BF2 .100.2     │
    │  p1:56.2        │  p1:56.3        │  p0:56.1        │
    └────────┬────────┘────────┬────────┘────────┬────────┘
             └─────────────────┴─────────────────┘
                  192.168.56.0/24 (100G switch)
```

### Communication paths

| Path | From → To | Use |
|------|-----------|-----|
| Comch (PCIe) | host ↔ local BF2 | Tunnel: host metric_push/slave_monitor ↔ BF2 |
| tmfifo | host ↔ local BF2 | SSH management |
| 100G fabric | BF2 ↔ BF2 | Offloaded control-plane messages |
| eno1 LAN | host ↔ host | Baseline TCP + SSH between hosts |

### Key facts

- Each host has 2 sockets × 16 cores × 2 HT = 64 logical CPUs (Xeon Gold 5218 @ 2.30 GHz)
- Each BF2 ARM has 8 cores (Cortex-A72)
- Host DOCA 3.1 (`libdoca_comch`), BF2 DOCA 1.5 (`libdoca_comm_channel`)
- BF2 PCI from host: `0000:5e:00.0`; from ARM: `03:00.0`
- SSH to BF2: `ssh root@192.168.100.2` (no password)

---

## 1. BF2 Fabric Network Setup

Assign IPs to the switch-connected BF2 ports so all three BF2 ARMs can
communicate over the 100G fabric.

**helong BF2 p0 already has 192.168.56.1.**  Configure tianjin and fujian:

```bash
# On tianjin (this machine):
ssh root@192.168.100.2 "ip addr add 192.168.56.2/24 dev p1 2>/dev/null; ip link set p1 up"

# On fujian:
ssh root@192.168.100.2 -J none -o ProxyCommand="ssh -W %h:%p $(whoami)@172.28.4.77" \
  "ip addr add 192.168.56.3/24 dev p1 2>/dev/null; ip link set p1 up"
```

If the ProxyCommand approach is difficult, SSH into fujian first, then SSH to its BF2:

```bash
ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'ip addr add 192.168.56.3/24 dev p1 2>/dev/null; ip link set p1 up'"
```

**Verify (from tianjin BF2):**
```bash
ssh root@192.168.100.2 "
  ping -c 2 192.168.56.1 && echo 'helong BF2 OK' &&
  ping -c 2 192.168.56.3 && echo 'fujian BF2 OK'
"
```

All three BF2s must be able to reach each other on 192.168.56.0/24.

---

## 2. Master-side BF2 Relay

Worker BF2s send control messages over 100G to tianjin BF2, which must
relay them to master_monitor on the tianjin host via tmfifo.

Install `socat` on tianjin BF2 and start the relay:

```bash
ssh root@192.168.100.2 "
  apt-get install -y socat 2>/dev/null || yum install -y socat 2>/dev/null
  # Kill any old relay
  pkill -f 'socat.*9100' 2>/dev/null || true
  # Relay: BF2 port 9100 (100G fabric) → host port 9000 (tmfifo)
  nohup socat TCP-LISTEN:9100,fork,reuseaddr TCP:192.168.100.1:9000 \
    > /tmp/socat_relay.log 2>&1 &
"
sleep 1
```

**Verify:**
```bash
ssh root@192.168.100.2 "ss -tlnp | grep 9100"
# Should show socat listening on :9100
```

---

## 3. Clone Repository (all hosts)

```bash
# On tianjin (this machine):
cd ~ && git clone git@github.com:Maxorao/SmartNIC-Based-Heterogeneous-Cluster.git experiments 2>/dev/null || (cd ~/experiments && git pull)

# On fujian and helong:
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

## 4. Install Dependencies (all hosts)

```bash
for host in 172.28.4.75 172.28.4.77 172.28.4.85; do
  ssh $(whoami)@${host} "
    sudo apt-get update -qq &&
    sudo apt-get install -y -qq libopenblas-dev linux-tools-\$(uname -r) \
      sysstat stress-ng python3-pip sockperf 2>/dev/null &&
    pip3 install -q numpy pandas
  " &
done
wait
echo "All hosts done"
```

**Verify (on any host):**
```bash
dpkg -l libopenblas-dev | grep '^ii' && echo "OpenBLAS OK"
which sockperf && echo "sockperf OK"
sudo sysctl -w kernel.perf_event_paranoid=1   # enable perf for non-root
```

---

## 5. Compile: Host-side (all hosts)

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

## 6. Compile: BF2-side (all BF2s)

```bash
# Push sources and compile on each BF2
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

## 7. Experiment A — Communication Path Latency

Measures one-way latency (RTT/2) across five communication paths.

### Path ① Comch host↔BF2 (PCIe kernel-bypass)

Already measured: ~29 µs.  Re-run on fujian for fresh data:

```bash
# On fujian — start BF2 echo server:
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 'pkill bench_nic 2>/dev/null; sleep 1
    nohup ~/experiments/bench/latency_bench/bench_nic --pci=03:00.0 --mode=comch \
    > /tmp/bench_nic.log 2>&1 &'
  sleep 3
  # Run from host:
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

### Path ③ TCP LAN host→host (eno1, baseline for direct control plane)

Uses sockperf for precise measurement:

```bash
# Start sockperf server on tianjin:
sockperf sr --tcp --port 11111 &
SOCKPERF_PID=$!
sleep 1

# Run from fujian:
ssh $(whoami)@172.28.4.77 "
  for size in 64 256 1024; do
    echo '--- TCP LAN \${size}B ---'
    sockperf pp --tcp -i 172.28.4.75 --port 11111 \
      -m \${size} -t 10 --full-rtt \
      2>&1 | tee ~/exp_data/A/tcp_lan_\${size}B.txt
  done
"

kill $SOCKPERF_PID 2>/dev/null
```

### Path ④ TCP 100G BF2→BF2 (offloaded control-plane fabric)

```bash
# Start sockperf server on tianjin BF2:
ssh root@192.168.100.2 "
  pkill sockperf 2>/dev/null
  apt-get install -y sockperf 2>/dev/null || true
  nohup sockperf sr --tcp --port 11111 > /tmp/sockperf.log 2>&1 &
"
sleep 2

# Run from fujian BF2:
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    apt-get install -y sockperf 2>/dev/null || true
    for size in 64 256 1024; do
      echo \"--- BF2-BF2 100G \${size}B ---\"
      sockperf pp --tcp -i 192.168.56.2 --port 11111 \
        -m \${size} -t 10 --full-rtt
    done
  '
" | tee ~/exp_data/A/tcp_bf2_100g.txt

# Cleanup
ssh root@192.168.100.2 "pkill sockperf"
```

### Path ⑤ End-to-end offload path (BF2→100G→BF2→tmfifo→host)

This tests the complete offloaded control-plane path latency.
Use the socat relay (port 9100 on tianjin BF2 → port 9000 on tianjin host).

```bash
# Start a simple echo server on tianjin host port 9000:
# (or use sockperf server)
sockperf sr --tcp --port 9000 &
ECHO_PID=$!
sleep 1

# From fujian BF2, connect through 100G → tianjin BF2 relay → tianjin host:
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    for size in 64 256 1024; do
      echo \"--- E2E offload \${size}B ---\"
      sockperf pp --tcp -i 192.168.56.2 --port 9100 \
        -m \${size} -t 10 --full-rtt
    done
  '
" | tee ~/exp_data/A/tcp_e2e_offload.txt

kill $ECHO_PID 2>/dev/null
```

### Collect results

```bash
echo "=== Experiment A Summary ===" | tee ~/exp_data/A/summary.txt
echo "" >> ~/exp_data/A/summary.txt
echo "Path ①②: see bench_host output in ~/exp_data/A/" >> ~/exp_data/A/summary.txt
echo "Path ③④⑤: see sockperf output files" >> ~/exp_data/A/summary.txt
cat ~/exp_data/A/tcp_lan_*.txt ~/exp_data/A/tcp_bf2_100g.txt ~/exp_data/A/tcp_e2e_offload.txt \
  >> ~/exp_data/A/summary.txt 2>/dev/null
```

**Data to collect**: paste the full summary here.

---

## 8. Experiment B — Interference Elimination

This is the core experiment.  Measures GEMM throughput on a worker host
(fujian) under three control-plane configurations.

### Architecture

```
Scenario 2 (no offload):
  fujian host: GEMM + N×slave_monitor → Comch → BF2 → TCP 100G → tianjin BF2 → relay → master
  (slave_monitor does: read /proc + build protocol msg + manage heartbeat + Comch send)

Scenario 3 (offloaded):
  fujian host: GEMM + N×metric_push → Comch → BF2 (forward_routine) → TCP 100G → tianjin BF2 → relay → master
  (metric_push does: read /proc + Comch send raw report — no protocol logic)
```

### Parameters

- **GEMM**: 16 threads on NUMA node 0 (16 physical cores, ~100% CPU)
- **N_MONITORS**: 8 instances (simulates kubelet + metrics + logging + health)
- **Interval**: 10 ms per instance (100 reports/s each, 800 total)
- **Duration**: 60 seconds per scenario

### Setup (run once before starting scenarios)

```bash
source ~/experiments/scripts/config.sh

# 1. Start master_monitor on tianjin host
"${MASTER_MONITOR}" --port="${MASTER_PORT}" \
  > ~/exp_data/B/master.log 2>&1 &
MASTER_PID=$!
sleep 2

# 2. Ensure socat relay on tianjin BF2 (from step 2)
ssh root@192.168.100.2 "ss -tlnp | grep 9100 || (
  nohup socat TCP-LISTEN:9100,fork,reuseaddr TCP:192.168.100.1:9000 \
    > /tmp/socat_relay.log 2>&1 &
)"

# 3. Start forward_routine on fujian BF2
ssh $(whoami)@172.28.4.77 "
  ssh root@192.168.100.2 '
    pkill -f forward_routine 2>/dev/null; sleep 1
    nohup ~/experiments/control-plane/forwarder/forward_routine \
      --pci=03:00.0 \
      --master-ip=192.168.56.2 \
      --master-port=9100 \
      > /tmp/forward_routine.log 2>&1 &
  '
"
sleep 3
echo "Setup complete. master_PID=${MASTER_PID}"
```

### Scenario 1: Baseline (GEMM only)

```bash
ssh $(whoami)@172.28.4.77 "
  source ~/experiments/scripts/config.sh
  mkdir -p ~/exp_data/B

  echo '=== Scenario 1: GEMM baseline ==='

  # Run GEMM on NUMA node 0
  numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=${GEMM_THREADS} \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=${GEMM_DURATION} \
    > ~/exp_data/B/scenario1_gflops.txt 2>/dev/null &
  GEMM_PID=\$!
  sleep 1

  # Attach perf stat
  sudo perf stat -p \${GEMM_PID} \
    -e LLC-load-misses,LLC-loads,context-switches,instructions \
    -I 1000 -o ~/exp_data/B/scenario1_perf.txt \
    sleep ${GEMM_DURATION} &

  wait \${GEMM_PID}
  sleep 2
  echo 'Scenario 1 done'
  head -3 ~/exp_data/B/scenario1_gflops.txt
"
```

### Scenario 2: No offload (GEMM + N×slave_monitor on host)

slave_monitor runs on fujian host, Comch-sends to BF2, BF2 forwards to master via 100G.

```bash
ssh $(whoami)@172.28.4.77 "
  source ~/experiments/scripts/config.sh

  echo '=== Scenario 2: GEMM + ${N_MONITORS}x slave_monitor (no offload) ==='

  # Start N slave_monitor instances on NUMA node 0
  SLAVE_PIDS=()
  for i in \$(seq 1 ${N_MONITORS}); do
    numactl --cpunodebind=0 \
      sudo ~/experiments/control-plane/slave/slave_monitor \
        --mode=offload \
        --pci=${HOST_PCI} \
        --interval=${HIGH_LOAD_INTERVAL} \
        --node-id=\"slave-\${i}-\$(hostname)\" \
      > ~/exp_data/B/scenario2_slave_\${i}.log 2>&1 &
    SLAVE_PIDS+=(\$!)
  done
  sleep 3

  # Start GEMM on same NUMA node
  numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=${GEMM_THREADS} \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=${GEMM_DURATION} \
    > ~/exp_data/B/scenario2_gflops.txt 2>/dev/null &
  GEMM_PID=\$!
  sleep 1

  sudo perf stat -p \${GEMM_PID} \
    -e LLC-load-misses,LLC-loads,context-switches,instructions \
    -I 1000 -o ~/exp_data/B/scenario2_perf.txt \
    sleep ${GEMM_DURATION} &

  wait \${GEMM_PID}

  # Stop slave_monitors
  for pid in \${SLAVE_PIDS[@]}; do
    sudo kill \${pid} 2>/dev/null
  done
  sleep 5
  echo 'Scenario 2 done'
  head -3 ~/exp_data/B/scenario2_gflops.txt
"
```

### Scenario 3: Offloaded (GEMM + N×metric_push on host)

metric_push is the lightweight shim — just reads /proc and Comch-sends
raw data.  All protocol logic runs on the BF2.

```bash
ssh $(whoami)@172.28.4.77 "
  source ~/experiments/scripts/config.sh

  echo '=== Scenario 3: GEMM + ${N_MONITORS}x metric_push (offloaded) ==='

  # Start N metric_push instances on NUMA node 0
  PUSH_PIDS=()
  for i in \$(seq 1 ${N_MONITORS}); do
    numactl --cpunodebind=0 \
      sudo ~/experiments/bench/metric_push/metric_push \
        --pci=${HOST_PCI} \
        --interval=${HIGH_LOAD_INTERVAL} \
        --node-id=\"push-\${i}-\$(hostname)\" \
      > ~/exp_data/B/scenario3_push_\${i}.log 2>&1 &
    PUSH_PIDS+=(\$!)
  done
  sleep 3

  # Start GEMM on same NUMA node
  numactl --cpunodebind=0 --membind=0 \
    env OPENBLAS_NUM_THREADS=${GEMM_THREADS} \
    ~/experiments/bench/gemm_bench/gemm_bench --duration=${GEMM_DURATION} \
    > ~/exp_data/B/scenario3_gflops.txt 2>/dev/null &
  GEMM_PID=\$!
  sleep 1

  sudo perf stat -p \${GEMM_PID} \
    -e LLC-load-misses,LLC-loads,context-switches,instructions \
    -I 1000 -o ~/exp_data/B/scenario3_perf.txt \
    sleep ${GEMM_DURATION} &

  wait \${GEMM_PID}

  # Stop metric_push instances
  for pid in \${PUSH_PIDS[@]}; do
    sudo kill \${pid} 2>/dev/null
  done
  sleep 5
  echo 'Scenario 3 done'
  head -3 ~/exp_data/B/scenario3_gflops.txt
"
```

### Cleanup and collect

```bash
# Stop master_monitor
kill ${MASTER_PID} 2>/dev/null

# Stop forward_routine on fujian BF2
ssh $(whoami)@172.28.4.77 "ssh root@192.168.100.2 'pkill -f forward_routine'"

# Fetch results from fujian
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

# Run analysis
python3 ~/experiments/scripts/analyze/analyze_B.py
```

**Data to collect**: paste the full summary, including per-scenario GFLOPS,
LLC miss rates, context-switch counts, interference%, and recovery%.

### Reproducibility (optional)

Repeat Experiment B on helong (172.28.4.85) to confirm results on a
second machine.  Use the same commands, replacing `172.28.4.77` with
`172.28.4.85` and adjusting the BF2 fabric IP from `192.168.56.3` to
`192.168.56.1`.

---

## 9. Experiment C — Scalability (deferred)

This experiment tests master_monitor scalability with increasing node count.
It will be designed after Chapter 2 experiments are validated.  Placeholder:

- Master: tianjin host
- Real workers: fujian BF2 + helong BF2 (2 real)
- Mock workers: Docker containers on fujian + helong
- Scale: 2, 8, 32, 128 total nodes

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `socat: command not found` on BF2 | `apt-get install -y socat` or `yum install -y socat` |
| BF2 ping 192.168.56.x fails | Check `ip addr show p1` on BF2; verify switch link is UP |
| `doca_devinfo_create_list failed` | `sudo mlnx_bf_configure` or rescan: `echo 1 > /sys/bus/pci/rescan` |
| Comch connect timeout | Check `ssh root@192.168.100.2 cat /tmp/forward_routine.log` |
| `libdoca_comch not found` | `make info` in `tunnel/host/` to check installed libs |
| perf permission denied | `sudo sysctl -w kernel.perf_event_paranoid=1` |
| `gemm_bench: CBLAS error` | `sudo apt-get install -y libopenblas-dev` |
| Multiple Comch instances fail | Only ONE Comch client can connect per BF2 at a time.  For N monitors, they must share a single Comch connection (see note below). |

### Important: Comch single-connection limitation

DOCA Comch supports **one client connection per service name**.  If you need
N slave_monitor instances, they cannot each open their own Comch connection.

**Workaround options for N>1 monitors:**
1. Run all N instances in **direct TCP mode** (`--mode=direct`) connecting to
   forward_routine via a local TCP port, and have ONE forward_routine handle
   the Comch tunnel.
2. Use different service names for each instance (requires code changes).
3. Run a single slave_monitor/metric_push that internally does N× the work
   (higher frequency or multiple /proc reads per cycle).

**Recommended for Experiment B:** Use option 1.  Start forward_routine on
the **host** listening on a local TCP port, and have N slave_monitors connect
to it via localhost TCP.  forward_routine handles the single Comch channel
to the BF2.

If this limitation is encountered, adapt the experiment script as follows:

```bash
# On fujian host: start forward_routine as local TCP-to-Comch proxy
sudo ~/experiments/control-plane/forwarder/forward_routine \
  --pci=0000:5e:00.0 --listen-port=8888 > /tmp/fwd_host.log 2>&1 &

# N slave_monitors connect to localhost:8888 (TCP, not Comch)
for i in $(seq 1 $N_MONITORS); do
  numactl --cpunodebind=0 \
    ~/experiments/control-plane/slave/slave_monitor \
      --mode=direct --master-ip=127.0.0.1 --master-port=8888 \
      --interval=${HIGH_LOAD_INTERVAL} --node-id="slave-${i}" &
done
```

This preserves the measurement: slave_monitor still runs on the host CPU,
and the BF2 path is still used for inter-node communication.

---

## Reporting Back

After each experiment, paste the output here verbatim.
Include any compilation errors, runtime errors, or unexpected behavior.
