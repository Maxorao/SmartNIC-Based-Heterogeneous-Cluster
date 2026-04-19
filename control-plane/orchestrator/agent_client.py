"""
agent_client.py — gRPC client for orchestrator_agent on BF2 nodes.

Replaces the two-hop SSH path with direct gRPC to each BF2 ARM over
the 100G fabric IP (192.168.56.x:50052).

Gracefully degrades: if grpc module is missing or a call fails, the caller
is expected to fall back to SSH.
"""

import logging
import time
from typing import Optional, Dict, Tuple

try:
    import grpc
    from proto import orchestrator_agent_pb2 as oa_pb2
    from proto import orchestrator_agent_pb2_grpc as oa_grpc
    GRPC_AVAILABLE = True
except ImportError:
    GRPC_AVAILABLE = False
    # Caller must check AgentClient.available() before use


class AgentClient:
    """Thin wrapper around the OrchestratorAgent gRPC stub with per-BF2 channel."""

    def __init__(self, bf2_ip: str, port: int = 50052,
                 timeout_s: float = 30.0):
        self.bf2_ip = bf2_ip
        self.port = port
        self.timeout_s = timeout_s
        self.channel = None
        self.stub = None
        self.log = logging.getLogger(f"oa.{bf2_ip}")
        if GRPC_AVAILABLE:
            self._connect()

    def available(self) -> bool:
        return GRPC_AVAILABLE and self.stub is not None

    def _connect(self):
        addr = f"{self.bf2_ip}:{self.port}"
        try:
            self.channel = grpc.insecure_channel(
                addr,
                options=[
                    ("grpc.keepalive_time_ms", 10000),
                    ("grpc.keepalive_timeout_ms", 5000),
                    ("grpc.keepalive_permit_without_calls", 1),
                ],
            )
            self.stub = oa_grpc.OrchestratorAgentStub(self.channel)
        except Exception as e:
            self.log.warning(f"grpc channel init failed: {e}")
            self.stub = None

    def ping(self) -> bool:
        if not self.stub: return False
        try:
            req = oa_pb2.PingRequest(client_timestamp_ns=int(time.time_ns()))
            self.stub.Ping(req, timeout=2.0)
            return True
        except Exception as e:
            self.log.debug(f"ping failed: {e}")
            return False

    def start_container(self, name: str, image: str, network: str = "host",
                         cpuset: str = "", extra_args=None
                         ) -> Tuple[bool, str]:
        if not self.stub: return False, "no grpc"
        req = oa_pb2.StartContainerRequest(
            name=name, image=image, network=network,
            cpuset=cpuset or "",
            extra_args=extra_args or [])
        try:
            resp = self.stub.StartContainer(req, timeout=self.timeout_s)
            if resp.success:
                return True, resp.container_id
            return False, resp.error
        except Exception as e:
            return False, str(e)

    def stop_container(self, name: str, force: bool = True
                        ) -> Tuple[bool, str]:
        if not self.stub: return False, "no grpc"
        req = oa_pb2.StopContainerRequest(name=name, force=force)
        try:
            resp = self.stub.StopContainer(req, timeout=self.timeout_s)
            return resp.success, resp.error
        except Exception as e:
            return False, str(e)

    def health_check(self, target_url: str, max_attempts: int = 30,
                      interval_ms: int = 1000) -> Tuple[bool, int, int]:
        if not self.stub: return False, 0, 0
        req = oa_pb2.HealthCheckRequest(
            target_url=target_url, max_attempts=max_attempts,
            interval_ms=interval_ms)
        try:
            resp = self.stub.HealthCheck(req, timeout=max_attempts + 5)
            return resp.healthy, resp.attempts, resp.duration_ms
        except Exception as e:
            self.log.warning(f"health_check rpc error: {e}")
            return False, 0, 0

    def switch_vip(self, vip: str, interface: str, action: str = "add",
                    prefix_len: int = 24, send_arp: bool = True
                    ) -> Tuple[bool, str, int]:
        if not self.stub: return False, "no grpc", 0
        req = oa_pb2.SwitchVipRequest(
            vip=vip, prefix_len=prefix_len, action=action,
            interface=interface, send_arp=send_arp)
        try:
            resp = self.stub.SwitchVip(req, timeout=5.0)
            return resp.success, resp.error, resp.duration_ms
        except Exception as e:
            return False, str(e), 0

    def run_perf(self, events: str, duration_s: float = 2.0,
                  target_pid: str = "") -> Optional[Dict[str, int]]:
        if not self.stub: return None
        req = oa_pb2.RunPerfRequest(events=events, duration_s=duration_s,
                                     target_pid=target_pid)
        try:
            resp = self.stub.RunPerf(req, timeout=duration_s + 10)
            if resp.success:
                return dict(resp.counters)
            return None
        except Exception as e:
            self.log.warning(f"run_perf rpc error: {e}")
            return None

    def close(self):
        if self.channel is not None:
            self.channel.close()
            self.channel = None
            self.stub = None
