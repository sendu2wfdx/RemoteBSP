"""只读 Runtime 快照的 JSON 契约与严格校验。"""

from __future__ import annotations

import copy
import json
import re


RUNTIME_SNAPSHOT_SCHEMA_VERSION = 1
_IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,95}$")
_SLUG = re.compile(r"^[a-z][a-z0-9_-]{0,47}$")
_NODE_STATES = {"online", "offline", "degraded"}
_LINK_STATES = {"online", "offline", "degraded"}
_SEVERITIES = {"info", "warning", "error", "critical"}


class RuntimeContractError(ValueError):
    """Runtime 快照不满足公开 JSON 契约。"""


def _object(value: object, path: str) -> dict:
    if not isinstance(value, dict):
        raise RuntimeContractError(f"{path}必须是对象")
    return value


def _array(value: object, path: str) -> list:
    if not isinstance(value, list):
        raise RuntimeContractError(f"{path}必须是数组")
    return value


def _string(value: object, path: str, *, identifier: bool = False,
            slug: bool = False, maximum: int = 160) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise RuntimeContractError(f"{path}必须是1～{maximum}字符的字符串")
    if identifier and not _IDENTIFIER.fullmatch(value):
        raise RuntimeContractError(f"{path}不是合法标识符")
    if slug and not _SLUG.fullmatch(value):
        raise RuntimeContractError(f"{path}不是合法类型名")
    return value


def _integer(value: object, path: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise RuntimeContractError(f"{path}必须是不小于{minimum}的整数")
    return value


def _boolean(value: object, path: str) -> bool:
    if not isinstance(value, bool):
        raise RuntimeContractError(f"{path}必须是布尔值")
    return value


def _required(item: dict, key: str, path: str) -> object:
    if key not in item:
        raise RuntimeContractError(f"{path}.{key}不能为空缺省")
    return item[key]


def _json_object(value: object, path: str) -> dict:
    value = _object(value, path)
    try:
        return json.loads(json.dumps(value, ensure_ascii=False,
                                     allow_nan=False))
    except (TypeError, ValueError, json.JSONDecodeError) as error:
        raise RuntimeContractError(f"{path}包含无效JSON值：{error}") from error


def _normalize_link(value: object, path: str) -> dict:
    item = _object(value, path)
    kind = _string(item.get("kind"), path + ".kind", slug=True)
    state = _string(item.get("state"), path + ".state", slug=True)
    if state not in _LINK_STATES:
        raise RuntimeContractError(f"{path}.state不受支持：{state}")
    return {"kind": kind, "state": state}


def _normalize_resource(value: object, path: str) -> dict:
    item = _object(value, path)
    return {
        "resource_id": _string(
            item.get("resource_id"), path + ".resource_id", identifier=True),
        "kind": _string(item.get("kind"), path + ".kind", slug=True),
        "name": _string(item.get("name"), path + ".name"),
        "available": _boolean(item.get("available"), path + ".available"),
        "state": _json_object(_required(item, "state", path),
                              path + ".state"),
    }


def _normalize_node(value: object, path: str) -> dict:
    item = _object(value, path)
    state = _string(item.get("state"), path + ".state", slug=True)
    if state not in _NODE_STATES:
        raise RuntimeContractError(f"{path}.state不受支持：{state}")
    links = [_normalize_link(link, f"{path}.links[{index}]")
             for index, link in enumerate(_array(item.get("links"),
                                                  path + ".links"))]
    resources = [_normalize_resource(resource,
                                     f"{path}.resources[{index}]")
                 for index, resource in enumerate(_array(
                     item.get("resources"), path + ".resources"))]
    resource_ids = [resource["resource_id"] for resource in resources]
    if len(resource_ids) != len(set(resource_ids)):
        raise RuntimeContractError(f"{path}.resources包含重复resource_id")
    link_kinds = [link["kind"] for link in links]
    if len(link_kinds) != len(set(link_kinds)):
        raise RuntimeContractError(f"{path}.links包含重复kind")
    return {
        "node_id": _string(
            item.get("node_id"), path + ".node_id", identifier=True),
        "board_type": _string(
            item.get("board_type"), path + ".board_type", identifier=True),
        "display_name": _string(
            item.get("display_name"), path + ".display_name"),
        "state": state,
        "last_seen_ms": _integer(
            item.get("last_seen_ms"), path + ".last_seen_ms"),
        "links": sorted(links, key=lambda link: link["kind"]),
        "resources": sorted(
            resources, key=lambda resource: resource["resource_id"]),
        "runtime": _json_object(_required(item, "runtime", path),
                                path + ".runtime"),
    }


def _normalize_alert(value: object, path: str) -> dict:
    item = _object(value, path)
    severity = _string(item.get("severity"), path + ".severity", slug=True)
    if severity not in _SEVERITIES:
        raise RuntimeContractError(f"{path}.severity不受支持：{severity}")
    resource_id = _required(item, "resource_id", path)
    if resource_id is not None:
        resource_id = _string(
            resource_id, path + ".resource_id", identifier=True)
    return {
        "alert_id": _string(
            item.get("alert_id"), path + ".alert_id", identifier=True),
        "node_id": _string(
            item.get("node_id"), path + ".node_id", identifier=True),
        "resource_id": resource_id,
        "severity": severity,
        "code": _string(item.get("code"), path + ".code", slug=True),
        "message": _string(item.get("message"), path + ".message",
                           maximum=500),
        "active": _boolean(item.get("active"), path + ".active"),
        "occurred_at_ms": _integer(
            item.get("occurred_at_ms"), path + ".occurred_at_ms"),
    }


def normalize_snapshot(value: object) -> dict:
    """校验并返回排序稳定、与输入对象互不共享的 v1 快照。"""
    root = _object(value, "snapshot")
    version = root.get("schema_version")
    if version != RUNTIME_SNAPSHOT_SCHEMA_VERSION or isinstance(version, bool):
        raise RuntimeContractError(
            f"snapshot.schema_version必须为{RUNTIME_SNAPSHOT_SCHEMA_VERSION}")
    nodes = [_normalize_node(node, f"snapshot.nodes[{index}]")
             for index, node in enumerate(_array(root.get("nodes"),
                                                  "snapshot.nodes"))]
    node_ids = [node["node_id"] for node in nodes]
    if len(node_ids) != len(set(node_ids)):
        raise RuntimeContractError("snapshot.nodes包含重复node_id")
    nodes_by_id = {node["node_id"]: node for node in nodes}

    alerts = [_normalize_alert(alert, f"snapshot.alerts[{index}]")
              for index, alert in enumerate(_array(root.get("alerts"),
                                                    "snapshot.alerts"))]
    alert_ids = [alert["alert_id"] for alert in alerts]
    if len(alert_ids) != len(set(alert_ids)):
        raise RuntimeContractError("snapshot.alerts包含重复alert_id")
    for alert in alerts:
        node = nodes_by_id.get(alert["node_id"])
        if node is None:
            raise RuntimeContractError(
                f"告警{alert['alert_id']}引用了未知节点{alert['node_id']}")
        resource_id = alert["resource_id"]
        if resource_id is not None and resource_id not in {
                item["resource_id"] for item in node["resources"]}:
            raise RuntimeContractError(
                f"告警{alert['alert_id']}引用了未知资源{resource_id}")

    normalized = {
        "schema_version": RUNTIME_SNAPSHOT_SCHEMA_VERSION,
        "snapshot_id": _string(
            root.get("snapshot_id"), "snapshot.snapshot_id", identifier=True),
        "captured_at_ms": _integer(
            root.get("captured_at_ms"), "snapshot.captured_at_ms"),
        "nodes": sorted(nodes, key=lambda node: node["node_id"]),
        "alerts": sorted(alerts, key=lambda alert: alert["alert_id"]),
    }
    return copy.deepcopy(normalized)
