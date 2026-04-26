#!/usr/bin/env python3
"""agent_simulator.py — simulate the CPU footprint of a Kubernetes monitoring
sidecar.

In production a worker node typically runs several lightweight monitoring
processes alongside kubelet: health probes, log collectors, node-exporter,
kube-proxy, container-runtime helpers. Each scans /proc periodically,
formats metrics, and writes to a sink. This script reproduces that pattern
so we can measure the cumulative CPU competition with the workload.

Each iteration:
  - Walk /proc, read /proc/<pid>/stat for SAMPLE_PIDS random PIDs.
  - Read /proc/stat and /proc/meminfo.
  - Format the data as JSON and hash it (simulates metric serialization).
  - Optionally append a short line to a log file (simulates log shipping).
  - Sleep INTERVAL_S between iterations.

Configurable via env vars:
  SIM_ROLE        descriptive role name (kubelet | health-probe | …).
  SIM_INTERVAL_S  loop period in seconds (default 0.1).
  SIM_SAMPLE_PIDS how many /proc/<pid>/stat to read per cycle (default 50).
  SIM_LOG         optional path to write a one-line log entry per cycle.
"""

import hashlib
import json
import os
import random
import sys
import time


def main() -> None:
    interval_s = float(os.environ.get("SIM_INTERVAL_S", "0.1"))
    sample_pids = int(os.environ.get("SIM_SAMPLE_PIDS", "50"))
    role = os.environ.get("SIM_ROLE", "agent")
    log_path = os.environ.get("SIM_LOG", "")

    sys.stderr.write(f"[sim] role={role} interval={interval_s}s pids={sample_pids}"
                     f" log={'yes' if log_path else 'no'}\n")
    sys.stderr.flush()

    while True:
        t0 = time.time()

        # Enumerate live PIDs.
        pids = [int(p) for p in os.listdir("/proc") if p.isdigit()]
        if not pids:
            time.sleep(interval_s)
            continue

        # Sample a subset (kubelet samples a few hundred per cycle in practice).
        chosen = random.sample(pids, min(sample_pids, len(pids)))
        data = []
        for pid in chosen:
            try:
                with open(f"/proc/{pid}/stat") as f:
                    line = f.readline()
                fields = line.split()
                # Only keep a handful of fields, like a real exporter would.
                data.append({
                    "pid": pid,
                    "comm": fields[1],
                    "state": fields[2],
                    "utime": int(fields[13]),
                    "stime": int(fields[14]),
                    "rss": int(fields[23]),
                })
            except (FileNotFoundError, ProcessLookupError, IndexError):
                # Process exited mid-scan — kubelet handles this all the time.
                continue

        # System-wide metrics.
        try:
            with open("/proc/stat") as f:
                stat = f.read()
            with open("/proc/meminfo") as f:
                meminfo = f.read()
        except OSError:
            stat = meminfo = ""

        # Format + hash (mimics serializer + signature work in prom-style agents).
        payload = json.dumps({
            "role": role,
            "ts": t0,
            "samples": data,
            "stat_len": len(stat),
            "meminfo_len": len(meminfo),
        })
        digest = hashlib.sha256(payload.encode()).hexdigest()

        if log_path:
            try:
                with open(log_path, "a") as f:
                    f.write(f"{t0:.3f} {role} pids={len(data)} hash={digest[:16]}\n")
            except OSError:
                pass

        # Sleep with drift correction.
        elapsed = time.time() - t0
        sleep_for = interval_s - elapsed
        if sleep_for > 0:
            time.sleep(sleep_for)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
