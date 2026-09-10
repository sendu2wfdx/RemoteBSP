"""HealthSnapshot v1 的严格只读解码、可信路由绑定与稳定 API 投影。"""

from __future__ import annotations

import copy
import struct
import threading
from dataclasses import dataclass


HEALTH_CONTRACT_VERSION = 1
HEALTH_SOURCE_TOOLBUSD = 3
MAXIMUM_HEALTH_METRICS = 48
_HEADER_SIZE = 36
_METRIC_SIZE = 12

_OVERALL = {0: "unknown", 1: "healthy", 2: "degraded", 3: "fault"}
_AVAILABILITY = {1: "available", 2: "unavailable", 3: "unknown"}
_UNIT = {
    1: "count",
    2: "bytes",
    3: "permille",
    4: "milliseconds",
    5: "generation",
    0x80: "nanoseconds",
}
_METRIC = {
    1: ("cpu_load_permille", 3),
    2: ("isr_load_permille", 3),
    3: ("minimum_stack_free_bytes", 2),
    4: ("request_queue_depth", 1),
    5: ("request_queue_capacity", 1),
    6: ("motion_queue_depth", 1),
    7: ("motion_queue_capacity", 1),
    8: ("stream_buffered_bytes", 2),
    9: ("stream_buffer_capacity_bytes", 2),
    10: ("retry_total", 1),
    11: ("timeout_total", 1),
    12: ("duplicate_response_total", 1),
    13: ("unexpected_response_total", 1),
    14: ("rx_frame_total", 1),
    15: ("tx_frame_total", 1),
    16: ("dropped_frame_total", 1),
    17: ("boot_generation", 5),
    18: ("session_generation", 5),
    19: ("clock_model_generation", 5),
    20: ("uptime_milliseconds", 4),
    21: ("active_lease_count", 1),
    22: ("resource_fault_count", 1),
    23: ("sample_overrun_total", 1),
    0x8001: ("traffic_admitted_packet_total", 1),
    0x8002: ("traffic_rejected_packet_total", 1),
    0x8003: ("traffic_guaranteed_overrun_total", 1),
    0x8004: ("traffic_admitted_frame_total", 1),
    0x8005: ("traffic_estimated_wire_time_ns", 0x80),
}


class HealthProjectionError(ValueError):
    """载荷或可信接线不满足 HealthSnapshot v1 约束。"""


@dataclass(frozen=True)
class _DecodedSnapshot:
    source: int
    overall: int
    sequence: int
    sample_time_ms: int
    node_id: int
    producer_generation: int
    metrics: tuple[tuple[int, int, int, int], ...]


def _decode(payload: bytes) -> _DecodedSnapshot:
    if not isinstance(payload, bytes):
        raise HealthProjectionError("健康载荷必须是 bytes")
    if len(payload) < _HEADER_SIZE:
        raise HealthProjectionError("健康载荷过短")
    if len(payload) > _HEADER_SIZE + MAXIMUM_HEALTH_METRICS * _METRIC_SIZE:
        raise HealthProjectionError("健康载荷超过资源上限")
    version, source, overall, sequence, sample_time_ms, node_id, generation, \
        count, reserved = struct.unpack_from("<HBBQQIQHH", payload)
    if version != HEALTH_CONTRACT_VERSION:
        raise HealthProjectionError("健康契约版本不受支持")
    if source == 0 or overall not in _OVERALL or sequence == 0 or \
            node_id > 127 or generation == 0 or reserved != 0:
        raise HealthProjectionError("健康载荷头字段无效")
    if count < 1 or count > MAXIMUM_HEALTH_METRICS or \
            len(payload) != _HEADER_SIZE + count * _METRIC_SIZE:
        raise HealthProjectionError("健康载荷长度或指标数量无效")

    metrics: list[tuple[int, int, int, int]] = []
    previous_id = 0
    for index in range(count):
        metric_id, availability, unit, value = struct.unpack_from(
            "<HBBQ", payload, _HEADER_SIZE + index * _METRIC_SIZE)
        if metric_id == 0 or metric_id <= previous_id:
            raise HealthProjectionError("健康指标必须按 ID 严格递增")
        previous_id = metric_id
        if availability not in _AVAILABILITY or unit == 0:
            raise HealthProjectionError("健康指标状态或单位无效")
        if availability != 1 and value != 0:
            raise HealthProjectionError("非 available 指标不得携带数值")
        known = _METRIC.get(metric_id)
        if known is not None and unit != known[1]:
            raise HealthProjectionError("健康指标单位与契约不匹配")
        if availability == 1 and metric_id in {1, 2} and value > 1000:
            raise HealthProjectionError("CPU 或 ISR 负载超出千分比范围")
        if availability == 1 and metric_id in {17, 18, 19} and value == 0:
            raise HealthProjectionError("可用的代际指标必须非零")
        metrics.append((metric_id, availability, unit, value))

    by_id = {item[0]: item for item in metrics}
    session_generation = by_id.get(18)
    if session_generation is not None and session_generation[1] == 1 and \
            session_generation[3] != generation:
        raise HealthProjectionError("会话代际指标与生产者代际不一致")
    for depth_id, capacity_id in ((4, 5), (6, 7), (8, 9)):
        depth = by_id.get(depth_id)
        capacity = by_id.get(capacity_id)
        if capacity is not None and capacity[1] == 1 and capacity[3] == 0:
            raise HealthProjectionError("健康队列容量不能为零")
        if depth is not None and capacity is not None and \
                depth[1] == capacity[1] == 1 and depth[3] > capacity[3]:
            raise HealthProjectionError("健康队列深度超过容量")
    return _DecodedSnapshot(source, overall, sequence, sample_time_ms,
                            node_id, generation, tuple(metrics))


def _project(snapshot: _DecodedSnapshot) -> dict:
    metrics = []
    for metric_id, availability, unit, value in snapshot.metrics:
        known = _METRIC.get(metric_id)
        metrics.append({
            "metric_id": metric_id,
            "name": known[0] if known is not None else
                    f"unknown_metric_{metric_id}",
            "availability": _AVAILABILITY[availability],
            "unit": _UNIT.get(unit, f"unknown_unit_{unit}"),
            # 0 只有在 available 时才是测量值；其余状态统一投影成 null。
            "value": value if availability == 1 else None,
        })
    return {
        "contract_version": HEALTH_CONTRACT_VERSION,
        "source": "toolbusd" if snapshot.source == HEALTH_SOURCE_TOOLBUSD
                  else f"unknown_source_{snapshot.source}",
        "node_id": snapshot.node_id,
        "producer_generation": snapshot.producer_generation,
        "sample_sequence": snapshot.sequence,
        "sample_time_ms": snapshot.sample_time_ms,
        "overall": _OVERALL[snapshot.overall],
        "metrics": metrics,
    }


class TrustedToolbusdHealthProjection:
    """将本地可信 daemon 世代绑定到单调、幂等的只读健康投影。"""

    def __init__(self, producer_generation: int):
        if type(producer_generation) is not int or not \
                1 <= producer_generation <= 0xFFFFFFFFFFFFFFFF:
            raise HealthProjectionError("可信 toolbusd 代际无效")
        self._generation = producer_generation
        self._last_sequence = 0
        self._last_sample_time_ms = 0
        self._last_payload: bytes | None = None
        self._last_projection: dict | None = None
        self._lock = threading.Lock()

    def replace_generation(self, producer_generation: int) -> None:
        """daemon 身份变化后由接线层显式切换，旧代际从此不能重新进入。"""
        if type(producer_generation) is not int or not \
                1 <= producer_generation <= 0xFFFFFFFFFFFFFFFF:
            raise HealthProjectionError("可信 toolbusd 代际无效")
        with self._lock:
            if producer_generation == self._generation:
                return
            self._generation = producer_generation
            self._last_sequence = 0
            self._last_sample_time_ms = 0
            self._last_payload = None
            self._last_projection = None

    def ingest(self, payload: bytes) -> dict:
        # 解码在锁外完成；失败不会改变已接受的可信状态。
        decoded = _decode(payload)
        if decoded.source != HEALTH_SOURCE_TOOLBUSD or decoded.node_id != 0:
            raise HealthProjectionError("健康载荷不是本地 toolbusd 来源")
        with self._lock:
            if decoded.producer_generation != self._generation:
                raise HealthProjectionError("健康载荷与当前 daemon 代际不匹配")
            if decoded.sequence == self._last_sequence:
                if payload != self._last_payload or self._last_projection is None:
                    raise HealthProjectionError("相同采样序号出现不同载荷")
                return copy.deepcopy(self._last_projection)
            if decoded.sequence < self._last_sequence:
                raise HealthProjectionError("健康载荷采样序号陈旧")
            if self._last_sequence != 0 and \
                    decoded.sample_time_ms < self._last_sample_time_ms:
                raise HealthProjectionError("健康载荷采样时钟回退")
            projected = _project(decoded)
            self._last_sequence = decoded.sequence
            self._last_sample_time_ms = decoded.sample_time_ms
            self._last_payload = payload
            self._last_projection = projected
            return copy.deepcopy(projected)
