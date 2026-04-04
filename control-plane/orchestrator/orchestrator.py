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
    """Executes blue-green migration of a workload to any SmartNIC."""

    def __init__(self, src_node: NodeConfig, dst_nic: SmartNICConfig,
                 workload: WorkloadConfig, vip: str,
                 src_is_host: bool, log: logging.Logger,
                 src_nic: Optional[SmartNICConfig] = None):
        self.src = src_node
        self.dst = dst_nic
        self.wl = workload
        self.vip = vip
        self.src_is_host = src_is_host
        self.src_nic = src_nic  # set when source is a SmartNIC (re-migration)
        self.log = log

    def execute(self) -> Tuple[bool, Dict[str, float]]:
        """
        Blue-green migration to target SmartNIC.
        Returns (success, {stage: duration_ms}).
        """
        timings = {}
        wl = self.wl

        # --- Stage 1: Start new container on target SmartNIC ---
        t0 = time.monotonic()
        self.log.info(f"[migrate] Starting {wl.name} on {self.dst.nic_id} ({self.dst.bf2_ip})")
        docker_cmd = (
            f"docker rm -f {wl.name}-new 2>/dev/null; "
            f"docker run -d --name {wl.name}-new --network=host "
            f"{wl.docker_args} {wl.image}"
        )
        rc, _, err = run_cmd(docker_cmd, self.dst.bf2_ssh, timeout=60)
        if rc != 0:
            self.log.error(f"[migrate] Container start failed: {err}")
            return False, timings
        timings["container_start"] = (time.monotonic() - t0) * 1000

        # --- Stage 2: Health check ---
        t0 = time.monotonic()
        self.log.info(f"[migrate] Health check on {self.dst.nic_id}...")
        healthy = False
        for attempt in range(wl.health_timeout):
            time.sleep(1)
            rc, _, _ = run_cmd(
                f"curl -sf -o /dev/null http://localhost:{wl.host_port}{wl.health_path}",
                self.dst.bf2_ssh, timeout=5
            )
            if rc == 0:
                healthy = True
                break

        if not healthy:
            self.log.error(f"[migrate] Health check failed, aborting")
            run_cmd(f"docker rm -f {wl.name}-new", self.dst.bf2_ssh)
            return False, timings
        timings["health_check"] = (time.monotonic() - t0) * 1000

        # --- Stage 3: VIP atomic switch ---
        t0 = time.monotonic()
        self.log.info(f"[migrate] VIP switch: {self.vip} → {self.dst.nic_id}")

        # Remove VIP from source
        if self.src_is_host:
            run_cmd(f"sudo ip addr del {self.vip}/24 dev {self.src.host_iface} 2>/dev/null",
                    self.src.host_ssh)
        elif self.src_nic:
            # Source is another SmartNIC — remove VIP from the actual source
            run_cmd(f"ip addr del {self.vip}/24 dev {self.src_nic.bf2_iface} 2>/dev/null",
                    self.src_nic.bf2_ssh)

        # Add VIP to destination SmartNIC
        rc, _, err = run_cmd(f"ip addr add {self.vip}/24 dev {self.dst.bf2_iface}",
                             self.dst.bf2_ssh)
        if rc != 0:
            self.log.error(f"[migrate] VIP add failed: {err}, rolling back")
            if self.src_is_host:
                run_cmd(f"sudo ip addr add {self.vip}/24 dev {self.src.host_iface}",
                        self.src.host_ssh)
            run_cmd(f"docker rm -f {wl.name}-new", self.dst.bf2_ssh)
            return False, timings

        # Gratuitous ARP
        run_cmd(f"arping -c 3 -A -I {self.dst.bf2_iface} {self.vip} &>/dev/null &",
                self.dst.bf2_ssh)
        timings["vip_switch"] = (time.monotonic() - t0) * 1000

        # --- Stage 4: Stop old container ---
        t0 = time.monotonic()
        if self.src_is_host:
            run_cmd(f"docker rm -f {wl.name}", self.src.host_ssh)
        # Rename new container
        run_cmd(f"docker rename {wl.name}-new {wl.name}", self.dst.bf2_ssh)
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
                continue

            avg_llc = sum(self.llc_window[uid]) / len(self.llc_window[uid])
            baseline = self.node_baselines.get(uid, 0.18)
            threshold = baseline * th.llc_factor

            if avg_llc > threshold:
                self.log.warning(
                    f"[P1] {uid}: avg LLC={avg_llc:.4f} > threshold={threshold:.4f} "
                    f"(window={len(self.llc_window[uid])} samples)")
                interfered_nodes.append(uid)
            else:
                self.log.debug(f"[P1] {uid}: avg LLC={avg_llc:.4f} OK")

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

    orch = Orchestrator(cfg)

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
