"""Runtime 健康趋势的本地、有界、版本化持久存储。"""

from __future__ import annotations

import json
import os
import secrets
import stat
from pathlib import Path


TREND_STORE_SCHEMA_VERSION = 1
TREND_STORE_KIND = "remotebsp-runtime-trends"
TREND_STORE_FILENAME = "trend-v1.json"
DEFAULT_MAXIMUM_BYTES = 1024 * 1024
MAXIMUM_NODES = 128
MAXIMUM_METRICS_PER_SAMPLE = 64


class TrendStoreError(RuntimeError):
    """趋势目录或持久写入不满足安全约束。"""


def _plain_int(value: object) -> bool:
    return type(value) is int and 0 <= value <= (1 << 63) - 1


def _safe_identifier(value: object) -> bool:
    return isinstance(value, str) and 1 <= len(value) <= 128 and all(
        char.isalnum() or char in "-_.:" for char in value)


class RuntimeTrendStore:
    """只在固定文件中保存展示所需的数值趋势，不保存完整快照。"""

    def __init__(self, directory: Path, *, capacity: int,
                 maximum_bytes: int = DEFAULT_MAXIMUM_BYTES):
        if type(capacity) is not int or not 1 <= capacity <= 600:
            raise TrendStoreError("趋势容量必须位于1～600")
        if type(maximum_bytes) is not int or not 4096 <= maximum_bytes <= \
                16 * 1024 * 1024:
            raise TrendStoreError("趋势文件上限必须位于4096～16777216字节")
        self.capacity = capacity
        self.maximum_bytes = maximum_bytes
        self.directory = Path(directory)
        self.path = self.directory / TREND_STORE_FILENAME
        self._prepare_directory()

    def _prepare_directory(self) -> None:
        try:
            if self.directory.exists() and self.directory.is_symlink():
                raise TrendStoreError("趋势目录不能是符号链接")
            self.directory.mkdir(mode=0o700, parents=False, exist_ok=True)
            if not self.directory.is_dir():
                raise TrendStoreError("趋势路径不是目录")
            if os.name == "posix":
                directory_stat = self.directory.stat()
                mode = stat.S_IMODE(directory_stat.st_mode)
                if mode & 0o077:
                    raise TrendStoreError("趋势目录不得允许组或其他用户访问")
                if directory_stat.st_uid != os.geteuid():
                    raise TrendStoreError("趋势目录必须属于当前服务用户")
            if self.path.is_symlink():
                raise TrendStoreError("趋势文件不能是符号链接")
        except TrendStoreError:
            raise
        except OSError as error:
            raise TrendStoreError(f"无法准备趋势目录：{error}") from error

    def _validate_sample(self, sample: object, *, health: bool) -> dict:
        if not isinstance(sample, dict) or set(sample) != {
                "identity", "time_ms", "values"}:
            raise ValueError("趋势样本字段不合法")
        identity = sample["identity"]
        if not isinstance(identity, list) or not 1 <= len(identity) <= 3 or \
                not all(_safe_identifier(value) or _plain_int(value)
                        for value in identity):
            raise ValueError("趋势样本身份不合法")
        if health and (len(identity) != 2 or
                       not all(_plain_int(value) for value in identity)):
            raise ValueError("健康样本身份不合法")
        if not _plain_int(sample["time_ms"]):
            raise ValueError("趋势样本时间不合法")
        values = sample["values"]
        if not isinstance(values, dict) or len(values) > MAXIMUM_METRICS_PER_SAMPLE:
            raise ValueError("趋势指标数量不合法")
        if not all(_safe_identifier(name) and _plain_int(value)
                   for name, value in values.items()):
            raise ValueError("趋势指标不合法")
        return {"identity": list(identity), "time_ms": sample["time_ms"],
                "values": dict(values)}

    def _validate(self, raw: object) -> dict:
        if not isinstance(raw, dict) or set(raw) != {
                "schema_version", "kind", "nodes", "toolbusd_health"} or \
                raw["schema_version"] != TREND_STORE_SCHEMA_VERSION or \
                raw["kind"] != TREND_STORE_KIND:
            raise ValueError("趋势文件头不合法")
        nodes = raw["nodes"]
        if not isinstance(nodes, dict) or len(nodes) > MAXIMUM_NODES:
            raise ValueError("趋势节点数量不合法")
        clean_nodes = {}
        for node_id, samples in nodes.items():
            if not _safe_identifier(node_id) or not isinstance(samples, list) or \
                    len(samples) > self.capacity:
                raise ValueError("节点趋势不合法")
            clean_nodes[node_id] = [self._validate_sample(item, health=False)
                                    for item in samples]
        health = raw["toolbusd_health"]
        if not isinstance(health, list) or len(health) > self.capacity:
            raise ValueError("toolbusd健康趋势不合法")
        return {"nodes": clean_nodes,
                "toolbusd_health": [self._validate_sample(item, health=True)
                                     for item in health]}

    def load(self) -> dict:
        if not self.path.exists():
            return {"nodes": {}, "toolbusd_health": []}
        try:
            if self.path.is_symlink():
                raise TrendStoreError("趋势文件不能是符号链接")
            file_stat = self.path.stat()
            if not stat.S_ISREG(file_stat.st_mode) or file_stat.st_nlink != 1:
                raise TrendStoreError("趋势文件必须是单链接普通文件")
            if os.name == "posix" and (
                    file_stat.st_uid != os.geteuid() or
                    stat.S_IMODE(file_stat.st_mode) & 0o077):
                raise TrendStoreError("趋势文件必须由服务用户独占")
            if file_stat.st_size > self.maximum_bytes:
                raise ValueError("趋势文件超过容量上限")
            raw = json.loads(self.path.read_bytes().decode("utf-8"))
            return self._validate(raw)
        except TrendStoreError:
            raise
        except (OSError, UnicodeDecodeError, json.JSONDecodeError,
                ValueError) as error:
            # 数据损坏不能污染新一代状态；保留隔离副本供人工诊断。
            try:
                # 硬链接创建带 O_EXCL 等价的“不覆盖”语义，再移除原名。
                for _ in range(8):
                    quarantine = self.directory / (
                        f"trend-v1.corrupt-{secrets.token_hex(8)}.json")
                    try:
                        os.link(self.path, quarantine)
                        break
                    except FileExistsError:
                        continue
                else:
                    raise OSError("无法分配隔离文件名")
                self.path.unlink()
            except OSError as isolate_error:
                raise TrendStoreError(
                    f"趋势文件损坏且无法隔离：{isolate_error}") from error
            return {"nodes": {}, "toolbusd_health": []}

    def save(self, state: dict) -> None:
        clean = self._validate({
            "schema_version": TREND_STORE_SCHEMA_VERSION,
            "kind": TREND_STORE_KIND,
            "nodes": state["nodes"],
            "toolbusd_health": state["toolbusd_health"],
        })
        document = {"schema_version": TREND_STORE_SCHEMA_VERSION,
                    "kind": TREND_STORE_KIND, **clean}
        encoded = (json.dumps(document, ensure_ascii=False, sort_keys=True,
                              separators=(",", ":")) + "\n").encode("utf-8")
        if len(encoded) > self.maximum_bytes:
            raise TrendStoreError("趋势文件超过容量上限")
        temporary = self.directory / f".{TREND_STORE_FILENAME}.{secrets.token_hex(8)}.tmp"
        try:
            descriptor = os.open(temporary,
                                 os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "wb") as output:
                output.write(encoded)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, self.path)
            if os.name == "posix":
                directory_fd = os.open(self.directory, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
        except OSError as error:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
            raise TrendStoreError(f"无法原子保存趋势：{error}") from error
