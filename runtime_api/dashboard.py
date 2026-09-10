"""Runtime Web 单板状态投影与有界趋势窗口。"""

from __future__ import annotations

import copy
import threading


DEFAULT_TREND_CAPACITY = 60
MAXIMUM_TREND_CAPACITY = 600


class RuntimeDashboard:
    """把已校验快照投影成适合下钻展示的只读模型。"""

    def __init__(self, capacity: int = DEFAULT_TREND_CAPACITY):
        if type(capacity) is not int or not 1 <= capacity <= MAXIMUM_TREND_CAPACITY:
            raise ValueError("趋势容量必须位于1～600")
        self.capacity = capacity
        self._series: dict[str, list[dict]] = {}
        self._health_series: list[dict] = []
        self._last_health_sample: tuple[int, int] | None = None
        self._last_snapshot_id: str | None = None
        self._lock = threading.Lock()

    @staticmethod
    def _resource(resource: dict, alerts: list[dict]) -> dict:
        # available 是资源枚举的事实；缺失的健康测量不能由空 state 或数值 0 推断。
        availability = "available" if resource["available"] else "unavailable"
        health = resource["state"].get("health")
        if health not in {"healthy", "degraded", "fault", "unknown"}:
            health = "unknown"
        return {
            **copy.deepcopy(resource),
            "availability": availability,
            "health": health,
            "active_alerts": [copy.deepcopy(item) for item in alerts
                              if item["resource_id"] == resource["resource_id"]
                              and item["active"]],
        }

    @staticmethod
    def _toolbusd_health(value: dict | None) -> dict:
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
        threshold_alerts = []
        for metric in metrics:
            if metric["availability"] != "available" or \
                    metric.get("name") not in {
                        "cpu_load_permille", "isr_load_permille"}:
                continue
            measured = metric["value"]
            severity = "critical" if measured >= 950 else \
                "warning" if measured >= 800 else None
            if severity is not None:
                threshold_alerts.append({
                    "metric": metric["name"], "severity": severity,
                    "value": measured, "unit": metric.get("unit"),
                    "warning_threshold": 800, "critical_threshold": 950,
                })
        return {"availability": "available", "overall": overall,
                "metrics": metrics, "threshold_alerts": threshold_alerts}

    @classmethod
    def _node_health(cls, value: object) -> dict:
        if not isinstance(value, dict):
            return {"availability": "unknown", "reason":
                    "node_health_missing", "sample_age_ms": None,
                    "overall": "unknown", "metrics": []}
        availability = value.get("availability")
        if availability not in {"available", "unavailable", "unknown"}:
            availability = "unknown"
        projected = cls._toolbusd_health({
            "available": availability == "available",
            "snapshot": value.get("snapshot"),
        })
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

    def observe(self, snapshot: dict, toolbusd_health: dict | None = None) -> dict:
        snapshot_id = snapshot["snapshot_id"]
        projected_health = self._toolbusd_health(toolbusd_health)
        with self._lock:
            if snapshot_id != self._last_snapshot_id:
                for node in snapshot["nodes"]:
                    samples = self._series.setdefault(node["node_id"], [])
                    values = {
                        name: value for name, value in node["runtime"].items()
                        if type(value) is int
                    }
                    samples.append({"captured_at_ms": snapshot["captured_at_ms"],
                                    "values": values})
                    del samples[:-self.capacity]
                live = {node["node_id"] for node in snapshot["nodes"]}
                for node_id in tuple(self._series):
                    if node_id not in live:
                        del self._series[node_id]
                self._last_snapshot_id = snapshot_id

            raw_health = toolbusd_health.get("snapshot") if isinstance(
                toolbusd_health, dict) else None
            if projected_health["availability"] == "available" and isinstance(
                    raw_health, dict):
                sample_key = (raw_health.get("producer_generation"),
                              raw_health.get("sample_sequence"))
                if all(type(value) is int for value in sample_key) and \
                        sample_key != self._last_health_sample:
                    values = {item["name"]: item["value"]
                              for item in projected_health["metrics"]
                              if item["availability"] == "available"}
                    self._health_series.append({
                        "sample_time_ms": raw_health.get("sample_time_ms"),
                        "values": values})
                    del self._health_series[:-self.capacity]
                    self._last_health_sample = sample_key

            nodes = []
            for node in snapshot["nodes"]:
                node_alerts = [item for item in snapshot["alerts"]
                               if item["node_id"] == node["node_id"]]
                samples = copy.deepcopy(self._series.get(node["node_id"], []))
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
                "samples": copy.deepcopy(self._health_series),
                "peaks": health_peaks,
            }
            return {
                "schema_version": 1,
                "snapshot_id": snapshot_id,
                "captured_at_ms": snapshot["captured_at_ms"],
                "nodes": nodes,
                "toolbusd_health": projected_health,
            }
