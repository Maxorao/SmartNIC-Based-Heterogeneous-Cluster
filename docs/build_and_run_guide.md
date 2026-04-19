# 构建与实验运行指南

本文档覆盖从源码到实验执行的完整流程。所有命令假定仓库位于
`~/experiments`（主机和 BF2 上路径一致）。

---

## 1. 一次性构建

### 1.1 主机侧构建（x86 / DOCA 3.1）

在每台主机（tianjin, fujian, helong）上：

```bash
cd ~/experiments
mkdir -p build && cd build
cmake -DBUILD_TARGET=HOST ..
make -j$(nproc)

# 产出：
#   build/control-plane/master/cluster_master
#   build/bench/metric_push/metric_push
#   build/bench/mock_slave/mock_slave
#   build/bench/rdma_e2e/e2e_host
#   build/bench/latency_bench/bench_host  (仍用现有 Makefile)
```

同时确保 `latency_bench` / `gemm_bench` 通过各自 Makefile 构建：

```bash
cd ~/experiments/bench/latency_bench && make
cd ~/experiments/bench/gemm_bench    && make
```

Python protos（orchestrator.py 使用）：

```bash
pip install -r ~/experiments/control-plane/orchestrator/requirements.txt
bash ~/experiments/scripts/gen_py_protos.sh
```

### 1.2 BF2 侧构建（aarch64 / DOCA 1.5）

在每个 BF2 ARM 上（通过 `ssh root@192.168.100.2` 进入）：

```bash
cd /root/experiments
mkdir -p build && cd build
cmake -DBUILD_TARGET=BF2 ..
make -j4

# 产出：
#   build/control-plane/slave/slave_agent
#   build/control-plane/watchdog/master_watchdog
#   build/control-plane/rdma_bridge/rdma_bridge_slave
#   build/control-plane/rdma_bridge/rdma_bridge_master
#   build/control-plane/orchestrator_agent/orchestrator_agent
#   build/bench/rdma_e2e/e2e_nic
```

BF2 依赖：
```bash
apt install libibverbs-dev librdmacm-dev libgrpc++-dev protobuf-compiler-grpc
```

### 1.3 一键构建脚本

```bash
# 主机侧（在 tianjin 上）
for h in ${TIANJIN_IP} ${FUJIAN_IP} ${HELONG_IP}; do
    ssh ${USER}@${h} "cd ~/experiments && mkdir -p build && cd build && \
        cmake -DBUILD_TARGET=HOST .. && make -j\$(nproc)"
done

# BF2 侧（通过每台主机的 tmfifo）
for h in ${TIANJIN_IP} ${FUJIAN_IP} ${HELONG_IP}; do
    ssh ${USER}@${h} "ssh root@192.168.100.2 'cd /root/experiments && \
        mkdir -p build && cd build && cmake -DBUILD_TARGET=BF2 .. && make -j4'"
done
```

---

## 2. 一次性集群配置

### 2.1 BF2 RDMA/OVS 环境（一次）

在每个 BF2 上执行（按角色）：

```bash
# tianjin BF2
ssh root@192.168.100.2 "BF_ROLE=A /root/experiments/scripts/bf2_rdma_setup.sh"

# fujian BF2  (from fujian host)
ssh ${USER}@${FUJIAN_IP} "ssh root@192.168.100.2 \
    'BF_ROLE=B /root/experiments/scripts/bf2_rdma_setup.sh'"

# helong BF2
ssh ${USER}@${HELONG_IP} "ssh root@192.168.100.2 \
    'BF_ROLE=C /root/experiments/scripts/bf2_rdma_setup.sh'"
```

详细步骤见 `docs/bf2_interconnect_setup.md`。

### 2.2 全局验证

```bash
cd ~/experiments
bash scripts/bf2_cluster_verify.sh
```

所有 ≥ 10 个检查应显示 `[OK]`。任何 `[FAIL]` 需先排除（日志在 `~/exp_data/cluster_verify.log`）。

### 2.3 orchestrator_agent 部署（一次）

```bash
bash ~/experiments/scripts/deploy_orchestrator_agent.sh
```

会在每个 BF2 上安装 systemd unit `orch-agent.service`，监听 `:50052`。
验证：
```bash
nc -zv ${FUJIAN_BF2_FABRIC} 50052
nc -zv ${HELONG_BF2_FABRIC} 50052
nc -zv ${TIANJIN_BF2_FABRIC} 50052
```

---

## 3. 实验执行顺序（按依赖关系）

### 阶段 1：基础验证（无依赖）

```bash
# 集群连通性
bash scripts/bf2_cluster_verify.sh

# 实验 A：Comch + TCP 延迟基线（已有）
bash scripts/exp_A_latency.sh
python3 scripts/analyze/analyze_A.py
```

### 阶段 2：端到端 RDMA 延迟验证（关键）

```bash
# 启动端到端测量：tianjin host → tianjin BF2 → RDMA → fujian BF2 → fujian host
bash scripts/exp_A2_e2e_rdma.sh

# 期望：P50 RTT ≈ 120-140 us （one-way ≈ 60-70 us），验证端到端 62 us 设计预期
```

### 阶段 3：加速比实验（各 10 次统计重复）

```bash
# 实验 B：控制平面卸载加速（10 次重复）
bash scripts/run_repeated.sh scripts/exp_B_interference.sh 10 \
    ~/exp_data/B_repeats.csv
python3 scripts/analyze/stats.py ~/exp_data/B_repeats.csv

# 实验 H：工作负载编排加速（10 次重复）
bash scripts/run_repeated.sh scripts/ch4_exp_H_interference.sh 10 \
    ~/exp_data/H_repeats.csv
python3 scripts/analyze/stats.py ~/exp_data/H_repeats.csv

# 实验 K：端到端系统加速（10 次重复）
bash scripts/run_repeated.sh scripts/ch5_exp_K_e2e.sh 10 \
    ~/exp_data/K_repeats.csv
python3 scripts/analyze/stats.py ~/exp_data/K_repeats.csv
```

### 阶段 4：补强实验

```bash
# 实验 I2：Nginx 尾延迟 P50/P99/P999
bash scripts/ch4_exp_I2_tail_latency.sh
python3 scripts/analyze/analyze_I_tail.py

# K.4：cgroups 公允基线
bash scripts/ch5_exp_K_cgroups.sh
python3 scripts/analyze/emit_summary.py --exp K \
    --data-dir ~/exp_data/K --out ~/exp_data/K/summary_with_cgroups.csv

# 阈值敏感性扫描 (5 θ × 3 label × 3 rep = 45 次 × 5min ≈ 4 小时)
bash scripts/ch4_exp_K_threshold_sweep.sh
python3 scripts/analyze/analyze_threshold_sweep.py

# J2：本地编排代理迁移时间对比
bash scripts/ch4_exp_J2_local_agent.sh
```

### 阶段 5：RDMA 控制面集成端到端

```bash
# 1. 在 tianjin BF2 启动 rdma_bridge_master
ssh root@192.168.100.2 \
    "/root/experiments/build/control-plane/rdma_bridge/rdma_bridge_master \
      --bind-ip=192.168.56.102 --port=7889 \
      --master-grpc=192.168.100.1:50051" &

# 2. 在 fujian BF2 启动 rdma_bridge_slave
ssh ${USER}@${FUJIAN_IP} "ssh root@192.168.100.2 \
    '/root/experiments/build/control-plane/rdma_bridge/rdma_bridge_slave \
      --master-ip=192.168.56.102 --port=7889 \
      --uds=/var/run/rdma_bridge.sock' &"

# 3. slave_agent 开启 --rdma-uds 模式
ssh ${USER}@${FUJIAN_IP} "ssh root@192.168.100.2 \
    '/root/experiments/build/control-plane/slave/slave_agent \
      --dev-pci=03:00.0 --master-addr=192.168.56.10:50051 \
      --rdma-uds=/var/run/rdma_bridge.sock \
      --rdma-only' &"

# 4. 观察 cluster_master 是否接收到 DirectPush 消息
# (cluster_master 日志应显示 DirectPush 调用)

# 5. 独立测量 RDMA 热路径延迟：再跑一次 A2 并对比
bash scripts/exp_A2_e2e_rdma.sh
```

---

## 4. 故障排查

| 症状 | 排查方向 |
|------|----------|
| CMake 找不到 gRPC | 检查 `pkg-config --exists grpc++`；安装 `libgrpc++-dev` |
| CMake 找不到 DOCA | 确认 `/opt/mellanox/doca` 存在，或 `cmake -DDOCA_DIR=/path/to/doca` |
| 找不到 libibverbs/librdmacm | `apt install libibverbs-dev librdmacm-dev` |
| e2e_nic 启动后 RDMA accept 卡住 | 确保对端 e2e_nic 先启动且 bf2_cluster_verify 通过 |
| slave_agent RDMA UDS 返回 ENOTCONN | rdma_bridge_slave 尚未启动；用 systemd 或手动先拉起 |
| orchestrator_agent 端口 50052 拒绝连接 | `systemctl status orch-agent`；查 `/var/log/orch-agent.log` |
| `grep: RESULT:` 解析失败 (J2) | 检查 `iter<N>.log` 内 Python 栈追踪 |

---

## 5. 数据目录布局

```
~/exp_data/
├── A/              # exp_A 延迟
├── A_e2e/          # exp_A2 端到端 RDMA
├── B/              # exp_B 控制平面干扰
├── H/              # ch4_exp_H 工作负载干扰
├── I_tail/         # ch4_exp_I2 尾延迟
├── J_local_agent/  # ch4_exp_J2 迁移时间
├── K/              # ch5_exp_K 端到端 (含 K.4 cgroups)
├── threshold_sweep/# θ_llc 敏感性
└── repeats/        # run_repeated.sh 每次运行的 summary.csv
```

每个实验目录下通常包含 `summary.csv`（机器可读汇总）和单独的原始输出文件。
