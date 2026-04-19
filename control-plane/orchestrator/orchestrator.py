#!/usr/bin/env python3
"""
orchestrator.py — Cluster-level workload orchestration daemon.

Runs on the master node. Monitors all worker nodes via TimescaleDB,
detects LLC interference using a sliding window, identifies I/O-intensive
workloads via four-metric joint evaluation, selects the optimal target
SmartNIC (local-first, cross-node fallback), and executes blue-green
migration with floating VIP.

Usage:
    python3 orchestrator.py \
        --db-connstr="host=localhost dbname=cluster_metrics user=postgres password=postgres" \
        --poll-interval=5 \
        --llc-threshold=2.0

Algorithm (4-phase, each poll cycle):
    Phase 1: Interference detection   — sliding-window avg LLC miss rate
    Phase 2: Source identification     — ctx_switch + net_io + cpu_usage
    Phase 3: Target SmartNIC selection — local-first, load-balanced
    Phase 4: Blue-green migration      — new container → health check → VIP switch → cleanup
"""

import argparse
import collections
import json
import logging
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from typing import Deque, Dict, List, Optional, Set, Tuple

try:
    import psycopg2
    import psycopg2.extras
except ImportError:
    psycopg2 = None
    print("WARNING: psycopg2 not installed, DB queries disabled", file=sys.stderr)

try:
    from agent_client import AgentClient, GRPC_AVAILABLE as _AGENT_AVAILABLE
except ImportError:
    AgentClient = None  # type: ignore
    _AGENT_AVAILABLE = False

# ---------------------------------------------------------------------------
# Configuration data classes
# ---------------------------------------------------------------------------

@dataclass
class WorkloadConfig:
    """Describes a migratable workload."""
    name: str                    # e.g. "nginx"
    image: str                   # Docker image (arm64-compatible)
    host_port: int               # Port the workload listens on
    health_path: str = "/"       # HTTP health check path
    health_timeout: int = 30     # Max seconds to wait for health check
    docker_args: str = ""        # Extra docker run arguments
    mem_req_mb: int = 256        # Estimated memory requirement (MB)


@dataclass
class SmartNICConfig:
    """Describes a SmartNIC that can host workloads."""
    nic_id: str                  # Unique ID, e.g. "fujian-bf2"
    bf2_ip: str                  # BF2 100G fabric IP
    bf2_ssh: str                 # SSH target (two-hop: user@host>>root@bf2)
    bf2_iface: str               # BF2 100G interface name (e.g. p1)
    parent_host: str             # node_uuid of the host this NIC belongs to


@dataclass
class NodeConfig:
    """Describes a worker host node."""
    node_uuid: str               # Cluster node ID
    host_ip: str                 # Host 100G IP
    host_ssh: str                # SSH target for host
    host_iface: str              # Host 100G interface name
    local_nic: str               # nic_id of the local SmartNIC


@dataclass
class ThresholdConfig:
    """Scheduling algorithm thresholds."""
    llc_factor: float = 2.0      # θ_llc = baseline × factor
    ctx_switch_min: float = 10000.0   # θ_csw: ctx switches/sec
    net_io_min: float = 1_000_000.0   # θ_io: bytes/sec
    cpu_max: float = 50.0        # θ_cpu: % (workloads above this stay on host)
    nic_cpu_max: float = 80.0    # θ_nic: SmartNIC CPU% cap
    nic_mem_reserve_mb: int = 2048  # Min free memory on SmartNIC (MB)


@dataclass
class OrchestratorConfig:
    db_connstr: str = "host=localhost dbname=cluster_metrics user=postgres password=postgres sslmode=disable"
    poll_interval: float = 5.0
    window_size: int = 6         # Sliding window: 6 samples × 5s = 30s
    perf_duration: float = 2.0
    thresholds: ThresholdConfig = field(default_factory=ThresholdConfig)
    nodes: Dict[str, NodeConfig] = field(default_factory=dict)
    nics: Dict[str, SmartNICConfig] = field(default_factory=dict)
    workloads: Dict[str, WorkloadConfig] = field(default_factory=dict)
    vips: Dict[str, str] = field(default_factory=dict)  # workload_name → VIP address
    decision_log: Optional[str] = None  # CSV path for threshold-sweep analysis
    use_local_agent: bool = False       # Use orchestrator_agent gRPC instead of SSH
    agent_port: int = 50052             # Port of orchestrator_agent on BF2


# ---------------------------------------------------------------------------
# Sliding window sample
# ---------------------------------------------------------------------------

@dataclass
class HostSample:
    """One sample of host-level metrics."""
    timestamp: float
    llc_miss_rate: float
    cpu_usage: float       # % overall host CPU
    net_io: float          # bytes/sec (rx + tx)
    ctx_switches: float    # per second


@dataclass
class NICSample:
    """One sample of SmartNIC-level metrics."""
    timestamp: float
    cpu_usage: float
    mem_avail_mb: float


# ---------------------------------------------------------------------------
# Utility: run command via SSH or locally
# ---------------------------------------------------------------------------

def run_cmd(cmd: str, ssh_target: Optional[str] = None,
            timeout: int = 30) -> Tuple[int, str, str]:
    """Run a shell command locally or via SSH. Returns (rc, stdout, stderr).

    ssh_target formats:
      "user@host"              → single-hop
      "user@host>>root@bf2"    → two-hop (host as jump proxy)
      None                     → local
    """
    if ssh_target and ">>" in ssh_target:
        jump_host, bf2_target = ssh_target.split(">>", 1)
        inner_cmd = cmd.replace("'", "'\\''")
        full_cmd = ["ssh", "-o", "StrictHostKeyChecking=no",
                     "-o", "ConnectTimeout=5", jump_host,
                     f"ssh -o StrictHostKeyChecking=no {bf2_target} '{inner_cmd}'"]
    elif ssh_target:
        full_cmd = ["ssh", "-o", "StrictHostKeyChecking=no",
                     "-o", "ConnectTimeout=5", ssh_target, cmd]
    else:
        full_cmd = ["bash", "-c", cmd]

    try:
        proc = subprocess.run(full_cmd, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except Exception as e:
        return -1, "", str(e)


# ---------------------------------------------------------------------------
# LLC miss rate measurement via perf stat
# ---------------------------------------------------------------------------

def measure_llc_miss_rate(ssh_target: Optional[str],
                          duration: float = 2.0) -> Optional[float]:
    """Measure system-wide LLC miss rate. Returns ratio or None."""
    perf_cmd = f"perf stat -e LLC-load-misses,LLC-loads -a sleep {duration} 2>&1"
    rc, out, err = run_cmd(perf_cmd, ssh_target, timeout=int(duration) + 10)
    combined = out + "\n" + err

    misses, loads = None, None
    for line in combined.split("\n"):
        line = line.strip().replace(",", "")
        if "LLC-load-misses" in line:
            parts = line.split()
            if parts and parts[0].isdigit():
                misses = int(parts[0])
        elif "LLC-loads" in line:
            parts = line.split()
            if parts and parts[0].isdigit():
                loads = int(parts[0])

    if misses is not None and loads is not None and loads > 0:
        return misses / loads
    return None


# ---------------------------------------------------------------------------
# Blue-green migration with VIP
# ---------------------------------------------------------------------------

class MigrationManager:
    """Executes blue-green migration of a workload to any SmartNIC.

    Transport selection per step:
      - if use_local_agent=True and a gRPC agent is reachable on the BF2 → use it
      - otherwise fall back to run_cmd() via two-hop SSH
    """

    def __init__(self, src_node: NodeConfig, dst_nic: SmartNICConfig,
                 workload: WorkloadConfig, vip: str,
                 src_is_host: bool, log: logging.Logger,
                 src_nic: Optional[SmartNICConfig] = None,
                 use_local_agent: bool = False, agent_port: int = 50052):
        self.src = src_node
        self.dst = dst_nic
        self.wl = workload
        self.vip = vip
        self.src_is_host = src_is_host
        self.src_nic = src_nic
        self.log = log
        self.dst_agent: Optional[AgentClient] = None
        self.src_nic_agent: Optional[AgentClient] = None
        if use_local_agent and _AGENT_AVAILABLE:
            self.dst_agent = AgentClient(self.dst.bf2_ip, agent_port)
            if not self.dst_agent.ping():
                self.log.warning(
                    f"orchestrator_agent unreachable on {self.dst.bf2_ip}, "
                    "falling back to SSH")
                self.dst_agent = None
            if src_nic:
                self.src_nic_agent = AgentClient(src_nic.bf2_ip, agent_port)
                if not self.src_nic_agent.ping():
                    self.src_nic_agent = None

    def _start_container_on_dst(self) -> Tuple[bool, str]:
        if self.dst_agent and self.dst_agent.available():
            return self.dst_agent.start_container(
                name=f"{self.wl.name}-new",
                image=self.wl.image,
                network="host",
                cpuset="",
                extra_args=self.wl.docker_args.split() if self.wl.docker_args else [],
            )
        docker_cmd = (
            f"docker rm -f {self.wl.name}-new 2>/dev/null; "
            f"docker run -d --name {self.wl.name}-new --network=host "
            f"{self.wl.docker_args} {self.wl.image}"
        )
        rc, out, err = run_cmd(docker_cmd, self.dst.bf2_ssh, timeout=60)
        return (rc == 0), (out if rc == 0 else err)

    def _stop_container_on_dst(self, name: str):
        if self.dst_agent and self.dst_agent.available():
            self.dst_agent.stop_container(name, force=True)
        else:
            run_cmd(f"docker rm -f {name}", self.dst.bf2_ssh)

    def _health_check_dst(self) -> bool:
        url = f"http://localhost:{self.wl.host_port}{self.wl.health_path}"
        if self.dst_agent and self.dst_agent.available():
            healthy, _attempts, _ms = self.dst_agent.health_check(
                url, max_attempts=self.wl.health_timeout, interval_ms=1000)
            return healthy
        for _ in range(self.wl.health_timeout):
            time.sleep(1)
            rc, _, _ = run_cmd(f"curl -sf -o /dev/null {url}",
                                self.dst.bf2_ssh, timeout=5)
            if rc == 0:
                return True
        return False

    def _vip_on_dst(self, action: str) -> bool:
        if self.dst_agent and self.dst_agent.available():
            ok, _err, _ms = self.dst_agent.switch_vip(
                self.vip, self.dst.bf2_iface, action=action,
                prefix_len=24, send_arp=(action == "add"))
            return ok
        cmd = f"ip addr {action} {self.vip}/24 dev {self.dst.bf2_iface}"
        rc, _, _ = run_cmd(cmd + " 2>/dev/null", self.dst.bf2_ssh)
        if action == "add" and rc == 0:
            run_cmd(f"arping -c 3 -A -I {self.dst.bf2_iface} {self.vip} &>/dev/null &",
                    self.dst.bf2_ssh)
        return rc == 0 or rc == 2  # EEXIST is ok

    def _vip_on_src(self, action: str):
        if self.src_is_host:
            cmd = f"sudo ip addr {action} {self.vip}/24 dev {self.src.host_iface}"
            run_cmd(cmd + " 2>/dev/null", self.src.host_ssh)
        elif self.src_nic:
            if self.src_nic_agent and self.src_nic_agent.available():
                self.src_nic_agent.switch_vip(
                    self.vip, self.src_nic.bf2_iface, action=action,
                    prefix_len=24, send_arp=(action == "add"))
            else:
                run_cmd(
                    f"ip addr {action} {self.vip}/24 dev {self.src_nic.bf2_iface} 2>/dev/null",
                    self.src_nic.bf2_ssh)

    def execute(self) -> Tuple[bool, Dict[str, float]]:
        """
        Blue-green migration to target SmartNIC.
        Returns (success, {stage: duration_ms}).
        """
        timings = {}
        wl = self.wl

        t0 = time.monotonic()
        self.log.info(f"[migrate] Start {wl.name} on {self.dst.nic_id} "
                      f"({'agent' if self.dst_agent else 'ssh'})")
        ok, detail = self._start_container_on_dst()
        if not ok:
            self.log.error(f"[migrate] Container start failed: {detail}")
            return False, timings
        timings["container_start"] = (time.monotonic() - t0) * 1000

        t0 = time.monotonic()
        if not self._health_check_dst():
            self.log.error(f"[migrate] Health check failed, aborting")
            self._stop_container_on_dst(f"{wl.name}-new")
            return False, timings
        timings["health_check"] = (time.monotonic() - t0) * 1000

        t0 = time.monotonic()
        self.log.info(f"[migrate] VIP switch: {self.vip} → {self.dst.nic_id}")
        self._vip_on_src("del")
        if not self._vip_on_dst("add"):
            self.log.error(f"[migrate] VIP add failed, rolling back")
            self._vip_on_src("add")
            self._stop_container_on_dst(f"{wl.name}-new")
            return False, timings
        timings["vip_switch"] = (time.monotonic() - t0) * 1000

        t0 = time.monotonic()
        if self.src_is_host:
            run_cmd(f"docker rm -f {wl.name}", self.src.host_ssh)
        # Rename new container to canonical name
        if self.dst_agent and self.dst_agent.available():
            # agent has no rename rpc; use docker command via ssh as fallback
            run_cmd(f"docker rename {wl.name}-new {wl.name}",
                    self.dst.bf2_ssh)
        else:
            run_cmd(f"docker rename {wl.name}-new {wl.name}",
                    self.dst.bf2_ssh)
        timings["old_cleanup"] = (time.monotonic() - t0) * 1000

        total = sum(timings.values())
        self.log.info(f"[migrate] Complete in {total:.0f}ms: " +
                      ", ".join(f"{k}={v:.0f}ms" for k, v in timings.items()))
        return True, timings


# ---------------------------------------------------------------------------
# Database helpers
# ---------------------------------------------------------------------------

def db_log_event(conn, node_uuid: Optional[str], event_type: str, detail: str):
    """Insert an event into cluster_events table."""
    if conn is None:
        return
    try:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO cluster_events (node_uuid, event_type, detail) "
                "VALUES (%s, %s, %s)",
                (node_uuid, event_type, detail)
            )
        conn.commit()
    except Exception as e:
        logging.getLogger("orchestrator").warning(f"DB log failed: {e}")
        try:
            conn.rollback()
        except Exception:
            pass


def db_query_host_metrics(conn, node_uuid: str, window_sec: int = 30) -> Optional[dict]:
    """Query average host metrics over sliding window."""
    if conn is None:
        return None
    try:
        with conn.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
            cur.execute("""
                SELECT
                    AVG(cpu_usage) as avg_cpu,
                    AVG(net_rx_bytes + net_tx_bytes) as avg_net_io,
                    AVG(ctx_switches) as avg_ctx_sw
                FROM host_metrics
                WHERE node_uuid = %s
                  AND time > NOW() - make_interval(secs => %s)
            """, (node_uuid, window_sec))
            row = cur.fetchone()
            if row and row['avg_cpu'] is not None:
                return dict(row)
    except Exception as e:
        logging.getLogger("orchestrator").warning(f"DB host query failed: {e}")
    return None


def db_query_nic_metrics(conn, nic_uuid: str, window_sec: int = 30) -> Optional[dict]:
    """Query average SmartNIC metrics over sliding window."""
    if conn is None:
        return None
    try:
        with conn.cursor(cursor_factory=psycopg2.extras.DictCursor) as cur:
            cur.execute("""
                SELECT
                    AVG(cpu_usage) as avg_cpu,
                    AVG(mem_available_mb) as avg_mem_avail
                FROM bf2_metrics
                WHERE node_uuid = %s
                  AND time > NOW() - make_interval(secs => %s)
            """, (nic_uuid, window_sec))
            row = cur.fetchone()
            if row and row['avg_cpu'] is not None:
                return dict(row)
    except Exception as e:
        logging.getLogger("orchestrator").warning(f"DB NIC query failed: {e}")
    return None


# ---------------------------------------------------------------------------
# Main orchestrator daemon
# ---------------------------------------------------------------------------

class Orchestrator:
    def __init__(self, config: OrchestratorConfig):
        self.cfg = config
        self.running = True
        self.db_conn = None
        self.log = logging.getLogger("orchestrator")

        # Sliding window of LLC samples per node (node_uuid → deque of floats)
        self.llc_window: Dict[str, Deque[float]] = {
            uid: collections.deque(maxlen=config.window_size)
            for uid in config.nodes
        }

        # Per-node LLC baselines
        self.node_baselines: Dict[str, float] = {}

        # Track current workload placement: workload_name → nic_id (or None if on host)
        self.placement: Dict[str, Optional[str]] = {}

        # Decision log file handle (for threshold sensitivity analysis)
        self.decision_log_fp = None
        self.dry_run = False
        if config.decision_log:
            os.makedirs(os.path.dirname(os.path.abspath(config.decision_log)),
                        exist_ok=True)
            self.decision_log_fp = open(config.decision_log, "a")
            # Write header if empty
            if os.path.getsize(config.decision_log) == 0:
                self.decision_log_fp.write(
                    "timestamp_ns,node_uuid,sample_llc,window_avg_llc,"
                    "baseline_llc,theta_llc_factor,threshold_llc,"
                    "above_threshold,decision\n")
                self.decision_log_fp.flush()

    def __del__(self):
        try:
            if self.decision_log_fp:
                self.decision_log_fp.close()
        except Exception:
            pass

    def log_decision(self, uid: str, sample_llc: float, window_avg: float,
                     baseline: float, threshold: float, above: bool,
                     decision: str):
        """Record one scheduling decision for threshold sensitivity analysis."""
        if not self.decision_log_fp:
            return
        import time as _t
        ts = int(_t.time_ns())
        self.decision_log_fp.write(
            f"{ts},{uid},{sample_llc:.6f},{window_avg:.6f},"
            f"{baseline:.6f},{self.cfg.thresholds.llc_factor:.3f},"
            f"{threshold:.6f},{int(above)},{decision}\n")
        self.decision_log_fp.flush()

    # --- DB connection ---

    def connect_db(self):
        if psycopg2 is None:
            return
        try:
            self.db_conn = psycopg2.connect(self.cfg.db_connstr)
            self.db_conn.autocommit = False
            self.log.info("Connected to TimescaleDB")
        except Exception as e:
            self.log.warning(f"DB connection failed: {e}")
            self.db_conn = None

    # --- Phase 0: Calibration ---

    def calibrate_baseline(self):
        """Measure baseline LLC miss rate on each node (no workload interference)."""
        self.log.info("Calibrating per-node LLC baselines...")
        for uid, node in self.cfg.nodes.items():
            rate = measure_llc_miss_rate(node.host_ssh, self.cfg.perf_duration)
            if rate is not None:
                self.node_baselines[uid] = rate
                threshold = rate * self.cfg.thresholds.llc_factor
                self.log.info(f"  {uid}: baseline={rate:.4f}, threshold={threshold:.4f}")
            else:
                self.log.warning(f"  {uid}: calibration failed, using default 0.18")
                self.node_baselines[uid] = 0.18

    # --- Phase 1: Interference detection (LLC miss rate, sliding window) ---

    def phase1_detect_interference(self) -> List[str]:
        """
        Sample LLC miss rate on each node, update sliding window,
        return list of node_uuids with avg LLC above threshold.
        """
        interfered_nodes = []
        th = self.cfg.thresholds

        for uid, node in self.cfg.nodes.items():
            rate = measure_llc_miss_rate(node.host_ssh, self.cfg.perf_duration)
            if rate is None:
                continue

            self.llc_window[uid].append(rate)

            # Need at least 2 samples for reliable average
            if len(self.llc_window[uid]) < 2:
                # Log even with short window for sensitivity study
                self.log_decision(uid, rate, rate,
                                  self.node_baselines.get(uid, 0.18),
                                  self.node_baselines.get(uid, 0.18) * th.llc_factor,
                                  False, "insufficient_window")
                continue

            avg_llc = sum(self.llc_window[uid]) / len(self.llc_window[uid])
            baseline = self.node_baselines.get(uid, 0.18)
            threshold = baseline * th.llc_factor

            above = avg_llc > threshold
            if above:
                self.log.warning(
                    f"[P1] {uid}: avg LLC={avg_llc:.4f} > threshold={threshold:.4f} "
                    f"(window={len(self.llc_window[uid])} samples)")
                interfered_nodes.append(uid)
                decision = "interference_detected"
            else:
                self.log.debug(f"[P1] {uid}: avg LLC={avg_llc:.4f} OK")
                decision = "no_interference"

            self.log_decision(uid, rate, avg_llc, baseline, threshold,
                              above, decision)

        return interfered_nodes

    # --- Phase 2: Source identification (four-metric joint) ---

    def phase2_identify_sources(self, interfered_nodes: List[str]) -> List[Tuple[str, str]]:
        """
        For each interfered node, find workloads suitable for migration.
        Returns list of (workload_name, node_uuid).
        """
        candidates = []
        th = self.cfg.thresholds
        window_sec = self.cfg.window_size * int(self.cfg.poll_interval)

        for uid in interfered_nodes:
            metrics = db_query_host_metrics(self.db_conn, uid, window_sec)
            if metrics is None:
                self.log.debug(f"[P2] {uid}: no DB metrics, skipping")
                continue

            avg_cpu = float(metrics.get('avg_cpu', 0) or 0)
            avg_io = float(metrics.get('avg_net_io', 0) or 0)
            avg_csw = float(metrics.get('avg_ctx_sw', 0) or 0)

            self.log.info(f"[P2] {uid}: cpu={avg_cpu:.1f}%, io={avg_io:.0f}B/s, "
                          f"csw={avg_csw:.0f}/s")

            for wl_name in self.cfg.workloads:
                # Skip if already migrated
                if self.placement.get(wl_name) is not None:
                    continue

                # Four-metric joint check
                if (avg_csw > th.ctx_switch_min and
                    avg_io > th.net_io_min and
                    avg_cpu < th.cpu_max):
                    self.log.info(f"[P2] {uid}/{wl_name}: eligible for migration "
                                  f"(csw={avg_csw:.0f}>{th.ctx_switch_min}, "
                                  f"io={avg_io:.0f}>{th.net_io_min}, "
                                  f"cpu={avg_cpu:.1f}<{th.cpu_max})")
                    candidates.append((wl_name, uid))

        return candidates

    # --- Phase 3: Target SmartNIC selection (local-first, load-balanced) ---

    def phase3_select_targets(self, candidates: List[Tuple[str, str]]) -> List[Tuple[str, str, str]]:
        """
        For each candidate (workload, source_node), select the best SmartNIC.
        Returns list of (workload_name, source_node_uuid, target_nic_id).
        Local SmartNIC is preferred; cross-node fallback sorted by CPU ascending.
        """
        migrations = []
        th = self.cfg.thresholds
        window_sec = self.cfg.window_size * int(self.cfg.poll_interval)

        # Collect current NIC loads
        nic_loads: Dict[str, dict] = {}
        for nic_id, nic in self.cfg.nics.items():
            m = db_query_nic_metrics(self.db_conn, nic_id, window_sec)
            if m:
                nic_loads[nic_id] = m
            else:
                nic_loads[nic_id] = {'avg_cpu': 0, 'avg_mem_avail': 8000}

        for wl_name, src_uid in candidates:
            wl = self.cfg.workloads[wl_name]
            src_node = self.cfg.nodes[src_uid]

            # Build candidate list: local NIC first, then remote NICs by CPU asc
            local_nic_id = src_node.local_nic
            remote_nics = sorted(
                [nid for nid in self.cfg.nics if nid != local_nic_id],
                key=lambda nid: float(nic_loads.get(nid, {}).get('avg_cpu', 100))
            )
            ordered = [local_nic_id] + remote_nics

            selected = None
            for nic_id in ordered:
                load = nic_loads.get(nic_id, {})
                cpu = float(load.get('avg_cpu', 100) or 100)
                mem = float(load.get('avg_mem_avail', 0) or 0)

                if cpu < th.nic_cpu_max and mem > wl.mem_req_mb:
                    selected = nic_id
                    loc = "local" if nic_id == local_nic_id else "cross-node"
                    self.log.info(f"[P3] {wl_name}: → {nic_id} ({loc}, "
                                  f"cpu={cpu:.1f}%, mem={mem:.0f}MB)")
                    break

            if selected:
                migrations.append((wl_name, src_uid, selected))
            else:
                self.log.warning(f"[P3] {wl_name}: no available SmartNIC")

        return migrations

    # --- Phase 4: Blue-green migration execution ---

    def phase4_migrate(self, migrations: List[Tuple[str, str, str]]):
        """Execute blue-green migrations."""
        for wl_name, src_uid, dst_nic_id in migrations:
            src_node = self.cfg.nodes[src_uid]
            dst_nic = self.cfg.nics[dst_nic_id]
            wl = self.cfg.workloads[wl_name]
            vip = self.cfg.vips.get(wl_name, "")

            if not vip:
                self.log.error(f"[P4] No VIP configured for {wl_name}")
                continue

            db_log_event(self.db_conn, src_uid, "migration_triggered",
                         f"wl={wl_name}, src={src_uid}, dst={dst_nic_id}")

            current_nic_id = self.placement.get(wl_name)
            src_nic = self.cfg.nics.get(current_nic_id) if current_nic_id else None
            mgr = MigrationManager(
                src_node=src_node,
                dst_nic=dst_nic,
                workload=wl,
                vip=vip,
                src_is_host=(current_nic_id is None),
                log=self.log,
                src_nic=src_nic,
                use_local_agent=self.cfg.use_local_agent,
                agent_port=self.cfg.agent_port,
            )
            success, timings = mgr.execute()

            if success:
                self.placement[wl_name] = dst_nic_id
                # Clear sliding window to avoid re-triggering on stale data
                self.llc_window[src_uid].clear()
                db_log_event(self.db_conn, src_uid, "migration_complete",
                             f"{wl_name} → {dst_nic_id}, "
                             f"total={sum(timings.values()):.0f}ms")
            else:
                db_log_event(self.db_conn, src_uid, "migration_failed",
                             f"{wl_name} → {dst_nic_id} failed")

    # --- Main scheduling cycle ---

    def ensure_db(self):
        """Reconnect to DB if connection is lost."""
        if self.db_conn is None or self.db_conn.closed:
            self.log.info("DB connection lost, reconnecting...")
            self.connect_db()

    def run_cycle(self):
        """One complete 4-phase scheduling cycle."""
        self.ensure_db()
        # Phase 1
        interfered = self.phase1_detect_interference()
        if not interfered:
            return

        if self.dry_run:
            self.log.info(f"[dry-run] would evaluate migration for {interfered}")
            return

        # Phase 2
        candidates = self.phase2_identify_sources(interfered)
        if not candidates:
            return

        # Phase 3
        migrations = self.phase3_select_targets(candidates)
        if not migrations:
            return

        # Phase 4 (one migration per cycle to avoid overload)
        self.phase4_migrate(migrations[:1])

    # --- Daemon loop ---

    def run(self):
        """Main daemon entry point."""
        self.log.info(f"Orchestrator starting — cluster-level scheduling")
        self.log.info(f"  poll={self.cfg.poll_interval}s, window={self.cfg.window_size} samples "
                      f"({self.cfg.window_size * self.cfg.poll_interval}s)")
        self.log.info(f"  nodes: {list(self.cfg.nodes.keys())}")
        self.log.info(f"  nics:  {list(self.cfg.nics.keys())}")
        self.log.info(f"  thresholds: llc={self.cfg.thresholds.llc_factor}x, "
                      f"csw>{self.cfg.thresholds.ctx_switch_min}, "
                      f"io>{self.cfg.thresholds.net_io_min}, "
                      f"cpu<{self.cfg.thresholds.cpu_max}%")

        self.connect_db()
        self.calibrate_baseline()

        db_log_event(self.db_conn, None, "orchestrator_start",
                     json.dumps({"poll": self.cfg.poll_interval,
                                 "window": self.cfg.window_size}))

        while self.running:
            try:
                self.run_cycle()
            except KeyboardInterrupt:
                break
            except Exception as e:
                self.log.error(f"Cycle error: {e}", exc_info=True)
            time.sleep(self.cfg.poll_interval)

        self.log.info("Orchestrator stopped")
        db_log_event(self.db_conn, None, "orchestrator_stop", "")
        if self.db_conn:
            self.db_conn.close()


# ---------------------------------------------------------------------------
# Cluster topology configuration
# ---------------------------------------------------------------------------

def build_default_config() -> OrchestratorConfig:
    """Build config from the standard 3-node cluster topology."""
    user = os.environ.get('USER', 'user')
    cfg = OrchestratorConfig()

    # --- Worker nodes ---
    cfg.nodes = {
        "fujian": NodeConfig(
            node_uuid="fujian",
            host_ip="192.168.56.11",
            host_ssh=f"{user}@172.28.4.77",
            host_iface="enp94s0f1np1",
            local_nic="fujian-bf2",
        ),
        "helong": NodeConfig(
            node_uuid="helong",
            host_ip="192.168.56.12",
            host_ssh=f"{user}@172.28.4.85",
            host_iface="enp94s0f0np0",
            local_nic="helong-bf2",
        ),
    }

    # --- SmartNICs (all available as migration targets) ---
    cfg.nics = {
        "fujian-bf2": SmartNICConfig(
            nic_id="fujian-bf2",
            bf2_ip="192.168.56.3",
            bf2_ssh=f"{user}@172.28.4.77>>root@192.168.100.2",
            bf2_iface="p1",
            parent_host="fujian",
        ),
        "helong-bf2": SmartNICConfig(
            nic_id="helong-bf2",
            bf2_ip="192.168.56.1",
            bf2_ssh=f"{user}@172.28.4.85>>root@192.168.100.2",
            bf2_iface="p0",
            parent_host="helong",
        ),
    }

    # --- Workloads ---
    cfg.workloads = {
        "nginx": WorkloadConfig(
            name="nginx",
            image="nginx:alpine",
            host_port=80,
            health_path="/",
            health_timeout=30,
            mem_req_mb=256,
        ),
    }

    # --- VIPs ---
    cfg.vips = {
        "nginx": "192.168.56.200",
    }

    return cfg


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Cluster-level workload orchestration daemon")
    parser.add_argument("--db-connstr", default=None)
    parser.add_argument("--poll-interval", type=float, default=5.0)
    parser.add_argument("--llc-threshold", type=float, default=2.0,
                        help="LLC threshold multiplier (default: 2.0x baseline)")
    parser.add_argument("--llc-baseline", type=float, default=0.0,
                        help="Known baseline LLC miss rate (0=auto-calibrate)")
    parser.add_argument("--perf-duration", type=float, default=2.0)
    parser.add_argument("--window-size", type=int, default=6,
                        help="Sliding window sample count (default: 6)")
    parser.add_argument("--log-level", default="INFO",
                        choices=["DEBUG", "INFO", "WARNING", "ERROR"])
    parser.add_argument("--decision-log", default=None,
                        help="CSV file to record every phase-1 decision "
                             "(for threshold sensitivity analysis)")
    parser.add_argument("--use-local-agent", action="store_true",
                        help="Use orchestrator_agent gRPC on BF2 instead of SSH")
    parser.add_argument("--agent-port", type=int, default=50052,
                        help="Port of orchestrator_agent on BF2 (default 50052)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Log decisions but skip phases 2-4 (no migration)")
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    cfg = build_default_config()
    if args.db_connstr:
        cfg.db_connstr = args.db_connstr
    cfg.poll_interval = args.poll_interval
    cfg.thresholds.llc_factor = args.llc_threshold
    cfg.perf_duration = args.perf_duration
    cfg.window_size = args.window_size
    cfg.decision_log = args.decision_log
    cfg.use_local_agent = args.use_local_agent
    cfg.agent_port = args.agent_port

    orch = Orchestrator(cfg)
    orch.dry_run = args.dry_run

    if args.llc_baseline > 0:
        for uid in cfg.nodes:
            orch.node_baselines[uid] = args.llc_baseline

    def handle_signal(signum, frame):
        orch.running = False

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    orch.run()


if __name__ == "__main__":
    main()
