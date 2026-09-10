"""Runtime Web 单板状态投影与有界趋势窗口。"""

from __future__ import annotations

import copy
import threading

from .alert_rules import AlertRuleEvaluator, AlertRuleManager
from .trend_store import MAXIMUM_NODES, RuntimeTrendStore


DEFAULT_TREND_CAPACITY = 60
MAXIMUM_TREND_CAPACITY = 600


class RuntimeDashboard:
    """把已校验快照投影成适合下钻展示的只读模型。"""

    def __init__(self, capacity: int = DEFAULT_TREND_CAPACITY,
                 store: RuntimeTrendStore | None = None,
                 alert_rules: AlertRuleManager | None = None):
        if type(capacity) is not int or not 1 <= capacity <= MAXIMUM_TREND_CAPACITY:
            raise ValueError("趋势容量必须位于1～600")
        self.capacity = capacity
        if store is not None and store.capacity != capacity:
            raise ValueError("趋势存储容量必须与仪表盘容量一致")
        self._store = store
        restored = store.load() if store is not None else {
            "nodes": {}, "toolbusd_health": []}
        self._series: dict[str, list[dict]] = restored["nodes"]
        self._health_series: list[dict] = restored["toolbusd_health"]
        self._lock = threading.Lock()
        self._alert_evaluator = AlertRuleEvaluator(
            alert_rules or AlertRuleManager())

    def storage_status(self) -> dict:
        """仅公开配置与容量，不公开本地持久化路径。"""
        return {
            "availability": "available",
            "configured": self._store is not None,
            "persistence": "enabled" if self._store is not None else "disabled",
            "sample_capacity_per_series": self.capacity,
            "maximum_file_bytes": (
                self._store.maximum_bytes if self._store is not None else None),
        }

    @staticmethod
    def _public_sample(sample: dict, *, health: bool = False) -> dict:
        return {
            "sample_time_ms" if health else "captured_at_ms": sample["time_ms"],
            "values": copy.deepcopy(sample["values"]),
        }

    @staticmethod
    def _resource(resource: dict, alerts: list[dict]) -> dict:
        # available 是资源枚举的事实；缺失的健康测量不能由空 state 或数值 0 推断。
        availability = "available" if resource["available"] else "unavailable"
        health = resource["state"].get("health")
        if health not in {"healthy", "degraded", "fault", "unknown"}:
            health = "unknown"
        projected = {
            **copy.deepcopy(resource),
            "availability": availability,
            "health": health,
            "active_alerts": [copy.deepcopy(item) for item in alerts
                              if item["resource_id"] == resource["resource_id"]
                              and item["active"]],
        }
        if resource.get("kind") in {"i2c_device", "spi_device"}:
            detail = resource.get("state", {}).get("bus_health")
            if not isinstance(detail, dict):
                detail = {"availability": "unknown", "last_status": "unknown",
                          "last_result_age_ms": None,
                          "consecutive_failures": 0,
                          "peak_consecutive_failures": 0,
                          "cumulative_failures": None,
                          "cumulative_availability": "unavailable"}
            projected["bus_health"] = copy.deepcopy(detail)
        return projected

    def _toolbusd_health(self, value: dict | None, *, scope: str = "toolbusd") -> dict:
        if not isinstance(value, dict) or value.get("available") is not True:
            return {"availability": "unknown", "overall": "unknown",
                    "metrics": []}
        snapshot = value.get("snapshot")
        if not isinstance(snapshot, dict):
            return {"availability": "unknown", "overall": "unknown",
                    "metrics": []}
        metrics = []
        for metric in snapshot.get("metrics", []):
            if not isinstance(metric, dict):
                continue
            availability = metric.get("availability")
            if availability not in {"available", "unavailable", "unknown"}:
                availability = "unknown"
            measured = metric.get("value") if availability == "available" else None
            if isinstance(measured, bool) or not isinstance(measured, int):
                measured = None
                if availability == "available":
                    availability = "unknown"
            metrics.append({**copy.deepcopy(metric), "availability": availability,
                            "value": measured})
        overall = snapshot.get("overall")
        if overall not in {"healthy", "degraded", "fault", "unknown"}:
            overall = "unknown"
        threshold_alerts = self._alert_evaluator.evaluate(metrics, scope=scope)
        return {"availability": "available", "overall": overall,
                "metrics": metrics, "threshold_alerts": threshold_alerts}

    def _node_health(self, value: object) -> dict:
        if not isinstance(value, dict):
            return {"availability": "unknown", "reason":
                    "node_health_missing", "sample_age_ms": None,
                    "overall": "unknown", "metrics": []}
        availability = value.get("availability")
        if availability not in {"available", "unavailable", "unknown"}:
            availability = "unknown"
        source = value.get("snapshot") if isinstance(value.get("snapshot"), dict) else {}
        scope = source.get("node_uuid") if isinstance(source.get("node_uuid"), str) else "node"
        projected = self._toolbusd_health({
            "available": availability == "available",
            "snapshot": value.get("snapshot"),
        }, scope=scope)
        return {
            "availability": availability,
            "reason": value.get("reason") if isinstance(
                value.get("reason"), str) else None,
            "sample_age_ms": value.get("sample_age_ms") if type(
                value.get("sample_age_ms")) is int else None,
            "overall": projected["overall"],
            "metrics": projected["metrics"],
            "threshold_alerts": projected.get("threshold_alerts", []),
        }

    def observe(self, snapshot: dict, toolbusd_health: dict | None = None,
                bus_reset_operations: list[dict] | None = None) -> dict:
        snapshot_id = snapshot["snapshot_id"]
        projected_health = self._toolbusd_health(toolbusd_health)
        with self._lock:
            changed = False
            for node in snapshot["nodes"]:
                samples = self._series.setdefault(node["node_id"], [])
                identity = [snapshot_id, snapshot["captured_at_ms"]]
                if not any(item["identity"] == identity for item in samples):
                    values = {
                        name: value for name, value in node["runtime"].items()
                        if type(value) is int and value >= 0
                    }
                    samples.append({"identity": identity,
                                    "time_ms": snapshot["captured_at_ms"],
                                    "values": values})
                    del samples[:-self.capacity]
                    changed = True
            live_node_ids = {node["node_id"] for node in snapshot["nodes"]}
            while len(self._series) > MAXIMUM_NODES:
                removable = [
                    (samples[-1]["time_ms"] if samples else -1, node_id)
                    for node_id, samples in self._series.items()
                    if node_id not in live_node_ids]
                if not removable:
                    break
                del self._series[min(removable)[1]]
                changed = True

            raw_health = toolbusd_health.get("snapshot") if isinstance(
                toolbusd_health, dict) else None
            if projected_health["availability"] == "available" and isinstance(
                    raw_health, dict):
                sample_key = (raw_health.get("producer_generation"),
                              raw_health.get("sample_sequence"))
                if all(type(value) is int for value in sample_key) and \
                        type(raw_health.get("sample_time_ms")) is int and \
                        raw_health["sample_time_ms"] >= 0 and \
                        not any(item["identity"] == list(sample_key)
                                for item in self._health_series):
                    values = {item["name"]: item["value"]
                              for item in projected_health["metrics"]
                              if item["availability"] == "available"}
                    self._health_series.append({
                        "identity": list(sample_key),
                        "time_ms": raw_health.get("sample_time_ms"),
                        "values": values})
                    del self._health_series[:-self.capacity]
                    changed = True

            if changed and self._store is not None:
                self._store.save({"nodes": self._series,
                                  "toolbusd_health": self._health_series})

            nodes = []
            for node in snapshot["nodes"]:
                node_alerts = [item for item in snapshot["alerts"]
                               if item["node_id"] == node["node_id"]]
                samples = [self._public_sample(item) for item in
                           self._series.get(node["node_id"], [])]
                peaks: dict[str, int] = {}
                for sample in samples:
                    for name, value in sample["values"].items():
                        peaks[name] = max(value, peaks.get(name, value))
                nodes.append({
                    "node_id": node["node_id"],
                    "display_name": node["display_name"],
                    "board_type": node["board_type"],
                    "state": node["state"],
                    "links": copy.deepcopy(node["links"]),
                    "runtime": copy.deepcopy(node["runtime"]),
                    "health": self._node_health(
                        node["runtime"].get("health_snapshot")),
                    "resources": [self._resource(item, node_alerts)
                                  for item in node["resources"]],
                    "active_alerts": [copy.deepcopy(item) for item in node_alerts
                                      if item["active"]],
                    "trend": {"capacity": self.capacity, "samples": samples,
                              "peaks": peaks},
                })
            health_peaks: dict[str, int] = {}
            for sample in self._health_series:
                for name, value in sample["values"].items():
                    health_peaks[name] = max(value, health_peaks.get(name, value))
            projected_health["trend"] = {
                "capacity": self.capacity,
                "samples": [self._public_sample(item, health=True)
                            for item in self._health_series],
                "peaks": health_peaks,
            }
            return {
                "schema_version": 1,
                "snapshot_id": snapshot_id,
                "captured_at_ms": snapshot["captured_at_ms"],
                "nodes": nodes,
                "toolbusd_health": projected_health,
                "bus_reset_operations": copy.deepcopy(
                    (bus_reset_operations or [])[-256:]),
            }
