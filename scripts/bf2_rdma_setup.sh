#!/bin/bash
# BF-2 ARM RDMA 环境配置脚本
#
# 用途：在 BF-2 ARM 上恢复可工作的 NIC↔NIC RDMA 环境
# 前置条件：
#   - BF-2 处于 switchdev 模式
#   - OVS 已启用 hw-offload (other_config:hw-offload=true)
#   - 物理端口 p1 处于 UP 状态
#
# 使用方法：
#   在 BF-A 上：BF_ROLE=A ./bf2_rdma_setup.sh
#   在 BF-B 上：BF_ROLE=B ./bf2_rdma_setup.sh
#
# 注意：节点角色（A/B）仅影响 IP 地址分配，不影响其他配置

set -e

# ========== 节点配置 ==========
BF_ROLE="${BF_ROLE:-A}"

if [ "$BF_ROLE" = "A" ]; then
  # tianjin BF2 (master)
  SF_NETDEV="enp3s0f1s0"
  SF_REPRESENTOR="en3f1pf1sf0"
  SF_IP="192.168.56.102/24"
  RDMA_DEV_HINT="mlx5_3"
  UPLINK_PORT="p1"
elif [ "$BF_ROLE" = "B" ]; then
  # fujian BF2 (worker 1)
  SF_NETDEV="enp3s0f1s2"
  SF_REPRESENTOR="en3f1pf1sf2"
  SF_IP="192.168.56.103/24"
  RDMA_DEV_HINT="mlx5_2"
  UPLINK_PORT="p1"
elif [ "$BF_ROLE" = "C" ]; then
  # helong BF2 (worker 2) — uses p0 uplink instead of p1
  SF_NETDEV="enp3s0f0s4"
  SF_REPRESENTOR="en3f0pf0sf4"
  SF_IP="192.168.56.101/24"
  RDMA_DEV_HINT="mlx5_0"
  UPLINK_PORT="p0"
else
  echo "Error: BF_ROLE must be A (tianjin-BF2), B (fujian-BF2), or C (helong-BF2)"
  exit 1
fi

echo "=========================================="
echo "BF-2 ARM RDMA 环境配置"
echo "  角色:         $BF_ROLE"
echo "  SF netdev:    $SF_NETDEV"
echo "  SF 代表口:    $SF_REPRESENTOR"
echo "  SF IP:        $SF_IP"
echo "  上行物理口:   $UPLINK_PORT"
echo "  预期 RDMA 设备: $RDMA_DEV_HINT"
echo "=========================================="

# ========== 步骤 1：验证 OVS hw-offload ==========
echo
echo "[1/5] 验证 OVS hw-offload 状态..."
HW_OFFLOAD=$(ovs-vsctl get Open_vSwitch . other_config:hw-offload 2>/dev/null || echo "")
if [ "$HW_OFFLOAD" = '"true"' ]; then
  echo "    OK: hw-offload 已启用"
else
  echo "    警告: hw-offload 未启用，正在启用..."
  ovs-vsctl set Open_vSwitch . other_config:hw-offload=true
  echo "    已启用，但需要重启 OVS 服务才能生效："
  echo "      systemctl restart openvswitch-switch"
  echo "    请手动重启后重新运行此脚本"
  exit 1
fi

# ========== 步骤 2：配置 OVS 流表（NORMAL 模式）==========
echo
echo "[2/5] 配置 OVS 流表..."
ovs-ofctl del-flows ovsbr1
ovs-ofctl add-flow ovsbr1 "actions=NORMAL"
echo "    已安装 NORMAL 流表（基于 MAC 学习的硬件 offload）"

# ========== 步骤 3：添加 SF representor 到 ovsbr1 ==========
echo
echo "[3/5] 将 SF 代表口加入 ovsbr1..."
if ovs-vsctl list-ports ovsbr1 | grep -q "^${SF_REPRESENTOR}$"; then
  echo "    OK: $SF_REPRESENTOR 已在 ovsbr1 中"
else
  ovs-vsctl add-port ovsbr1 $SF_REPRESENTOR
  echo "    已添加 $SF_REPRESENTOR 到 ovsbr1"
fi

# ========== 步骤 4：配置 SF netdev IP ==========
echo
echo "[4/5] 配置 SF netdev IP..."
if ip addr show $SF_NETDEV | grep -q "$(echo $SF_IP | cut -d/ -f1)"; then
  echo "    OK: $SF_NETDEV 已配置 $SF_IP"
else
  ip addr add $SF_IP dev $SF_NETDEV 2>/dev/null || true
  echo "    已配置 $SF_NETDEV → $SF_IP"
fi
ip link set $SF_NETDEV up

# ========== 步骤 5：验证 RDMA 环境 ==========
echo
echo "[5/5] 验证 RDMA 环境..."

# 动态检测 SF 对应的 RDMA 设备
DETECTED_DEV=""
for dev in /sys/class/infiniband/mlx5_*; do
  devname=$(basename $dev)
  if [ -d "$dev/device/net/$SF_NETDEV" ]; then
    DETECTED_DEV=$devname
    break
  fi
done

if [ -z "$DETECTED_DEV" ]; then
  echo "    错误：找不到 $SF_NETDEV 对应的 RDMA 设备"
  exit 1
fi
echo "    $SF_NETDEV → $DETECTED_DEV"

# 验证 GID
SF_IPV4=$(echo $SF_IP | cut -d/ -f1)
GID_FOUND=0
for i in $(seq 0 15); do
  gid=$(cat /sys/class/infiniband/$DETECTED_DEV/ports/1/gids/$i 2>/dev/null || echo "")
  if [ -n "$gid" ] && [ "$gid" != "0000:0000:0000:0000:0000:0000:0000:0000" ]; then
    ndev=$(cat /sys/class/infiniband/$DETECTED_DEV/ports/1/gid_attrs/ndevs/$i 2>/dev/null || echo "")
    if [ "$ndev" = "$SF_NETDEV" ]; then
      GID_FOUND=1
    fi
  fi
done

if [ $GID_FOUND -eq 1 ]; then
  echo "    OK: $DETECTED_DEV 已创建 RoCE GID (from $SF_NETDEV)"
else
  echo "    警告：未找到预期的 RoCE GID，可能需要重启 netdev"
fi

echo
echo "=========================================="
echo "配置完成。建议执行以下验证："
echo "  show_gids                    # 确认 GID 存在"
echo "  ovs-vsctl show               # 确认 OVS 拓扑"
echo "  ovs-ofctl dump-flows ovsbr1  # 确认流表"
echo
echo "RDMA 测试命令："
echo "  # 作为 server:"
echo "  ib_write_bw -d $DETECTED_DEV -F"
echo "  ib_read_lat -d $DETECTED_DEV -F"
echo
echo "  # 作为 client (连接对端):"
echo "  ib_write_bw -d $DETECTED_DEV -F <远端 SF IP>"
echo "  ib_read_lat -d $DETECTED_DEV -F <远端 SF IP>"
echo "=========================================="
