#!/usr/bin/env python3
"""生产角色授权、一次性字段与批量烧号的失败关闭软件闭环。"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import tempfile
import secrets
from pathlib import Path

from device_parameters import DeviceParameterError, WRITE_CONFIRMATION
from parameter_audit import parameter_evidence

POLICY_FORMAT = "REMOTEBSP_PROVISIONING_POLICY_V1"
BATCH_FORMAT = "REMOTEBSP_PROVISIONING_BATCH_V1"
_ID = re.compile(r"[a-zA-Z0-9_.-]{1,64}")
_UUID = re.compile(r"[0-9a-f]{32}")


def _digest(value):
    return hashlib.sha256(json.dumps({k: v for k, v in value.items() if k != "sha256"},
        ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":")).encode()).hexdigest()


def create_policy(*, operators: list[dict], parameter_rules: list[dict]) -> dict:
    policy = {"format": POLICY_FORMAT, "schema_version": 1,
              "operators": operators, "parameter_rules": parameter_rules}
    policy["sha256"] = _digest(policy)
    return validate_policy(policy)


def validate_policy(value: object) -> dict:
    if not isinstance(value, dict) or set(value) != {"format", "schema_version",
            "operators", "parameter_rules", "sha256"} or \
            value.get("format") != POLICY_FORMAT or value.get("schema_version") != 1 or \
            value.get("sha256") != _digest(value):
        raise DeviceParameterError("生产授权策略格式或SHA-256无效")
    operators, rules = value["operators"], value["parameter_rules"]
    if not isinstance(operators, list) or not 1 <= len(operators) <= 64 or \
            not isinstance(rules, list) or not 1 <= len(rules) <= 64:
        raise DeviceParameterError("生产授权策略数量越界")
    seen = set()
    for item in operators:
        if not isinstance(item, dict) or set(item) != {"operator_id", "role"} or \
                not _ID.fullmatch(str(item.get("operator_id", ""))) or \
                item.get("role") not in ("provisioner", "supervisor") or \
                item["operator_id"] in seen:
            raise DeviceParameterError("生产操作员重复、角色或ID无效")
        seen.add(item["operator_id"])
    seen.clear()
    for item in rules:
        if not isinstance(item, dict) or set(item) != {"parameter_id", "strategy"} or \
                type(item.get("parameter_id")) is not int or not 0 <= item["parameter_id"] <= 0xffff or \
                item.get("strategy") not in ("replaceable", "write_once") or \
                item["parameter_id"] in seen:
            raise DeviceParameterError("生产参数规则重复或无效")
        seen.add(item["parameter_id"])
    return value


class ProvisioningBatchController:
    def __init__(self, *, policy: dict, journal_root: Path,
                 manager_factory, audit_factory):
        self.policy = validate_policy(policy)
        if journal_root.exists() and (journal_root.is_symlink() or not journal_root.is_dir()):
            raise DeviceParameterError("批量烧号日志目录无效")
        journal_root.mkdir(parents=False, exist_ok=True)
        self.root = journal_root; self.manager_factory = manager_factory
        self.audit_factory = audit_factory

    def execute(self, batch: object) -> dict:
        if not isinstance(batch, dict) or not _ID.fullmatch(
                str(batch.get("batch_id", ""))):
            raise DeviceParameterError("批量烧号批次ID无效")
        lock_path = self.root / f"batch-{batch['batch_id']}.lock"
        lock_token = secrets.token_hex(16)
        try:
            descriptor = os.open(lock_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                                 0o600)
        except FileExistsError as error:
            raise DeviceParameterError("相同批次正在由另一进程执行，拒绝并发") from error
        try:
            with os.fdopen(descriptor, "w", encoding="ascii") as stream:
                stream.write(f"pid={os.getpid()} token={lock_token}\n")
                stream.flush(); os.fsync(stream.fileno())
            return self._execute_locked(batch)
        finally:
            try:
                if lock_path.read_text(encoding="ascii") == \
                        f"pid={os.getpid()} token={lock_token}\n":
                    lock_path.unlink()
            except (OSError, UnicodeError):
                # 无法证明仍是本进程的锁时保留它，失败关闭而不是误删他人锁。
                pass

    def _execute_locked(self, batch: object) -> dict:
        if not isinstance(batch, dict) or set(batch) != {"format", "schema_version",
                "batch_id", "operator_id", "supervisor_id", "devices", "sha256"} or \
                batch.get("format") != BATCH_FORMAT or batch.get("schema_version") != 1 or \
                batch.get("sha256") != _digest(batch) or \
                not _ID.fullmatch(str(batch.get("batch_id", ""))):
            raise DeviceParameterError("批量烧号请求格式或SHA-256无效")
        roles = {x["operator_id"]: x["role"] for x in self.policy["operators"]}
        if roles.get(batch["operator_id"]) != "provisioner":
            raise DeviceParameterError("批量烧号需要已授权provisioner")
        if batch.get("supervisor_id") == batch.get("operator_id"):
            raise DeviceParameterError("supervisor必须与provisioner为独立身份")
        rules = {x["parameter_id"]: x["strategy"] for x in self.policy["parameter_rules"]}
        devices = batch["devices"]
        if not isinstance(devices, list) or not 1 <= len(devices) <= 256:
            raise DeviceParameterError("批量烧号设备数量必须位于1～256")
        path = self.root / f"batch-{batch['batch_id']}.json"
        if path.exists():
            existing = json.loads(path.read_text(encoding="utf-8"))
            self._validate_result(existing)
            if existing.get("request_sha256") != batch["sha256"]:
                raise DeviceParameterError("批次ID已存在但请求内容冲突")
            return self._response(existing, replayed=True)
        uuids = set(); prepared = []
        for device in devices:
            if not isinstance(device, dict) or set(device) != {"uuid", "generation", "parameters"} or \
                    not _UUID.fullmatch(str(device.get("uuid", ""))) or device["uuid"] in uuids or \
                    type(device.get("generation")) is not int or \
                    not 0 <= device["generation"] <= 0xffffffff or \
                    not isinstance(device.get("parameters"), list) or \
                    not 1 <= len(device["parameters"]) <= 64:
                raise DeviceParameterError("批量烧号目标重复或字段无效")
            uuids.add(device["uuid"]); seen = set(); records = []
            for record in device["parameters"]:
                if not isinstance(record, dict) or set(record) != {"id", "value_base64"} or \
                        record.get("id") in seen or record.get("id") not in rules:
                    raise DeviceParameterError("批量烧号参数重复、未授权或字段无效")
                seen.add(record["id"])
                try: raw = base64.b64decode(record["value_base64"], validate=True)
                except (TypeError, ValueError) as error:
                    raise DeviceParameterError("批量烧号参数值不是合法Base64") from error
                if not raw or len(raw) > 64:
                    raise DeviceParameterError("批量烧号参数值长度无效")
                records.append((record, raw))
            prepared.append((device, records))
        if any(rules[r["id"]] == "write_once" for _, rs in prepared for r, _ in rs) and \
                roles.get(batch["supervisor_id"]) != "supervisor":
            raise DeviceParameterError("一次性字段需要独立supervisor授权")
        checked = []
        # 整批在首个写入前完成UUID、代数和一次性字段预检，避免可预见的半批写入。
        for device, records in prepared:
            manager = self.manager_factory(device["uuid"])
            snapshot = manager.snapshot()
            current = {x["id"]: x for x in snapshot["parameters"]}
            if snapshot["node_uuid"] != device["uuid"] or \
                    snapshot["status"]["generation"] != device["generation"]:
                raise DeviceParameterError("批量烧号目标UUID或代数冲突")
            for record, _ in records:
                if rules[record["id"]] == "write_once" and \
                        current.get(record["id"], {}).get("byte_count") != 0:
                    raise DeviceParameterError("一次性字段已经存在，拒绝覆盖")
            checked.append((device, records, manager))
        results = []
        for device, records, manager in checked:
            generation = device["generation"]
            for record, raw in records:
                audit = self.audit_factory(device["uuid"])
                operation = audit.begin(operation="write", node_id=manager.node_id,
                    node_uuid=device["uuid"], expected_generation=generation,
                    parameters=[parameter_evidence(record["id"], raw)],
                    operator={"identity": batch["operator_id"],
                              "role": "provisioner",
                              "policy_sha256": self.policy["sha256"]})
                try:
                    snapshot = manager.write(expected_uuid=device["uuid"],
                        expected_generation=generation, parameter_id=record["id"],
                        value_base64=record["value_base64"], confirmation=WRITE_CONFIRMATION)
                    after = snapshot["status"]["generation"]
                    audit.finish(operation, outcome="success", generation_after=after,
                                 applied_count=after-generation)
                    generation = after
                except Exception as error:
                    audit.finish(operation, outcome="failure", generation_after=None,
                                 applied_count=0, error_type=type(error).__name__)
                    self._save(path, batch, results, "failed", type(error).__name__)
                    raise
            results.append({"uuid": device["uuid"], "generation_after": generation,
                            "status": "software_write_completed_unverified_hardware"})
        return self._response(
            self._save(path, batch, results, "completed", None), replayed=False)

    @staticmethod
    def _response(record, *, replayed):
        return {"ok": True, "format": "REMOTEBSP_PROVISIONING_BATCH_RESPONSE_V1",
                "record": record, "replayed_existing": replayed,
                "hardware_acceptance": False}

    @staticmethod
    def _validate_result(record):
        fields = {"format", "schema_version", "batch_id", "request_sha256",
                  "policy_sha256", "outcome", "results", "error_type",
                  "hardware_acceptance", "sha256"}
        if not isinstance(record, dict) or set(record) != fields or \
                record.get("format") != "REMOTEBSP_PROVISIONING_BATCH_RESULT_V1" or \
                record.get("schema_version") != 1 or record.get("sha256") != _digest(record) or \
                record.get("hardware_acceptance") is not False or \
                record.get("outcome") not in ("completed", "failed") or \
                not isinstance(record.get("results"), list):
            raise DeviceParameterError("已有批次终态损坏或被篡改")

    def _save(self, path, batch, results, outcome, error_type):
        record = {"format": "REMOTEBSP_PROVISIONING_BATCH_RESULT_V1",
            "schema_version": 1, "batch_id": batch["batch_id"],
            "request_sha256": batch["sha256"], "policy_sha256": self.policy["sha256"],
            "outcome": outcome, "results": results, "error_type": error_type,
            "hardware_acceptance": False}
        record["sha256"] = _digest(record)
        content = (json.dumps(record, ensure_ascii=False, sort_keys=True, indent=2)+"\n").encode()
        descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=self.root)
        temp = Path(name)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(content); stream.flush(); os.fsync(stream.fileno())
            os.link(temp, path); temp.unlink()
        finally: temp.unlink(missing_ok=True)
        return record
