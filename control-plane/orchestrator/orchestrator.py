#!/usr/bin/env python3
"""
orchestrator.py — Workload orchestration daemon.

Runs on the master node as a long-lived background service. Monitors cluster
metrics in TimescaleDB, detects LLC interference on worker nodes, and
automatically migrates I/O-intensive workloads from host to SmartNIC via
blue-green deployment with floating VIP.

Usage:
    python3 orchestrator.py \
        --db-connstr="host=localhost dbname=cluster_metrics user=postgres password=postgres" \
        --poll-interval=5 \
        --llc-threshold=2.0

The daemon operates in three phases:
    1. MONITOR:  Query TimescaleDB + perf stat for LLC miss rate
    2. DECIDE:   Compare against threshold, identify migration candidate
    3. MIGRATE:  Blue-green deploy to BF2 with VIP switch
"""

import argparse
import json
import logging
import os
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

try:
    import psycopg2
    import psycopg2.extras
except ImportError:
    psycopg2 = None
    print("WARNING: psycopg2 not installed, DB queries disabled", file=sys.stderr)

# ---------------------------------------------------------------------------
# Configuration
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


@dataclass
class NodeConfig:
    """Describes a worker node and its BF2."""
    node_uuid: str               # Cluster node ID
    host_ip: str                 # Host 100G IP (e.g. 192.168.56.11)
    bf2_ip: str                  # BF2 100G IP (e.g. 192.168.56.3)
    bf2_ssh: str                 # SSH target for BF2 (e.g. root@192.168.100.2)
    host_ssh: str                # SSH target for host (user@mgmt_ip)
    host_iface: str              # Host 100G interface name
    bf2_iface: str               # BF2 100G interface name (e.g. p1)
    vip: str = ""                # Floating VIP assigned to this node's workload


@dataclass
class OrchestratorConfig:
    db_connstr: str = "host=localhost dbname=cluster_metrics user=postgres password=postgres sslmode=disable"
    poll_interval: float = 5.0   # Seconds between monitoring cycles
    llc_threshold: float = 2.0   # Trigger when LLC miss rate > baseline * this factor
    llc_baseline: float = 0.0    # Baseline LLC miss rate (set during calibration)
    perf_duration: float = 2.0   # Seconds to sample perf stat
    nodes: List[NodeConfig] = field(default_factory=list)
    workloads: Dict[str, WorkloadConfig] = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Utility: run command via SSH or locally
# ---------------------------------------------------------------------------

def run_cmd(cmd: str, ssh_target: Optional[str] = None,
            timeout: int = 30) -> Tuple[int, str, str]:
    """Run a shell command locally or via SSH. Returns (rc, stdout, stderr).

    ssh_target can be:
      - "user@host"              → single-hop SSH
      - "user@host>>root@bf2"    → two-hop SSH (host is jump proxy for BF2)
      - None                     → run locally
    """
    if ssh_target and ">>" in ssh_target:
        # Two-hop: ssh to host, then nested ssh to BF2
        jump_host, bf2_target = ssh_target.split(">>", 1)
        # Escape inner command for nested SSH
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
        proc = subprocess.run(full_cmd, capture_output=True, text=True,
                              timeout=timeout)
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()
    except subprocess.TimeoutExpired:
        return -1, "", "timeout"
    except Exception as e:
        return -1, "", str(e)


# ---------------------------------------------------------------------------
# LLC miss rate measurement via perf stat
# ---------------------------------------------------------------------------

def measure_llc_miss_rate(ssh_target: Optional[str], duration: float = 2.0,
                           pid: Optional[int] = None) -> Optional[float]:
    """
    Measure system-wide LLC miss rate using perf stat.
    Returns LLC-load-misses / LLC-loads ratio, or None on failure.
    """
    perf_cmd = f"perf stat -e LLC-load-misses,LLC-loads -a "
    if pid:
        perf_cmd = f"perf stat -e LLC-load-misses,LLC-loads -p {pid} "
    perf_cmd += f"sleep {duration} 2>&1"

    rc, out, err = run_cmd(perf_cmd, ssh_target, timeout=int(duration) + 10)
    combined = out + "\n" + err

    misses = None
    loads = None
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
    """Executes blue-green migration of a workload from host to BF2."""

    def __init__(self, node: NodeConfig, workload: WorkloadConfig, log: logging.Logger):
        self.node = node
        self.wl = workload
        self.log = log

    def migrate_to_bf2(self) -> bool:
        """
        Blue-green migration: host → BF2.
        1. Start new container on BF2 (old still running on host)
        2. Health check new container
        3. VIP switch: remove from host, add to BF2, gratuitous ARP
        4. Stop old container on host
        Returns True on success.
        """
        n, wl = self.node, self.wl

        # Step 1: Start new container on BF2
        self.log.info(f"[migrate] Starting {wl.name} on BF2 ({n.bf2_ip})...")
        docker_cmd = (
            f"docker rm -f {wl.name}-new 2>/dev/null; "
            f"docker run -d --name {wl.name}-new --network=host "
            f"{wl.docker_args} {wl.image}"
        )
        rc, out, err = run_cmd(docker_cmd, n.bf2_ssh, timeout=60)
        if rc != 0:
            self.log.error(f"[migrate] Failed to start container on BF2: {err}")
            return False

        # Step 2: Health check
        self.log.info(f"[migrate] Waiting for health check...")
        healthy = False
        for attempt in range(wl.health_timeout):
            time.sleep(1)
            rc, _, _ = run_cmd(
                f"curl -sf -o /dev/null http://localhost:{wl.host_port}{wl.health_path}",
                n.bf2_ssh, timeout=5
            )
            if rc == 0:
                healthy = True
                self.log.info(f"[migrate] Health check passed (attempt {attempt+1})")
                break

        if not healthy:
            self.log.error(f"[migrate] Health check failed after {wl.health_timeout}s, aborting")
            run_cmd(f"docker rm -f {wl.name}-new", n.bf2_ssh)
            return False

        # Step 3: VIP switch (with rollback on failure)
        self.log.info(f"[migrate] Switching VIP {n.vip} from host to BF2...")
        t_switch_start = time.monotonic()

        # Remove VIP from host
        run_cmd(f"sudo ip addr del {n.vip}/24 dev {n.host_iface} 2>/dev/null",
                n.host_ssh)
        # Add VIP to BF2
        rc, _, err = run_cmd(f"ip addr add {n.vip}/24 dev {n.bf2_iface}",
                             n.bf2_ssh)
        if rc != 0:
            self.log.error(f"[migrate] VIP add to BF2 failed: {err}, rolling back")
            run_cmd(f"sudo ip addr add {n.vip}/24 dev {n.host_iface}", n.host_ssh)
            run_cmd(f"docker rm -f {wl.name}-new", n.bf2_ssh)
            return False
        # Gratuitous ARP to update switch MAC tables
        run_cmd(f"arping -c 3 -A -I {n.bf2_iface} {n.vip} &>/dev/null &",
                n.bf2_ssh)

        t_switch_ms = (time.monotonic() - t_switch_start) * 1000
        self.log.info(f"[migrate] VIP switch completed in {t_switch_ms:.0f} ms")

        # Step 4: Stop old container on host
        self.log.info(f"[migrate] Stopping old container on host...")
        run_cmd(f"docker rm -f {wl.name}", n.host_ssh)

        # Rename new container
        run_cmd(f"docker rename {wl.name}-new {wl.name}", n.bf2_ssh)

        self.log.info(f"[migrate] Migration complete: {wl.name} now on BF2 ({n.bf2_ip})")
        return True

    def migrate_to_host(self) -> bool:
        """
        Reverse migration: BF2 → host (for recovery or rebalancing).
        Same blue-green pattern but in the opposite direction.
        """
        n, wl = self.node, self.wl

        # Start new container on host
        docker_cmd = (
            f"docker rm -f {wl.name}-new 2>/dev/null; "
            f"docker run -d --name {wl.name}-new --network=host "
            f"{wl.docker_args} {wl.image}"
        )
        rc, _, err = run_cmd(docker_cmd, n.host_ssh, timeout=60)
        if rc != 0:
            self.log.error(f"[migrate-back] Failed to start on host: {err}")
            return False

        # Health check
        for attempt in range(wl.health_timeout):
            time.sleep(1)
            rc, _, _ = run_cmd(
                f"curl -sf -o /dev/null http://localhost:{wl.host_port}{wl.health_path}",
                n.host_ssh, timeout=5
            )
            if rc == 0:
                break
        else:
            run_cmd(f"docker rm -f {wl.name}-new", n.host_ssh)
            return False

        # VIP switch: BF2 → host
        run_cmd(f"ip addr del {n.vip}/24 dev {n.bf2_iface} 2>/dev/null",
                n.bf2_ssh)
        run_cmd(f"sudo ip addr add {n.vip}/24 dev {n.host_iface}",
                n.host_ssh)
        run_cmd(f"sudo arping -c 3 -A -I {n.host_iface} {n.vip} &>/dev/null &",
                n.host_ssh)

        # Stop old on BF2
        run_cmd(f"docker stop {wl.name} && docker rm {wl.name}", n.bf2_ssh)
        run_cmd(f"docker rename {wl.name}-new {wl.name}", n.host_ssh)

        self.log.info(f"[migrate-back] {wl.name} returned to host ({n.host_ip})")
        return True


# ---------------------------------------------------------------------------
# Database event logging
# ---------------------------------------------------------------------------

def db_log_event(conn, node_uuid: str, event_type: str, detail: str):
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
        logging.getLogger("orchestrator").warning(f"DB event log failed: {e}")
        try:
            conn.rollback()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Main orchestrator daemon
# ---------------------------------------------------------------------------

class Orchestrator:
    def __init__(self, config: OrchestratorConfig):
        self.cfg = config
        self.running = True
        self.db_conn = None
        self.log = logging.getLogger("orchestrator")

        # Track which workloads are currently on BF2 (node_uuid -> set of workload names)
        self.migrated: Dict[str, set] = {}
        # Per-node LLC baselines (measured during calibration)
        self.node_baselines: Dict[str, float] = {}

    def connect_db(self):
        if psycopg2 is None:
            return
        try:
            self.db_conn = psycopg2.connect(self.cfg.db_connstr)
            self.db_conn.autocommit = False
            self.log.info(f"Connected to TimescaleDB")
        except Exception as e:
            self.log.warning(f"DB connection failed: {e}")
            self.db_conn = None

    def calibrate_baseline(self):
        """Measure baseline LLC miss rate on each node (no interference)."""
        self.log.info("Calibrating per-node LLC baselines...")
        for node in self.cfg.nodes:
            rate = measure_llc_miss_rate(node.host_ssh, self.cfg.perf_duration)
            if rate is not None:
                self.node_baselines[node.node_uuid] = rate
                self.log.info(f"  {node.node_uuid}: baseline LLC miss rate = {rate:.4f}, "
                              f"threshold = {rate * self.cfg.llc_threshold:.4f}")
            else:
                self.log.warning(f"  {node.node_uuid}: calibration failed")

    def check_interference(self, node: NodeConfig) -> Optional[float]:
        """Check LLC miss rate on a node. Returns rate if above threshold."""
        rate = measure_llc_miss_rate(node.host_ssh, self.cfg.perf_duration)
        if rate is None:
            return None

        baseline = self.node_baselines.get(node.node_uuid, self.cfg.llc_baseline)
        threshold = baseline * self.cfg.llc_threshold
        if threshold <= 0:
            threshold = 0.1  # default 10% miss rate if no baseline

        if rate > threshold:
            self.log.warning(
                f"[{node.node_uuid}] LLC miss rate {rate:.4f} > threshold {threshold:.4f} "
                f"(baseline={baseline:.4f})")
            return rate
        return None

    def should_migrate(self, node: NodeConfig, workload_name: str) -> bool:
        """Check if a workload on this node should be migrated to BF2."""
        # Don't migrate if already on BF2
        if workload_name in self.migrated.get(node.node_uuid, set()):
            return False

        # Query recent network I/O from DB to verify it's I/O-intensive
        if self.db_conn:
            try:
                with self.db_conn.cursor() as cur:
                    cur.execute(
                        "SELECT AVG(net_rx_bytes + net_tx_bytes) "
                        "FROM host_metrics "
                        "WHERE node_uuid = %s AND time > NOW() - INTERVAL '30 seconds'",
                        (node.node_uuid,)
                    )
                    row = cur.fetchone()
                    if row and row[0]:
                        net_io = float(row[0])
                        self.log.info(f"[{node.node_uuid}] avg net I/O: {net_io:.0f} bytes/s")
                        # Only migrate if significant network I/O
                        if net_io < 1_000_000:  # < 1 MB/s
                            self.log.info(f"  Low net I/O, skipping migration")
                            return False
            except Exception as e:
                self.log.warning(f"DB query failed: {e}")

        return True

    def run_cycle(self):
        """One monitoring + decision cycle."""
        for node in self.cfg.nodes:
            # Check interference
            llc_rate = self.check_interference(node)
            if llc_rate is None:
                continue

            # Find a workload to migrate
            for wl_name, wl_cfg in self.cfg.workloads.items():
                if not self.should_migrate(node, wl_name):
                    continue

                self.log.info(f"Triggering migration: {wl_name} on {node.node_uuid} "
                              f"(LLC miss rate = {llc_rate:.4f})")

                db_log_event(self.db_conn, node.node_uuid, "migration_triggered",
                             f"LLC={llc_rate:.4f}, workload={wl_name}")

                mgr = MigrationManager(node, wl_cfg, self.log)
                success = mgr.migrate_to_bf2()

                if success:
                    self.migrated.setdefault(node.node_uuid, set()).add(wl_name)
                    db_log_event(self.db_conn, node.node_uuid, "migration_complete",
                                 f"{wl_name} -> BF2")
                else:
                    db_log_event(self.db_conn, node.node_uuid, "migration_failed",
                                 f"{wl_name} migration failed")
                break  # One migration per cycle to avoid overload

    def run(self):
        """Main daemon loop."""
        self.log.info(f"Orchestrator starting (poll={self.cfg.poll_interval}s, "
                      f"llc_threshold={self.cfg.llc_threshold}x)")

        self.connect_db()

        if self.cfg.llc_baseline <= 0:
            self.calibrate_baseline()

        db_log_event(self.db_conn, None, "orchestrator_start",
                     f"poll={self.cfg.poll_interval}s, llc_threshold={self.cfg.llc_threshold}x")

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
# CLI entry point
# ---------------------------------------------------------------------------

def build_default_config() -> OrchestratorConfig:
    """Build config from the standard cluster topology."""
    cfg = OrchestratorConfig()
    user = os.environ.get('USER', 'user')
    cfg.nodes = [
        NodeConfig(
            node_uuid="fujian-bf2",
            host_ip="192.168.56.11",
            bf2_ip="192.168.56.3",
            bf2_ssh=f"{user}@172.28.4.77>>root@192.168.100.2",  # two-hop via host
            host_ssh=f"{user}@172.28.4.77",
            host_iface="enp94s0f1np1",
            bf2_iface="p1",
            vip="192.168.56.200",
        ),
        NodeConfig(
            node_uuid="helong-bf2",
            host_ip="192.168.56.12",
            bf2_ip="192.168.56.1",
            bf2_ssh=f"{user}@172.28.4.85>>root@192.168.100.2",  # two-hop via host
            host_ssh=f"{user}@172.28.4.85",
            host_iface="enp94s0f0np0",
            bf2_iface="p0",
            vip="192.168.56.201",
        ),
    ]
    cfg.workloads = {
        "nginx": WorkloadConfig(
            name="nginx",
            image="nginx:alpine",
            host_port=80,
            health_path="/",
            health_timeout=30,
        ),
    }
    return cfg


def main():
    parser = argparse.ArgumentParser(description="Workload orchestration daemon")
    parser.add_argument("--db-connstr", default=None,
                        help="PostgreSQL connection string")
    parser.add_argument("--poll-interval", type=float, default=5.0,
                        help="Monitoring poll interval in seconds (default: 5)")
    parser.add_argument("--llc-threshold", type=float, default=2.0,
                        help="LLC miss rate threshold multiplier (default: 2.0x baseline)")
    parser.add_argument("--llc-baseline", type=float, default=0.0,
                        help="Known baseline LLC miss rate (0 = auto-calibrate)")
    parser.add_argument("--perf-duration", type=float, default=2.0,
                        help="perf stat sampling duration in seconds (default: 2)")
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
    cfg.llc_threshold = args.llc_threshold
    cfg.llc_baseline = args.llc_baseline
    cfg.perf_duration = args.perf_duration

    orch = Orchestrator(cfg)

    def handle_signal(signum, frame):
        orch.running = False

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    orch.run()


if __name__ == "__main__":
    main()
