# SmartNIC-Based Heterogeneous Cluster System — Design Document

Version: 3.0 (post-experiment, reflects deployed code)
Date: 2026-04-02

---

## 1. System Overview

This system manages heterogeneous clusters where each server is equipped with
an NVIDIA BlueField-2 SmartNIC. The architecture spans three layers, each
corresponding to a thesis chapter:

| Layer | Chapter | Purpose | Status |
|-------|---------|---------|--------|
| High-speed offload tunnel | Ch.2 | PCIe kernel-bypass (Comch) between host and BF2 | **Complete** |
| Cluster management | Ch.3 | Node lifecycle, fault recovery, state persistence | **Complete** |
| Workload orchestration | Ch.4 | Classification + dynamic placement on SmartNIC | **Not started** |

### Architecture (Chapters 2+3, deployed)

```
┌─────────── Master Node (tianjin) ──────────────────────┐
│  Host:                                                  │
│    cluster_master (:50051 gRPC, :8080 HTTP)             │
│         │                                               │
│         ├── NodeSession (bidi stream per node)          │
│         ├── DirectPush (fallback unary RPC)             │
│         ├── DbWriter (async batch, 4-conn pool)        │
│         └──→ TimescaleDB (node_registry, host_metrics,  │
│              bf2_metrics, cluster_events)               │
│                                                         │
│  BF2:   master_watchdog (Comch + gRPC health check)     │
└─────────────────────────────────────────────────────────┘
                    ▲ gRPC (100G, 192.168.56.x)
         ┌──────────┼──────────┐
         │          │          │
  ┌──────┴───┐ ┌────┴─────┐
  │fujian    │ │helong    │
  │          │ │          │
  │ BF2:     │ │ BF2:     │
  │ slave_   │ │ slave_   │   ← intelligent management agent
  │ agent    │ │ agent    │   (registration, heartbeat, dual-domain
  │    ▲     │ │    ▲     │    monitoring, fault detection)
  │    │Comch│ │    │Comch│
  │ Host:    │ │ Host:    │
  │ metric_  │ │ metric_  │   ← lightweight /proc collector
  │ push     │ │ push     │   (Comch primary, gRPC fallback)
  └──────────┘ └──────────┘
```

### Key Design Decisions

1. **Management agent on BF2, not host** — zero host CPU interference,
   native dual-domain visibility, independent fault detection
2. **gRPC replaces raw TCP** — built-in keepalive, bidirectional streaming,
   protobuf serialization, instant stream-close detection
3. **Async batch DB writer** — decouples gRPC latency from DB writes,
   3.8x throughput improvement at 256 nodes
4. **Comch→gRPC fallback** — metric_push auto-switches transport when
   SmartNIC fails, zero metric data loss

---

## 2. Components

### 2.1 cluster_master

**Location:** Master host (tianjin)
**Binary:** `build/control-plane/master/cluster_master`
**Source:** `cluster_master.cc` (280 lines)

gRPC C++ server. Entry point creates NodeRegistry, connects to TimescaleDB,
starts DbWriter (4-connection pool), registers gRPC services, spawns watchdog
thread (1s state transition sweep), HTTP status server, and signal handler.

**gRPC Services:**
- `ClusterControl.NodeSession` — bidi stream per slave_agent
- `ClusterControl.DirectPush` — unary RPC for metric_push fallback
- `MasterHealth.Ping` — health probe for master_watchdog

**Key runtime components:**
- `NodeRegistry` (node_registry.{h,cc}, 333 lines) — thread-safe
  `unordered_map<uuid, NodeEntry>`, state machine transitions with
  lock-free callback dispatch
- `DbWriter` (db_writer.{h,cc}, 493 lines) — spinlock MPSC queue,
  vector-swap drain, multi-row INSERT batching, 4-connection pool
- `grpc_service` (grpc_service.{h,cc}, 438 lines) — NodeSession handler
  with ACK-before-DB pattern, db_mu_ mutex for sync DB calls
- `db` (db.{h,cc}, 641 lines) — libpq wrapper with auto-reconnect,
  schema v2 (4 tables), parameterized queries

### 2.2 slave_agent

**Location:** Worker BF2 ARM (fujian, helong)
**Binary:** `build/control-plane/slave/slave_agent`
**Source:** `slave_agent.cc` (585 lines)

Core innovation: SmartNIC is the management agent, not a relay.

**Sub-components:**
- `HostCollector` (host_collector.{h,cc}, 306 lines) — Comch NIC-side
  receiver thread, parses binary protocol.h messages from metric_push,
  exposes thread-safe `getLatest()` and `isAlive()`
- `BF2Collector` (bf2_collector.{h,cc}, 208 lines) — reads local
  /proc/stat, /proc/meminfo, /sys/class/thermal, /sys/class/net/p0,
  `ovs-dpctl dump-flows`

**Main loop:** register → heartbeat/report cycle with three independent
timers (heartbeat 3s, host report 3s, BF2 report 5s) → StatusChangeNotice
on domain transitions → exponential backoff reconnect on failure.

### 2.3 metric_push

**Location:** Worker host (fujian, helong)
**Binary:** `build/bench/metric_push/metric_push`
**Source:** `metric_push.cc` (299 lines)

Dual-mode metric collector:
- **Comch mode:** read /proc → build binary msg → comch_host_send() (~65µs)
- **gRPC mode:** read /proc → build protobuf → DirectPush() unary RPC
- Auto-switch after 5 consecutive Comch failures; retry Comch every 30s

### 2.4 master_watchdog

**Location:** Master BF2 ARM (tianjin)
**Binary:** `build/control-plane/watchdog/master_watchdog`
**Source:** `master_watchdog.cc` (306 lines)

Dual-channel health monitor:
- Comch: MSG_HEARTBEAT to cluster_master, 5 failures = unhealthy
- gRPC: MasterHealth.Ping(), 10 failures = failed
- Recovery: `systemctl restart cluster_master` (3 retries)

### 2.5 mock_slave

**Location:** Any host (test tool)
**Binary:** `build/bench/mock_slave/mock_slave`
**Source:** `mock_slave.cc` (372 lines)

Scalability test: N threads, each opens NodeSession bidi stream,
sends heartbeat + ResourceReport at configurable interval. Measures
registration latency, report-to-ACK latency, errors.

---

## 3. Communication Protocols

### 3.1 Host ↔ BF2: Binary over Comch (Chapter 2)

```
┌──────────┬──────────┬──────────┬──────────┐
│ Magic(4) │ Type(2)  │ Flags(2) │ Seq(4)   │  Header: 16 bytes
├──────────┴──────────┴──────────┴──────────┤
│           PayloadLen(4)                    │
├────────────────────────────────────────────┤
│           Payload (0-4064 bytes)           │
└────────────────────────────────────────────┘
Magic = 0xBEEF1234
Max message: 4080 bytes (BF2 DOCA 1.5 hardware limit)
```

Message types: REGISTER(1), REGISTER_ACK(2), HEARTBEAT(3),
HEARTBEAT_ACK(4), RESOURCE_REPORT(5), COMMAND(6), COMMAND_ACK(7),
DEREGISTER(8), BF2_REPORT(9), STATUS_CHANGE(10), DEREGISTER_NOTICE(11).

### 3.2 BF2 ↔ Master: gRPC over TCP/100G (Chapter 3)

Defined in `proto/cluster.proto` (240 lines). Key services:

```protobuf
service ClusterControl {
  rpc NodeSession(stream NodeMessage) returns (stream MasterMessage);
  rpc DirectPush(DirectPushRequest) returns (DirectPushResponse);
}
service MasterHealth {
  rpc Ping(HealthPingRequest) returns (HealthPingResponse);
}
service FailoverControl {
  rpc TriggerFailover(FailoverRequest) returns (FailoverResponse);
}
```

**NodeSession message flow:**
```
slave_agent                          cluster_master
    │──── RegisterRequest ────────────────>│
    │<─── RegisterAck ────────────────────│
    │                                      │
    │──── HeartbeatPing ──────────────────>│  (every 3s)
    │<─── HeartbeatAck ───────────────────│
    │──── ResourceReport ─────────────────>│  (every 3s)
    │<─── ReportAck ──────────────────────│
    │──── BF2MetricsReport ───────────────>│  (every 5s)
    │──── StatusChangeNotice ─────────────>│  (on domain change)
    │──── DeregisterNotice ───────────────>│  (graceful shutdown)
```

**Keepalive:** Client KEEPALIVE_TIME=10s, TIMEOUT=5s; Server
MIN_RECV_PING_INTERVAL=5s.

---

## 4. Node State Machine

```
Unregistered ──register(uuid)──> Online ◄──heartbeat──┐
                                   │                   │
                         no HB 15s │                   │
                                   ▼                   │
                                Suspect ───────────────┘
                                   │
                         no HB 45s │
                                   ▼
                                Offline ──re-register──> Online
                                           (same uuid)
```

**Dual-domain status:** Each node has independent `host_status` (OK /
Degraded / Unreachable) and `bf2_status` (OK / Degraded / Offline),
enabling fine-grained fault diagnosis beyond K8s's single Ready/NotReady.

---

## 5. Fault Recovery (Validated by Experiment D)

| Scenario | Detection | Recovery | Validated |
|----------|-----------|----------|-----------|
| slave_agent crash | 2.5s (gRPC stream close) | 6.9s total | 5 runs, σ=160ms |
| Comch/PCIe failure | 5.3s (metric_push fallback) | 9.6s Comch restore | 5 runs |
| cluster_master crash | 2.0s (restart) | 3.1s full reconnect | 5 runs, σ<2ms |
| Full node power loss | 45s (offline threshold) | re-register on boot | by design |
| 100G network partition | immediate TCP disconnect | exp. backoff 3-30s | by design |

---

## 6. Database Schema & Performance (Validated by Experiment E)

**Tables:** node_registry (relational), host_metrics (hypertable),
bf2_metrics (hypertable), cluster_events (hypertable).

**Write performance (async batch writer):**

| Nodes | Sync (rows/s) | Async (rows/s) | Improvement | Latency |
|-------|---------------|----------------|-------------|---------|
| 64 | 44 | 60 | 1.4x | 10.1ms |
| 128 | 51 | 118 | 2.3x | 10.1ms |
| 256 | 60 | 227 | 3.8x | 11.0ms |

**Query performance:** Status query 5.9ms, 5-min aggregation 10.8ms,
1-hour time_bucket 13.1ms. Compression ratio: 4.6:1.

---

## 7. Build System

CMake with `-DBUILD_TARGET=HOST|BF2` flag:

```bash
# Host (x86): cluster_master, metric_push, mock_slave
cmake -B build -DBUILD_TARGET=HOST -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# BF2 (ARM): slave_agent, master_watchdog
cmake -B build -DBUILD_TARGET=BF2 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**Dependencies:** gRPC++ (libgrpc++-dev), protobuf, PostgreSQL (libpq-dev,
host only), DOCA SDK (Comch).

Comch static library (`comch_c`) is built from `tunnel/host/comch_host.c`
(HOST) or `tunnel/nic/comch_nic.c` (BF2) with version-appropriate DOCA
linking (3.1 libdoca_comch for host, 1.5 libdoca_comm_channel for BF2).

---

## 8. File Inventory (current, 2026-04-02)

| File | Lines | Purpose |
|------|-------|---------|
| `proto/cluster.proto` | 240 | gRPC service + message definitions |
| `common/node_state.h` | 109 | State machine enums + transitions |
| `common/protocol.h` | 167 | Binary Comch protocol (Ch.2) |
| `common/timing.h` | — | Nanosecond timing utilities (Ch.2) |
| `CMakeLists.txt` | 157 | Top-level build (BUILD_TARGET, DOCA, gRPC) |
| `proto/CMakeLists.txt` | 57 | Protobuf/gRPC code generation |
| **cluster_master** | | |
| `cluster_master.cc` | 280 | Main entry point |
| `grpc_service.{h,cc}` | 438 | NodeSession + DirectPush + Health |
| `node_registry.{h,cc}` | 333 | Thread-safe node state management |
| `db_writer.{h,cc}` | 493 | Async batch writer (spinlock + pool) |
| `db.{h,cc}` | 641 | libpq wrapper, schema v2 |
| `http_status.{h,cc}` | 192 | HTTP JSON status server |
| **slave_agent** | | |
| `slave_agent.cc` | 585 | BF2 management agent main |
| `bf2_collector.{h,cc}` | 208 | Local ARM metric collection |
| `host_collector.{h,cc}` | 306 | Comch receiver for host metrics |
| **auxiliary** | | |
| `master_watchdog.cc` | 306 | Master health monitor (BF2) |
| `metric_push.cc` | 299 | Host collector + gRPC fallback |
| `mock_slave.cc` | 372 | Scalability test tool |
| **Total** | **~5,200** | |

---

## 9. Deployment Topology

```
                172.28.4.x management LAN (eno1, 1G)
    ┌─────────────────┬─────────────────┬─────────────────┐
    │  tianjin .75    │  fujian .77     │  helong .85     │
    │  MASTER         │  WORKER         │  WORKER         │
    │  cluster_master │  metric_push    │  metric_push    │
    │  TimescaleDB    │                 │                 │
    │                 │                 │                 │
    │  BF2: watchdog  │  BF2: slave_    │  BF2: slave_    │
    │       (.56.2)   │  agent (.56.3)  │  agent (.56.1)  │
    └────────┬────────┘────────┬────────┘────────┬────────┘
             └─────────────────┴─────────────────┘
                  192.168.56.0/24 (100G switch)
```

---

## 10. Chapter 4 — Workload Orchestration (TODO)

### Not yet implemented. Planned components:

1. **Workload classifier** — rule-based or ML model to categorize
   workloads as I/O-intensive, compute-intensive, or lightweight
2. **Scheduling engine** — queries cluster_master's TimescaleDB for
   real-time metrics, decides placement (host vs BF2)
3. **Migration trigger** — monitors LLC miss rate and memory bandwidth,
   triggers container migration when interference threshold exceeded
4. **BF2 container runtime** — lightweight container environment on
   BF2 ARM for running Nginx, PostgreSQL, FaaS functions

### Planned experiments (F-I):
- F: Workload feature profiling (x86 vs ARM performance comparison)
- G: Interference quantification (GEMM + Nginx co-location)
- H: SmartNIC workload execution performance
- I: Orchestration strategy comparison (static vs dynamic)
