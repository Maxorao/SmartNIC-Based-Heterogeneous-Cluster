# Remote Experiment Setup & Execution Guide

This document is intended for a Claude agent running on one of the lab host machines
(**tianjin / fujian / helong**).  Follow the steps in order.  Each section ends with
a verification command; do not proceed to the next section until it passes.

---

## 0. Context

| Component | Detail |
|-----------|--------|
| Host OS   | Ubuntu 22.04, x86 |
| Host DOCA | 3.1.0 (`/opt/mellanox/doca`) |
| Host DPDK | 22.11 (mlnx) |
| BF2 access | `ssh root@192.168.100.2` (no password) |
| BF2 OS    | Ubuntu 20.04 ARM |
| BF2 DOCA  | 1.5.4003 |
| BF2 PCI (host view) | `5e:00.0`, `5e:00.1` |
| BF2 PCI (ARM view)  | `03:00.0` |
| BF2 representor     | auto-detected (pf0hpf) |
| Repo | `git@github.com:Maxorao/SmartNIC-Based-Heterogeneous-Cluster.git` |

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

## 2. Edit Configuration

Open `scripts/config.sh` and fill in the values for **this specific host**:

```bash
# Key fields to update:
MASTER_IP="<IP of the node running master_monitor>"   # e.g. the host itself
MASTER_PORT="9000"
GNODE2_IP="$(hostname -I | awk '{print $1}')"         # this host's IP
GNODE2_BF_IP="192.168.100.2"
GNODE2_PCI="5e:00.0"                                  # BF2 PCI as seen from host
GNODE2_NIC_PCI="03:00.0"                              # BF2 device as seen from ARM
DB_CONNSTR="host=localhost dbname=cluster_metrics user=postgres password=postgres"
```

**Verify:**
```bash
source scripts/config.sh && echo "Config OK: MASTER_IP=$MASTER_IP"
```

---

## 3. Install Build Dependencies (host)

```bash
sudo apt-get install -y libopenblas-dev linux-tools-$(uname -r) sysstat stress-ng python3-pip
pip3 install numpy pandas
```

**Verify:**
```bash
dpkg -l libopenblas-dev | grep '^ii' && echo "OpenBLAS OK"
perf stat echo test 2>&1 | grep -q "Performance" && echo "perf OK"
```

---

## 4. Compile: Host-side Components

### 4a. Tunnel (host)
```bash
cd ~/experiments/tunnel/host
make COMCH_HOST_DOCA_VER=31
# Expected: produces comch_host.o
```

### 4b. Slave monitor
```bash
cd ~/experiments/control-plane/slave
make
# Expected: produces slave_monitor binary
```

### 4c. Master monitor
```bash
cd ~/experiments/control-plane/master
make
# Expected: produces master_monitor binary
```

### 4d. Benchmark programs
```bash
cd ~/experiments/bench/gemm_bench && make
cd ~/experiments/bench/latency_bench && make  # produces bench_host
cd ~/experiments/bench/mock_slave && make
```

**Verify (host):**
```bash
ls -la ~/experiments/control-plane/slave/slave_monitor \
       ~/experiments/control-plane/master/master_monitor \
       ~/experiments/bench/gemm_bench/gemm_bench \
       ~/experiments/bench/latency_bench/bench_host \
       ~/experiments/bench/mock_slave/mock_slave
# All five files should exist
```

---

## 5. Compile: BF2-side Components

Copy source to BF2 and compile there:

```bash
# Push repo to BF2
ssh root@192.168.100.2 "rm -rf ~/experiments && mkdir -p ~/experiments"
scp -r ~/experiments/tunnel     root@192.168.100.2:~/experiments/
scp -r ~/experiments/control-plane root@192.168.100.2:~/experiments/
scp -r ~/experiments/bench/latency_bench root@192.168.100.2:~/experiments/bench/
scp -r ~/experiments/common     root@192.168.100.2:~/experiments/

# Compile on BF2
ssh root@192.168.100.2 "
  cd ~/experiments/tunnel/nic && make COMCH_NIC_DOCA_VER=15
  cd ~/experiments/control-plane/forwarder && make
  cd ~/experiments/bench/latency_bench && make bench_nic
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

This step verifies that the DOCA 3.1 host-side Comch client can handshake
with the DOCA 1.5 BF2-side server.  **If this fails, report the error and
do not proceed — fall back to TCP mode (see Section 6b).**

### 6a. DOCA Comch smoke test

**On BF2** (background):
```bash
ssh root@192.168.100.2 "nohup ~/experiments/bench/latency_bench/bench_nic \
  --pci=03:00.0 --mode=comch > /tmp/bench_nic.log 2>&1 &"
sleep 3
```

**On host** (single ping-pong):
```bash
~/experiments/bench/latency_bench/bench_host \
  --pci=5e:00.0 --mode=comch --size=256 --iters=10 2>&1
```

**Expected output** (if compatible):
```
Comch ping-pong 256B x10: avg=X.XX µs  P99=X.XX µs
```

**If you see a connection error or timeout**, DOCA versions are likely not
wire-compatible.  Proceed to **6b**.

### 6b. TCP fallback (if 6a fails)

All experiments can fall back to TCP over the `tmfifo_net0` interface
(192.168.100.2).  Set the following in `scripts/config.sh`:

```bash
TUNNEL_MODE="tcp"          # overrides default "comch"
TUNNEL_TCP_IP="192.168.100.2"
TUNNEL_TCP_PORT="12345"
```

Then rerun the bench test:
```bash
ssh root@192.168.100.2 "nohup ~/experiments/bench/latency_bench/bench_nic \
  --mode=tcp > /tmp/bench_nic_tcp.log 2>&1 &"
~/experiments/bench/latency_bench/bench_host \
  --ip=192.168.100.2 --mode=tcp --size=256 --iters=10
```

---

## 7. Experiment A — Tunnel Latency

Run after section 6 is confirmed working.

```bash
cd ~/experiments
chmod +x scripts/exp_A_latency.sh
bash scripts/exp_A_latency.sh
```

This will:
- Run `bench_nic` on BF2 as echo server
- Run `bench_host` on this machine for sizes 64/256/1024/4096/65536 B
- Save per-sample CSV files to `~/exp_data/A/`

After completion:
```bash
python3 scripts/analyze/analyze_A.py
# Prints P50/P99/P99.9 table for all sizes × protocols
```

**Data to collect**: paste the printed table here.

---

## 8. Experiment B — Interference Elimination

> Requires: `master_monitor` running (start with `~/experiments/control-plane/master/master_monitor &`)

```bash
# Start master_monitor in background
source scripts/config.sh
"${MASTER_MONITOR}" --port="${MASTER_PORT}" \
  --db-connstr="${DB_CONNSTR}" > ~/exp_data/master.log 2>&1 &

# Run experiment B
bash scripts/exp_B_interference.sh
```

After completion:
```bash
python3 scripts/analyze/analyze_B.py
# Prints T_base, T_mixed, T_offload, interference%, recovery%
```

**Data to collect**: paste the printed summary here.

---

## 9. Experiment C — Scalability

```bash
bash scripts/exp_C_scale.sh
python3 scripts/analyze/analyze_C.py
```

**Data to collect**: paste the node-count table here.

---

## 10. Kubelet Reference Data

```bash
bash scripts/exp_kubelet.sh
# Prints kubelet vs slave_monitor CPU/memory comparison
```

**Data to collect**: paste the comparison lines here.

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `doca_devinfo_create_list failed` | DOCA not loaded | `sudo mlnx_bf_configure` or check `lspci` |
| Comch connect timeout | BF2 not listening | Check bench_nic log: `ssh root@192.168.100.2 cat /tmp/bench_nic.log` |
| `libdoca_comch not found` | Wrong lib name | Run `make info` in tunnel/host/ to check |
| perf permission denied | Kernel lockdown | `sudo sysctl -w kernel.perf_event_paranoid=1` |
| `gemm_bench: CBLAS error` | OpenBLAS not installed | `sudo apt-get install -y libopenblas-dev` |

---

## Reporting Back

After completing each experiment, paste the analysis output here.
Include any error messages verbatim so we can diagnose issues together.
