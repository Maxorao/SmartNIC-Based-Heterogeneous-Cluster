# BF2 集群互联配置与验证指南

本文档描述 3 主机 + 3 BlueField-2 SmartNIC 集群的网络互联配置，覆盖：
- 主机-主机 100G 以太网链路
- 主机-BF2 tmfifo 管理通道（192.168.100.0/24）
- BF2-BF2 RDMA (RoCE v2) 数据通路（192.168.56.0/24）
- OVS hw-offload 与 Scalable Function (SF) 配置

---

## 1. 集群拓扑

```
┌───────────────────────┐    ┌───────────────────────┐    ┌───────────────────────┐
│    tianjin (172.28.4.75)  │    │    fujian  (172.28.4.77)  │    │    helong  (172.28.4.85)  │
│   (master node)       │    │   (worker node)       │    │   (worker node)       │
│                       │    │                       │    │                       │
│   100G: 192.168.56.10 │    │   100G: 192.168.56.11 │    │   100G: 192.168.56.12 │
│   enp94s0f1np1        │    │   enp94s0f1np1        │    │   enp94s0f0np0        │
│       │               │    │       │               │    │       │               │
│  [PCIe] BF2           │    │  [PCIe] BF2           │    │  [PCIe] BF2           │
│   fabric: 56.2 (p1)   │    │   fabric: 56.3 (p1)   │    │   fabric: 56.1 (p0)   │
│   tmfifo: 100.2 (BF2) │    │   tmfifo: 100.2 (BF2) │    │   tmfifo: 100.2 (BF2) │
└──────────┬────────────┘    └──────────┬────────────┘    └──────────┬────────────┘
           │                            │                            │
           └────────────────100G Switch (RoCE v2 enabled)─────────────┘
```

角色说明：
- **tianjin**：主控节点。运行 `cluster_master`、TimescaleDB、`orchestrator`（主机侧）。tianjin-BF2 运行 `master_watchdog`、`rdma_bridge_master`、`orchestrator_agent`。
- **fujian / helong**：工作节点。运行 `metric_push`（主机侧）、DGEMM/Nginx 用户负载。fujian/helong-BF2 运行 `slave_agent`、`rdma_bridge_slave`、`orchestrator_agent`，可承载迁移过来的 Nginx 容器。

---

## 2. 一次性配置（每个 BF2 执行一次）

### 2.1 主机侧 BF2 管理通道（tmfifo）

每台主机出厂默认 tmfifo 会自动起来，可通过 `ip a show tmfifo_net0` 确认。

默认地址：
- 主机侧：`192.168.100.1/30`
- BF2 侧：`192.168.100.2/30`

使用 `ssh root@192.168.100.2` 可登录本地 BF2。三台主机的 BF2 都使用相同地址（因为 tmfifo 是 point-to-point）。

### 2.2 BF2 模式切换

BF2 必须处于 **DPU 模式 + switchdev**。在每个 BF2 上：

```bash
# 在 BF2 上执行
mlxconfig -d /dev/mst/mt41686_pciconf0 set INTERNAL_CPU_MODEL=1
# 之后冷重启 BF2（整机断电重启或 mlxfwreset）

# 确认处于 switchdev 模式
devlink dev eswitch show pci/0000:03:00.0
# 期望输出: mode switchdev
# 若为 legacy:
devlink dev eswitch set pci/0000:03:00.0 mode switchdev
```

### 2.3 启用 OVS 硬件卸载

```bash
# 在每个 BF2 上
ovs-vsctl set Open_vSwitch . other_config:hw-offload=true
systemctl restart openvswitch-switch
```

验证：
```bash
ovs-vsctl get Open_vSwitch . other_config:hw-offload
# 期望: "true"
```

### 2.4 100G 物理端口与 OVS 网桥

每个 BF2 的物理上行口 `p0`/`p1` 加入 OVS 网桥 `ovsbr1`（已在出厂配置中存在）。添加主机代表口 `pf0hpf` 建立 L2 桥接：

```bash
# fujian BF2 示例（tianjin 类似，helong 用 p0 替代 p1）
ovs-vsctl --may-exist add-br ovsbr1
ovs-vsctl --may-exist add-port ovsbr1 p1       # 100G 物理口
ovs-vsctl --may-exist add-port ovsbr1 pf0hpf   # 主机代表口
ovs-ofctl del-flows ovsbr1
ovs-ofctl add-flow ovsbr1 "actions=NORMAL"    # L2 MAC 学习，走硬件流表

ip link set ovsbr1 up
ip link set p1 up
ip link set pf0hpf up
```

### 2.5 Scalable Function (SF) + RoCE v2 配置

SF 是 BF2 上的轻量级虚拟 RDMA 设备，独立于主机网卡接口。每个 BF2 创建一个 SF 用于 BF2-BF2 RDMA 通信。

**本文库已提供 `scripts/bf2_rdma_setup.sh`**，在每个 BF2 上按角色执行：

```bash
# tianjin BF2 (master BF2，角色 A)
BF_ROLE=A /root/experiments/scripts/bf2_rdma_setup.sh

# fujian BF2 (角色 B)
BF_ROLE=B /root/experiments/scripts/bf2_rdma_setup.sh

# helong BF2 (角色 C，下文会扩展脚本)
BF_ROLE=C /root/experiments/scripts/bf2_rdma_setup.sh
```

下面给出三节点版本的推荐 SF IP 分配：

| BF2 节点 | SF netdev | SF Representor | SF IP | Fabric IP (100G 端口) |
|---------|-----------|----------------|-------|----------------------|
| tianjin | enp3s0f1s0 | en3f1pf1sf0 | 192.168.56.102/24 | 192.168.56.2 |
| fujian | enp3s0f1s2 | en3f1pf1sf2 | 192.168.56.103/24 | 192.168.56.3 |
| helong | enp3s0f0s4 | en3f0pf0sf4 | 192.168.56.101/24 | 192.168.56.1 |

说明：
- SF IP 和 fabric IP 都在 `192.168.56.0/24` 子网中，便于 L2 互通。
- SF 用于 RDMA 数据通路；fabric IP 用于普通 IP 通信（gRPC、容器网络、VIP）。
- tianjin/fujian 使用 `p1` 作为上行口，helong 使用 `p0`（与现有 `config.sh` 一致）。

### 2.6 承载用户容器的网络命名空间

BF2 上的 Docker 容器使用 `--network host` 模式，直接复用 BF2 的网络栈。容器看到 `p0`/`p1` 即物理端口（经 OVS 桥接），可直接接收 VIP 流量。

---

## 3. 运行前验证（每次实验前执行）

### 3.1 一键验证脚本

```bash
# 在 tianjin 主机上
bash ~/experiments/scripts/bf2_cluster_verify.sh
```

该脚本会检查：
- 三主机之间 100G 连通性（ping + iperf）
- 三主机 → 本地 BF2 tmfifo 可达
- 三 BF2 之间 SF fabric IP 互通
- 三 BF2 之间 RoCE 硬件路径可用（ib_write_lat 短测）
- OVS 流表是否为硬件加速模式

脚本会把每项检查结果打印为 `[OK]` / `[FAIL]` 并写入 `~/exp_data/cluster_verify.log`。

### 3.2 手动分项验证

#### 3.2.1 主机-主机 100G

```bash
# tianjin → fujian
ping -c 3 -I enp94s0f1np1 192.168.56.11
# tianjin → helong
ping -c 3 -I enp94s0f1np1 192.168.56.12

# 带宽（可选，~5 秒）
iperf3 -c 192.168.56.11 -t 5
# 期望：≥ 90 Gbps
```

#### 3.2.2 主机-本地 BF2

```bash
ssh root@192.168.100.2 "hostname; uname -m"
# 期望：aarch64 + BF2 hostname
```

#### 3.2.3 BF2 SF RDMA 验证（核心）

在两端 BF2 上分别启动 `ib_write_lat`：

```bash
# server 端：tianjin BF2
ssh root@192.168.56.2 << 'EOF'
DEV=$(ls /sys/class/infiniband/ | head -1)
ib_write_lat -d ${DEV} -F -s 128 &
EOF

# client 端：fujian BF2（tianjin SF IP 作为目标）
ssh root@192.168.56.3 << 'EOF'
DEV=$(ls /sys/class/infiniband/ | head -1)
ib_write_lat -d ${DEV} -F -s 128 192.168.56.102
EOF
```

期望输出：
```
#bytes   #iterations    t_avg[usec]    t_99.9%[usec]
128      1000           ~3.4           <15
```

若 `t_avg` 超过 20 微秒或测试报错 `timeout`：
- 检查 OVS hw-offload 是否开启：`ovs-vsctl get Open_vSwitch . other_config:hw-offload`
- 检查 RoCE GID 是否存在：`show_gids` 应看到本机 SF IP 对应的 RoCE v2 GID
- 检查 SF 是否 UP：`ip link show <SF-netdev>` 应为 state UP

#### 3.2.4 BF2 间 IP 连通性

```bash
# 进入 tianjin BF2
ssh root@192.168.56.2
ping -c 3 192.168.56.3   # fujian BF2
ping -c 3 192.168.56.1   # helong BF2
```

#### 3.2.5 容器 VIP 可达性

```bash
# tianjin → fujian BF2 上的测试容器
ssh root@192.168.56.3 "docker run -d --rm --name test --network=host nginx:alpine"
curl -m 5 http://192.168.56.3/   # 直接访问 BF2 IP
ssh root@192.168.56.3 "ip addr add 192.168.56.200/24 dev p1"
curl -m 5 http://192.168.56.200/ # 通过 VIP 访问
ssh root@192.168.56.3 "ip addr del 192.168.56.200/24 dev p1; docker rm -f test"
```

---

## 4. 故障排查

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| ib_write_lat 报 `unable to create QP` | RoCE 未启用 | `mlxconfig -d ... set LINK_TYPE_P1=2`（RoCE 模式）|
| 延迟 > 20 us | 走了软件慢路径 | 检查 `ovs-appctl dpctl/dump-flows type=offloaded` 应有非零计数 |
| `ip a` 看不到 SF netdev | SF 未创建 | 重跑 `bf2_rdma_setup.sh`，检查 mlxdevm 是否支持 |
| `show_gids` 看不到 RoCE v2 GID | OVS NORMAL 流未下发 | `ovs-ofctl add-flow ovsbr1 "actions=NORMAL"` |
| 主机 ping 不通 BF2 fabric IP | ovsbr1 的 pf0hpf 端口 DOWN | `ip link set pf0hpf up` + `ip link set ovsbr1 up` |
| 跨 BF2 VIP 切换失败 | ARP 未更新 | 手动 `arping -c 3 -A -I p1 <VIP>`；检查交换机 MAC 表 |

---

## 5. 软件版本（已验证）

| 组件 | 版本 |
|------|------|
| 主机 OS | Ubuntu 22.04 LTS |
| BF2 OS | Ubuntu 20.04 LTS (DOCA 1.5.4003 镜像) |
| 主机 DOCA | 3.1.0 |
| BF2 固件 | 24.39.2048 或更新 |
| OVS | 2.17+ |
| Kernel 模块 | mlx5_core, mlx5_ib, rdma_cm, ib_uverbs, ib_core |

BF2 固件更新：
```bash
mlxfwreset -d /dev/mst/mt41686_pciconf0 reset
mlxfwmanager --query
# 若需升级：mlxfwmanager --online --update
```
