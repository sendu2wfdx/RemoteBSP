"""Runtime 告警规则的封闭模型、原子存储与两阶段更新。"""

from __future__ import annotations

import json
import os
import secrets
import stat
import threading
import time
from pathlib import Path


ALERT_RULE_SCHEMA_VERSION = 1
ALERT_RULE_KIND = "remotebsp-runtime-alert-rules"
ALERT_RULE_FILENAME = "alert-rules-v1.json"
ALERT_RULE_CONFIRMATION = "APPLY_ALERT_RULES"
MAXIMUM_RULES = 16
MAXIMUM_RULE_FILE_BYTES = 32 * 1024
_METRICS = {
    "cpu_load_permille": ("permille", 0, 1000),
    "isr_load_permille": ("permille", 0, 1000),
}
_COMPARISONS = {"greater_or_equal", "less_or_equal"}
_SEVERITIES = {"warning", "critical"}


class AlertRuleError(RuntimeError):
    """规则配置不能安全加载或更新。"""


class AlertRuleStorageError(AlertRuleError):
    """规则存储不可用；异常细节不得直接暴露给远端调用方。"""


DEFAULT_ALERT_RULES = (
    {"rule_id": "cpu-load-warning", "metric": "cpu_load_permille",
     "unit": "permille", "comparison": "greater_or_equal",
     "trigger_threshold": 800, "recovery_threshold": 750,
     "severity": "warning", "debounce_samples": 1},
    {"rule_id": "cpu-load-critical", "metric": "cpu_load_permille",
     "unit": "permille", "comparison": "greater_or_equal",
     "trigger_threshold": 950, "recovery_threshold": 900,
     "severity": "critical", "debounce_samples": 1},
    {"rule_id": "isr-load-warning", "metric": "isr_load_permille",
     "unit": "permille", "comparison": "greater_or_equal",
     "trigger_threshold": 800, "recovery_threshold": 750,
     "severity": "warning", "debounce_samples": 1},
    {"rule_id": "isr-load-critical", "metric": "isr_load_permille",
     "unit": "permille", "comparison": "greater_or_equal",
     "trigger_threshold": 950, "recovery_threshold": 900,
     "severity": "critical", "debounce_samples": 1},
)


def _identifier(value: object) -> bool:
    return isinstance(value, str) and 1 <= len(value) <= 64 and all(
        char.isalnum() or char in "-_." for char in value)


def validate_rules(value: object) -> tuple[dict, ...]:
    if not isinstance(value, list) or not 1 <= len(value) <= MAXIMUM_RULES:
        raise AlertRuleError(f"rules数量必须位于1～{MAXIMUM_RULES}")
    clean: list[dict] = []
    ids: set[str] = set()
    fields = {"rule_id", "metric", "unit", "comparison",
              "trigger_threshold", "recovery_threshold", "severity",
              "debounce_samples"}
    for index, rule in enumerate(value):
        if not isinstance(rule, dict) or set(rule) != fields:
            raise AlertRuleError(f"rules[{index}]字段不合法")
        rule_id, metric = rule["rule_id"], rule["metric"]
        if not _identifier(rule_id) or rule_id in ids:
            raise AlertRuleError(f"rules[{index}].rule_id无效或重复")
        ids.add(rule_id)
        if metric not in _METRICS:
            raise AlertRuleError(f"rules[{index}].metric不受支持")
        unit, minimum, maximum = _METRICS[metric]
        if rule["unit"] != unit or rule["comparison"] not in _COMPARISONS or \
                rule["severity"] not in _SEVERITIES:
            raise AlertRuleError(f"rules[{index}]单位、比较符或级别不合法")
        trigger, recovery = rule["trigger_threshold"], rule["recovery_threshold"]
        debounce = rule["debounce_samples"]
        if type(trigger) is not int or type(recovery) is not int or not \
                minimum <= trigger <= maximum or not minimum <= recovery <= maximum:
            raise AlertRuleError(f"rules[{index}]阈值超出指标范围")
        if (rule["comparison"] == "greater_or_equal" and recovery >= trigger) or \
                (rule["comparison"] == "less_or_equal" and recovery <= trigger):
            raise AlertRuleError(f"rules[{index}]恢复阈值没有形成迟滞区间")
        if type(debounce) is not int or not 1 <= debounce <= 60:
            raise AlertRuleError(f"rules[{index}].debounce_samples必须位于1～60")
        clean.append({key: rule[key] for key in fields})
    return tuple(sorted(clean, key=lambda item: item["rule_id"]))


class AlertRuleStore:
    def __init__(self, directory: Path):
        self.directory = Path(directory)
        self.path = self.directory / ALERT_RULE_FILENAME
        try:
            if self.directory.exists() and self.directory.is_symlink():
                raise AlertRuleStorageError("告警规则目录不能是符号链接")
            self.directory.mkdir(mode=0o700, parents=False, exist_ok=True)
            if not self.directory.is_dir() or self.path.is_symlink():
                raise AlertRuleStorageError("告警规则存储路径不安全")
            if os.name == "posix":
                info = self.directory.stat()
                if info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) & 0o077:
                    raise AlertRuleStorageError("告警规则目录必须由服务用户独占")
        except AlertRuleStorageError:
            raise
        except OSError as error:
            raise AlertRuleStorageError(
                f"无法准备告警规则目录：{error}") from error

    def _document(self, revision: int, rules: tuple[dict, ...]) -> dict:
        return {"schema_version": ALERT_RULE_SCHEMA_VERSION,
                "kind": ALERT_RULE_KIND, "revision": revision,
                "rules": [dict(item) for item in rules]}

    def load(self) -> dict:
        if not self.path.exists():
            return self._document(0, DEFAULT_ALERT_RULES)
        try:
            if self.path.is_symlink():
                raise AlertRuleStorageError("告警规则文件不能是符号链接")
            info = self.path.stat()
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or \
                    (os.name == "posix" and (info.st_uid != os.geteuid() or
                                             stat.S_IMODE(info.st_mode) & 0o077)):
                raise AlertRuleStorageError(
                    "告警规则文件必须是服务用户独占的单链接普通文件")
            if info.st_size > MAXIMUM_RULE_FILE_BYTES:
                raise ValueError("文件超过容量上限")
            raw = json.loads(self.path.read_bytes().decode("utf-8"))
            if not isinstance(raw, dict) or set(raw) != {
                    "schema_version", "kind", "revision", "rules"} or \
                    raw["schema_version"] != ALERT_RULE_SCHEMA_VERSION or \
                    raw["kind"] != ALERT_RULE_KIND or type(raw["revision"]) is not int or \
                    not 0 <= raw["revision"] <= (1 << 63) - 1:
                raise ValueError("文件头不合法")
            try:
                rules = validate_rules(raw["rules"])
            except AlertRuleError as error:
                raise ValueError(str(error)) from error
            return self._document(raw["revision"], rules)
        except AlertRuleStorageError:
            raise
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            try:
                quarantine = self.directory / f"alert-rules-v1.corrupt-{secrets.token_hex(8)}.json"
                os.link(self.path, quarantine)
                self.path.unlink()
            except OSError as isolate_error:
                raise AlertRuleStorageError(
                    f"规则文件损坏且无法隔离：{isolate_error}") from error
            raise AlertRuleStorageError(
                "规则文件已损坏并隔离；拒绝隐式回退") from error

    def save(self, revision: int, rules: tuple[dict, ...]) -> dict:
        document = self._document(revision, validate_rules(list(rules)))
        encoded = (json.dumps(document, ensure_ascii=False, sort_keys=True,
                              separators=(",", ":")) + "\n").encode()
        if len(encoded) > MAXIMUM_RULE_FILE_BYTES:
            raise AlertRuleStorageError("规则文件超过容量上限")
        temporary = self.directory / f".{ALERT_RULE_FILENAME}.{secrets.token_hex(8)}.tmp"
        try:
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "wb") as stream:
                stream.write(encoded); stream.flush(); os.fsync(stream.fileno())
            os.replace(temporary, self.path)
            if os.name == "posix":
                directory_fd = os.open(self.directory, os.O_RDONLY)
                try: os.fsync(directory_fd)
                finally: os.close(directory_fd)
            return document
        except OSError as error:
            temporary.unlink(missing_ok=True)
            raise AlertRuleStorageError(f"无法原子保存规则：{error}") from error


class AlertRuleManager:
    """提供快照查询和单次短时确认令牌；写入前再次检查 revision。"""
    def __init__(self, store: AlertRuleStore | None = None, *, token_ttl: float = 300):
        if not 30 <= token_ttl <= 600:
            raise ValueError("确认令牌有效期必须位于30～600秒")
        self.store, self.token_ttl = store, token_ttl
        self._state = store.load() if store else {
            "schema_version": ALERT_RULE_SCHEMA_VERSION, "kind": ALERT_RULE_KIND,
            "revision": 0, "rules": [dict(item) for item in
                                      validate_rules(list(DEFAULT_ALERT_RULES))]}
        self._tokens: dict[str, tuple[float, int, tuple[dict, ...]]] = {}
        self._lock = threading.Lock()

    def snapshot(self) -> dict:
        with self._lock:
            return json.loads(json.dumps(self._state))

    def preflight(self, expected_revision: object, rules: object) -> dict:
        if type(expected_revision) is not int:
            raise AlertRuleError("expected_revision必须是整数")
        clean = validate_rules(rules)
        with self._lock:
            if expected_revision != self._state["revision"]:
                raise AlertRuleError("规则revision已变化，请重新读取")
            now = time.monotonic()
            self._tokens = {key: item for key, item in self._tokens.items()
                            if item[0] > now}
            if len(self._tokens) >= 16:
                raise AlertRuleError("待确认更新已达到上限")
            token = secrets.token_urlsafe(32)
            self._tokens[token] = (now + self.token_ttl, expected_revision, clean)
        return {"confirmation_token": token,
                "confirmation_phrase": ALERT_RULE_CONFIRMATION,
                "expires_in_seconds": self.token_ttl,
                "expected_revision": expected_revision,
                "next_revision": expected_revision + 1,
                "rules": [dict(item) for item in clean], "applied": False}

    def apply(self, token: object, confirmation: object) -> dict:
        if not isinstance(token, str) or len(token) > 128 or \
                confirmation != ALERT_RULE_CONFIRMATION:
            raise AlertRuleError("确认令牌或确认短语无效")
        with self._lock:
            pending = self._tokens.pop(token, None)
            if pending is None or pending[0] <= time.monotonic():
                raise AlertRuleError("确认令牌不存在、已使用或已过期")
            _, revision, rules = pending
            if revision != self._state["revision"]:
                raise AlertRuleError("规则revision已变化，请重新预检")
            next_revision = revision + 1
            state = self.store.save(next_revision, rules) if self.store else \
                {"schema_version": ALERT_RULE_SCHEMA_VERSION,
                 "kind": ALERT_RULE_KIND, "revision": next_revision,
                 "rules": [dict(item) for item in rules]}
            self._state = state
            return {**json.loads(json.dumps(state)), "applied": True}


class AlertRuleEvaluator:
    """按规则 ID 有界保存去抖/迟滞状态。"""
    def __init__(self, manager: AlertRuleManager):
        self.manager = manager
        self._state: dict[tuple[str, str], tuple[bool, int]] = {}
        self._lock = threading.Lock()

    def evaluate(self, metrics: list[dict], *, scope: str = "toolbusd") -> list[dict]:
        if not _identifier(scope):
            raise AlertRuleError("告警评估scope不合法")
        values = {item.get("name"): item.get("value") for item in metrics
                  if item.get("availability") == "available" and
                  type(item.get("value")) is int}
        result = []
        rules = self.manager.snapshot()["rules"]
        thresholds = {(rule["metric"], rule["severity"]):
                      rule["trigger_threshold"] for rule in rules}
        with self._lock:
            live_ids = {rule["rule_id"] for rule in rules}
            self._state = {key: value for key, value in self._state.items()
                           if key[1] in live_ids}
            for rule in rules:
                state_key = (scope, rule["rule_id"])
                measured = values.get(rule["metric"])
                if measured is None:
                    self._state.pop(state_key, None); continue
                active, count = self._state.get(state_key, (False, 0))
                high = rule["comparison"] == "greater_or_equal"
                triggering = measured >= rule["trigger_threshold"] if high else \
                    measured <= rule["trigger_threshold"]
                recovered = measured <= rule["recovery_threshold"] if high else \
                    measured >= rule["recovery_threshold"]
                if active and recovered: active, count = False, 0
                elif not active:
                    count = count + 1 if triggering else 0
                    if count >= rule["debounce_samples"]: active = True
                self._state[state_key] = (active, count)
                if active:
                    result.append({"rule_id": rule["rule_id"],
                                   "metric": rule["metric"], "severity": rule["severity"],
                                   "value": measured, "unit": rule["unit"],
                                   "comparison": rule["comparison"],
                                   "trigger_threshold": rule["trigger_threshold"],
                                   "recovery_threshold": rule["recovery_threshold"],
                                   "debounce_samples": rule["debounce_samples"],
                                   "warning_threshold": thresholds.get(
                                       (rule["metric"], "warning")),
                                   "critical_threshold": thresholds.get(
                                       (rule["metric"], "critical"))})
        return sorted(result, key=lambda item: (
            0 if item["severity"] == "critical" else 1, item["rule_id"]))
