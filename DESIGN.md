# Cluster Management System Design Document

Version: 2.0 (Chapter 3 redesign)
Date: 2026-04-01

## 1. Overview

This document describes the redesigned cluster management system for Chapter 3
of the thesis. The key change from Chapter 2: the SmartNIC evolves from a
**stateless relay** (forward_routine) into an **intelligent management agent**
(slave_agent) that handles registration, heartbeats, dual-domain monitoring,
and fault detection.

### Architecture Change

```
Chapter 2 (tunnel layer):
  Host metric_push → Comch → BF2 forward_routine → TCP → master_monitor
  (BF2 is a dumb relay; no state, no protocol logic)

Chapter 3 (management layer):
  Host metric_push → Comch → BF2 slave_agent → gRPC → cluster_master
                                    ↑ BF2 self-monitoring    ↑ primary-standby
  Host metric_push ··fallback gRPC (direct)··→ cluster_master
  (BF2 is the management agent; handles registration, heartbeat, fault detection)
```

### Comparison with Existing Systems

| Dimension | Kubernetes | Ceph | This System |
|-----------|-----------|------|-------------|
| Agent location | kubelet on host | OSD on host | slave_agent on SmartNIC |
| Heartbeat | Lease object (10s) | OSD peer (6s) | gRPC stream (3s) |
| Fault detection | 40s grace + 5min eviction | 20s grace + Reporter | 15s suspect + 45s offline |
| Heterogeneous | Device Plugin (passive) | Device Class | Native dual-domain monitoring |
| Degradation | Node-level NotReady | OSD-level down | Per-domain + fallback channel |
| Host CPU impact | kubelet consumes CPU | OSD consumes CPU | Zero host CPU for management |

## 2. Components

### 2.1 cluster_master (Master Host)

gRPC C++ server replacing the legacy master_monitor.

**Responsibilities:**
- Node registry with state machine (Online/Suspect/Offline)
- Heartbeat monitoring with configurable thresholds
- Metric aggregation → TimescaleDB (host_metrics, bf2_metrics tables)
- Self-monitoring (CPU, memory, disk)
- HTTP JSON status endpoint (port 8080)
- Event logging to cluster_events table

**gRPC Services:**
- `ClusterControl.NodeSession` — bidirectional stream for slave_agents
- `ClusterControl.DirectPush` — unary RPC for metric_push fallback
- `MasterHealth.Ping` — health check for master_watchdog

### 2.2 slave_agent (Worker BF2)

Intelligent management agent running on each worker's BF2 ARM processor.

**Responsibilities:**
- Register with cluster_master using BF2 hardware UUID
- Receive host metrics from metric_push via Comch (PCIe kernel-bypass)
- Collect BF2 metrics locally (/proc/stat, thermal, network stats, OVS)
- Send heartbeats with dual-domain status (host_status + bf2_status)
- Detect host failure (Comch timeout → host_status=UNREACHABLE)
- Auto-reconnect to master with exponential backoff

**Key design decision:** slave_agent runs on BF2 (not host) because:
1. Zero host CPU interference (consistent with Chapter 2 thesis)
2. Native dual-domain visibility (host via Comch + BF2 local)
3. Independent fault detection (BF2 detects host Comch timeout)
4. Single process replaces forward_routine + slave_monitor

### 2.3 metric_push (Worker Host)

Lightweight metric collector, same as Chapter 2 with added fallback.

**Normal mode (Comch):** Read /proc, send binary message via Comch to BF2.
~65us per iteration, negligible CPU impact.

**Fallback mode (gRPC direct):** When Comch fails 5 consecutive times,
auto-switch to direct gRPC call to cluster_master. Periodically attempt
Comch recovery. Switch back when Comch recovers.

### 2.4 master_watchdog (Master BF2)

Monitors cluster_master process health from the master's own BF2.

**Detection:** Comch health pings + gRPC Health check, every 3s.
**Recovery:** Auto-restart cluster_master (3 retries). If all fail,
signal standby node via gRPC FailoverControl.

### 2.5 TimescaleDB (Master Host)

Time-series database for cluster state persistence.

**Tables:**
- `node_registry` — current node state (relational, non-time-series)
- `host_metrics` — host CPU/memory/network hypertable
- `bf2_metrics` — BF2 ARM CPU/temp/port stats hypertable
- `cluster_events` — audit log hypertable

## 3. Communication Protocols

### 3.1 Host ↔ BF2: Comch Binary Protocol (unchanged from Chapter 2)

```
┌──────────┬──────────┬──────────┬──────────┐
│ Magic(4) │ Type(2)  │ Flags(2) │ Seq(4)   │  Header: 16 bytes
├──────────┴──────────┴──────────┴──────────┤
│           PayloadLen(4)                    │
├────────────────────────────────────────────┤
│           Payload (0-4064 bytes)           │
└────────────────────────────────────────────┘
Magic = 0xBEEF1234
Types: REGISTER(1), HEARTBEAT(3), RESOURCE_REPORT(5), etc.
Max message: 4080 bytes (Comch hardware limit)
```

### 3.2 BF2 ↔ Master: gRPC over TCP/100G (new)

Uses Protocol Buffers + HTTP/2 bidirectional streaming.

**NodeSession flow:**
```
slave_agent                          cluster_master
    │                                      │
    │──── RegisterRequest ────────────────>│
    │<─── RegisterAck ────────────────────│
    │                                      │
    │──── HeartbeatPing ──────────────────>│
    │<─── HeartbeatAck ───────────────────│
    │                                      │
    │──── ResourceReport (host metrics) ──>│
    │<─── ReportAck ──────────────────────│
    │                                      │
    │──── BF2MetricsReport ───────────────>│
    │                                      │
    │──── StatusChangeNotice ─────────────>│  (host_status changed)
    │                                      │
    │<─── NodeCommand ────────────────────│  (admin command)
    │──── CommandResult ──────────────────>│
    │                                      │
    │──── DeregisterNotice ───────────────>│
    │                                      │
```

**gRPC Keepalive Configuration:**
- Client: KEEPALIVE_TIME=10s, KEEPALIVE_TIMEOUT=5s, PERMIT_WITHOUT_CALLS=true
- Server: MIN_RECV_PING_INTERVAL=5s, MAX_PING_STRIKES=2

## 4. Node State Machine

```
                     register(uuid)
   ┌──────────────┐ ──────────────> ┌──────────┐
   │ Unregistered  │                 │  Online   │◄──────────┐
   └──────────────┘                  └──────────┘            │
                                          │                  │
                              no heartbeat│                  │ heartbeat
                              for 15s     │                  │ received
                                          ▼                  │
                                     ┌──────────┐           │
                                     │ Suspect   │───────────┘
                                     └──────────┘
                                          │
                              no heartbeat│
                              for 45s     │
                                          ▼
                                     ┌──────────┐  re-register
                                     │ Offline   │────────────> Online
                                     └──────────┘  (same uuid)
```

**Parameters:**
| Parameter | Value | Rationale |
|-----------|-------|-----------|
| heartbeat_interval | 3s | Dedicated 100G network, stable latency |
| suspect_threshold | 15s (5x) | Fast detection, low false positive rate |
| offline_threshold | 45s (15x) | Confirmed failure, trigger recovery |
| check_period | 1s | Master polling frequency |

**Dual-domain status (unique to this system):**

| node_state | host_domain | bf2_domain | Meaning |
|------------|-------------|------------|---------|
| Online | OK | OK | Normal operation |
| Online | Unreachable | OK | PCIe/Comch fault, BF2 still reporting |
| Online | OK | Degraded | BF2 thermal/resource pressure |
| Suspect | - | - | Full node heartbeat timeout |
| Offline | - | Offline | Confirmed node failure |
| Online | OK | N/A | Degraded mode: metric_push direct to master |

## 5. Fault Recovery

### Scenario 1: slave_agent crash (BF2 process dies)
- Detection: 15s (suspect_threshold)
- Recovery: systemd restart → re-register with same UUID → Online
- Total: ~5s (restart + register)

### Scenario 2: Comch/PCIe link failure
- Detection: 3s (slave_agent Comch timeout)
- Action: slave_agent reports host_status=UNREACHABLE
- Fallback: metric_push switches to direct gRPC (~4s)
- Control plane interruption: ~4s

### Scenario 3: Full node failure (power loss)
- Detection: 45s (offline_threshold)
- BF2 powered via PCIe → also loses power
- Recovery: on reboot, slave_agent re-registers

### Scenario 4: 100G network partition
- Detection: slave_agent detects TCP disconnect immediately
- Action: exponential backoff reconnection (3s, 6s, 12s, max 30s)
- Buffer: up to 100 heartbeats locally (~5 minutes)
- Fallback: try 1G management network if available

### Scenario 5: cluster_master crash
- Detection: master_watchdog (Comch + gRPC) within 3-15s
- Recovery: watchdog restarts master (3 retries)
- If hardware failure: failover to standby node
- slave_agents: reconnect with exponential backoff

## 6. Database Schema

```sql
-- Node registry (mutable state, non-time-series)
CREATE TABLE IF NOT EXISTS node_registry (
    node_uuid    TEXT PRIMARY KEY,
    hostname     TEXT NOT NULL,
    pci_bus_id   TEXT,
    state        TEXT NOT NULL DEFAULT 'offline',
    host_status  TEXT DEFAULT 'unknown',
    bf2_status   TEXT DEFAULT 'unknown',
    registered_at TIMESTAMPTZ DEFAULT NOW(),
    last_seen    TIMESTAMPTZ DEFAULT NOW()
);

-- Host metrics (time-series hypertable)
CREATE TABLE IF NOT EXISTS host_metrics (
    time         TIMESTAMPTZ NOT NULL,
    node_uuid    TEXT NOT NULL,
    cpu_pct      REAL,
    mem_total_kb BIGINT,
    mem_avail_kb BIGINT,
    net_rx_bytes BIGINT,
    net_tx_bytes BIGINT
);
SELECT create_hypertable('host_metrics', 'time', if_not_exists => TRUE);

-- BF2 metrics (time-series hypertable)
CREATE TABLE IF NOT EXISTS bf2_metrics (
    time           TIMESTAMPTZ NOT NULL,
    node_uuid      TEXT NOT NULL,
    arm_cpu_pct    REAL,
    arm_mem_total_kb BIGINT,
    arm_mem_avail_kb BIGINT,
    temperature_c  REAL,
    port_rx_bytes  BIGINT,
    port_tx_bytes  BIGINT,
    port_rx_drops  BIGINT,
    ovs_flow_count INT
);
SELECT create_hypertable('bf2_metrics', 'time', if_not_exists => TRUE);

-- Cluster events (audit log hypertable)
CREATE TABLE IF NOT EXISTS cluster_events (
    time       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    node_uuid  TEXT,
    event_type TEXT NOT NULL,
    detail     TEXT
);
SELECT create_hypertable('cluster_events', 'time', if_not_exists => TRUE);

-- Compression policies (>1 day)
SELECT add_compression_policy('host_metrics', INTERVAL '7 days');
SELECT add_compression_policy('bf2_metrics', INTERVAL '7 days');

-- Retention policies (>90 days)
SELECT add_retention_policy('host_metrics', INTERVAL '90 days');
SELECT add_retention_policy('bf2_metrics', INTERVAL '90 days');
SELECT add_retention_policy('cluster_events', INTERVAL '90 days');
```

## 7. Build System

CMake-based build replacing per-directory Makefiles (legacy Makefiles preserved).

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# On BF2 ARM (native compilation):
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCOMCH_NIC_DOCA_VER=15
cmake --build build --target slave_agent master_watchdog
```

**Dependencies:**
- gRPC++ (libgrpc++-dev) — both x86 and aarch64
- protobuf (libprotobuf-dev) — both architectures
- PostgreSQL (libpq-dev) — master only
- DOCA SDK — conditional, for Comch-enabled builds

## 8. Deployment

```
Master (tianjin):
  Host:  cluster_master --grpc-port=50051 --http-port=8080 --db-connstr=...
  BF2:   master_watchdog --master-grpc-addr=192.168.100.1:50051

Worker (fujian, helong):
  Host:  metric_push_v2 --pci=0000:5e:00.0 --master-addr=192.168.56.10:50051
  BF2:   slave_agent --node-uuid=<auto> --master-addr=192.168.56.10:50051
```

## 9. File Inventory

| File | Language | Lines | Purpose |
|------|----------|-------|---------|
| proto/cluster.proto | Protobuf | 190 | Service + message definitions |
| common/node_state.h | C | 95 | State machine + domain types |
| CMakeLists.txt | CMake | 65 | Top-level build |
| proto/CMakeLists.txt | CMake | 30 | Proto code generation |
| control-plane/master/cluster_master.cc | C++ | 250 | Master entry point |
| control-plane/master/grpc_service.{h,cc} | C++ | 260 | gRPC service impl |
| control-plane/master/node_registry.{h,cc} | C++ | 240 | Node state management |
| control-plane/master/http_status.{h,cc} | C++ | 145 | HTTP JSON status |
| control-plane/master/db.{h,cc} | C/C++ | 450 | TimescaleDB interface |
| control-plane/slave/slave_agent.cc | C++ | 350 | BF2 agent entry point |
| control-plane/slave/bf2_collector.{h,cc} | C++ | 225 | BF2 metric collection |
| control-plane/slave/host_collector.{h,cc} | C++ | 160 | Comch host metric proxy |
| control-plane/watchdog/master_watchdog.cc | C++ | 200 | Master health monitor |
| bench/metric_push/metric_push.cc | C++ | 200 | Host metric push + fallback |
| bench/mock_slave/mock_slave_grpc.cc | C++ | 250 | Scalability testing |
