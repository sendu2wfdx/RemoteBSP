#!/usr/bin/env python3
"""从已校验的 Studio 工程生成确定、可审计的中文工程资料包。"""

from __future__ import annotations

import hashlib
import io
import json
import zipfile
from dataclasses import dataclass

from project_config import collect_validated_project


REPORT_SCHEMA_VERSION = 1


@dataclass(frozen=True)
class ReportArtifact:
    filename: str
    content_type: str
    content: bytes
    sha256: str


@dataclass(frozen=True)
class ProjectReportBundle:
    board_id: str
    project_sha256: str
    project_schema_version: int
    original_schema_version: int
    migrations: tuple[str, ...]
    resource_count: int
    resource_set_sha256: str
    artifacts: tuple[ReportArtifact, ...]
    archive_filename: str
    archive: bytes
    archive_sha256: str


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, allow_nan=False,
                       sort_keys=True, indent=2) + "\n").encode("utf-8")


def _sha256(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _find(items: list[dict], key: str, value: object) -> dict:
    result = next((item for item in items if item.get(key) == value), None)
    if result is None:
        # 前置统一校验已经检查过端点；这里只防止目录在生成期间被错误替换。
        raise ValueError(f"已校验工程的端点丢失：{value}")
    return result


def _binding(role: str, value: str, *, shared_from: str | None = None
             ) -> dict:
    result = {"role": role, "value": value}
    if shared_from is not None:
        result["shared_from"] = shared_from
    return result


def _entry(key: str, kind: str, name: str, index: int, status: str,
           bindings: list[dict], parameters: dict) -> dict:
    return {
        "key": key,
        "kind": kind,
        "name": name,
        "index": index,
        "backend_status": status,
        "bindings": sorted(bindings,
                           key=lambda item: (item["role"], item["value"])),
        "parameters": parameters,
    }


def build_resource_entries(board: dict, resources: dict) -> list[dict]:
    """把已校验资源转换为稳定排序、可比较的 Studio 清单条目。"""
    entries: list[dict] = []

    for index, item in enumerate(resources["gpio"]):
        name = str(item.get("name") or f"gpio_{index}")
        entries.append(_entry(
            f"gpio/{index:03d}/{name}", "gpio", name, index,
            "implemented", [_binding("IO", item["pin"])], {
                "direction": item.get("direction"),
                "pull": item.get("pull", "none"),
                "active_low": bool(item.get("active_low", False)),
                "safe_level": item.get("safe_level"),
                "debounce_ms": item.get("debounce_ms", 0),
            }))

    uart_endpoints = board.get("uart", {}).get("endpoints", [])
    for index, item in enumerate(resources["uart"]):
        endpoint = _find(uart_endpoints, "endpoint_id", item["endpoint_id"])
        name = str(item.get("name") or f"uart_{index}")
        bindings = [_binding("RX", endpoint["rx_pin"]),
                    _binding("TX", endpoint["tx_pin"])]
        if item.get("direction_pin"):
            bindings.append(_binding("方向控制", item["direction_pin"]))
        entries.append(_entry(
            f"uart/{int(item['port']):03d}/{name}", "uart", name,
            int(item["port"]), endpoint["backend_status"], bindings, {
                "endpoint_id": endpoint["endpoint_id"],
                "controller": f"USART{int(item['port']) + 1}",
                "baud_rate": int(item["baud_rate"]),
            }))

    axes = resources["axes"]

    def enable_binding(axis_index: int) -> tuple[str, str | None]:
        axis = axes[axis_index]
        source = axis.get("enable_source")
        if source is None:
            return str(axis["enable"]), None
        pin, _ = enable_binding(int(source))
        return pin, f"motion/{int(source):03d}/axis_{int(source)}"

    for index, item in enumerate(axes):
        name = str(item.get("name") or f"axis_{index}")
        enable_pin, shared_from = enable_binding(index)
        bindings = [
            _binding("STEP", item["step"]),
            _binding("DIR", item["dir"]),
            _binding("EN", enable_pin, shared_from=shared_from),
        ]
        if item.get("limit"):
            bindings.append(_binding("DIAG/LIMIT", item["limit"]))
        driver = item.get("driver_type") or (
            "tmc2209_uart" if item.get("tmc_uart") else "none")
        if driver == "tmc2209_uart":
            bindings.append(_binding("TMC UART", item["tmc_uart"]))
        entries.append(_entry(
            f"motion/{index:03d}/{name}", "motion_axis", name, index,
            "implemented", bindings, {
                "driver_type": driver,
                "tmc_address": int(item.get("tmc_address", 0)),
                "maximum_step_rate_hz": int(
                    item.get("maximum_step_rate_hz", 10_000)),
                "dir_inverted": bool(item.get("dir_inverted", False)),
                "enable_active_low": bool(
                    item.get("enable_active_low", True)),
            }))

    pwm_endpoints = board.get("waveform", {}).get("pwm", [])
    for index, item in enumerate(resources["pwm"]):
        endpoint = _find(pwm_endpoints, "endpoint_id", item["endpoint_id"])
        name = str(item.get("name") or f"pwm_{index}")
        entries.append(_entry(
            f"pwm/{int(item.get('channel', index)):03d}/{name}", "pwm",
            name, int(item.get("channel", index)), endpoint["backend_status"],
            [_binding("PWM", item.get("pin") or endpoint["pin"])], {
                "endpoint_id": endpoint["endpoint_id"],
                "timer": endpoint.get("timer"),
                "frequency_group": endpoint.get("frequency_group"),
                "frequency_hz": int(item["frequency_hz"]),
                "default_duty_percent": item.get(
                    "default_duty_percent", 50),
                "active_low": bool(item.get("active_low", False)),
            }))

    strip_endpoints = board.get("waveform", {}).get("ws2812", [])
    for index, item in enumerate(resources["strips"]):
        endpoint = _find(
            strip_endpoints, "endpoint_id", item["endpoint_id"])
        name = str(item.get("name") or f"strip_{index}")
        entries.append(_entry(
            f"timed_bitstream/{int(item.get('channel', index)):03d}/{name}",
            "ws2812", name, int(item.get("channel", index)),
            endpoint["backend_status"],
            [_binding("DATA", item.get("pin") or endpoint["pin"])], {
                "endpoint_id": endpoint["endpoint_id"],
                "timer_dma": endpoint.get("timer_dma"),
                "timer_group": endpoint.get("timer_group"),
                "dma_resource": endpoint.get("dma_resource"),
                "pixel_count": int(item["pixel_count"]),
                "color_order": item.get("color_order", "GRB"),
                "reset_time_us": int(item.get("reset_time_us", 80)),
            }))

    bus_catalog = board.get("bus", {})
    bus_by_name: dict[tuple[str, str], dict] = {}
    for kind in ("i2c", "spi"):
        endpoints = bus_catalog.get(kind, {}).get("endpoints", [])
        for item in resources[f"{kind}_buses"]:
            endpoint = _find(endpoints, "endpoint_id", item["endpoint_id"])
            bindings = ([_binding("SCL", endpoint["scl_pin"]),
                         _binding("SDA", endpoint["sda_pin"])]
                        if kind == "i2c" else
                        [_binding("SCK", endpoint["sck_pin"]),
                         _binding("MISO", endpoint["miso_pin"]),
                         _binding("MOSI", endpoint["mosi_pin"])])
            name = item["name"]
            bus_by_name[(kind, name)] = item
            entries.append(_entry(
                f"{kind}_bus/{item['instance']:03d}/{name}",
                f"{kind}_bus", name, item["instance"], "mock_only",
                bindings, {
                    "endpoint_id": item["endpoint_id"],
                    "controller": item["controller"],
                    **item["contract"],
                }))

        for index, item in enumerate(resources[f"{kind}_devices"]):
            name = item["name"]
            parent = bus_by_name[(kind, item["parent_bus"])]
            bindings = [_binding("父总线", item["parent_bus"])]
            parameters = {"parent_bus": item["parent_bus"],
                          **item["contract"]}
            if kind == "i2c":
                bindings.append(_binding("7-bit地址", f"0x{item['address']:02X}"))
                parameters["address"] = item["address"]
                parameters["initial_data"] = item["initial_data"]
            else:
                bindings.append(_binding("CS", item["chip_select_pin"]))
                parameters.update({
                    "chip_select_pin": item["chip_select_pin"],
                    "mode": item["mode"],
                    "bits_per_word": item["bits_per_word"],
                    "deterministic_response":
                        item["deterministic_response"],
                })
            entries.append(_entry(
                f"{kind}_device/{parent['instance']:03d}/{index:03d}/{name}",
                f"{kind}_device", name, index, "mock_only", bindings,
                parameters))

    return sorted(entries, key=lambda item: item["key"])


def _status_text(status: str) -> str:
    return "仅 Mock 数字孪生" if status == "mock_only" else "实体后端已实现"


def _markdown_cell(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\r", "").replace(
        "\n", "<br>")


def _parameter_text(parameters: dict) -> str:
    preferred = (
        ("baud_rate", "波特率"),
        ("frequency_hz", "频率"),
        ("maximum_clock_hz", "最高时钟"),
        ("maximum_step_rate_hz", "最高STEP"),
        ("mode", "Mode"),
        ("bits_per_word", "位宽"),
        ("pixel_count", "灯珠"),
    )
    parts = []
    for key, label in preferred:
        if key in parameters:
            value = parameters[key]
            suffix = " Hz" if key.endswith("_hz") else ""
            parts.append(f"{label} {value}{suffix}")
    return "；".join(parts) or "—"


def _markdown(board: dict, project_schema_version: int,
              project_sha256: str, resource_set_sha256: str,
              entries: list[dict]) -> bytes:
    lines = [
        f"# {board['label']} 接线表",
        "",
        "> 本表由 RemoteBSP Studio 的版本化工程生成。生成前已经执行与固件配置共用的后端静态校验。",
        "",
        f"- 板卡：{board['label']}",
        f"- MCU：{board['mcu']}",
        f"- 工程 schema：v{project_schema_version}",
        f"- 工程 SHA-256：`{project_sha256}`",
        f"- 资源集合 SHA-256：`{resource_set_sha256}`",
        "",
        "## 接线与关系",
        "",
        "| 类别 | 逻辑资源 | 信号或关系 | 引脚或值 | 主要参数 | 后端状态 |",
        "|---|---|---|---|---|---|",
    ]
    labels = {
        "gpio": "GPIO", "uart": "UART", "motion_axis": "运动轴",
        "pwm": "PWM", "ws2812": "WS2812", "i2c_bus": "I2C总线",
        "i2c_device": "I2C设备", "spi_bus": "SPI总线",
        "spi_device": "SPI设备",
    }
    for entry in entries:
        parameters = _parameter_text(entry["parameters"])
        for binding_index, binding in enumerate(entry["bindings"]):
            relation = binding["role"]
            if binding.get("shared_from"):
                relation += f"（共享自 {binding['shared_from']}）"
            lines.append(
                f"| {labels[entry['kind']]} | "
                f"{_markdown_cell(entry['name'])} | "
                f"{_markdown_cell(relation)} | "
                f"`{_markdown_cell(binding['value'])}` | "
                f"{_markdown_cell(parameters) if binding_index == 0 else '—'} | "
                f"{_status_text(entry['backend_status'])} |")
    if not entries:
        lines.append("| — | 当前工程没有用户资源 | — | — | — | — |")
    lines.extend(["", "## 板卡固定占用", "",
                  "| 引脚 | 固定用途 |", "|---|---|"])
    for item in sorted(board.get("reserved", []),
                       key=lambda value: (value["pin"], value["owner"])):
        lines.append(
            f"| `{_markdown_cell(item['pin'])}` | "
            f"{_markdown_cell(item['owner'])} |")
    if not board.get("reserved"):
        lines.append("| — | 板卡目录未声明固定占用 |")
    lines.extend([
        "", "## 重要边界", "",
        "- 标记为“仅 Mock 数字孪生”的 I2C/SPI 资源不能据此认定实体板卡可用。",
        "- 本表描述静态资源映射，不代表固件已经构建、烧录或完成电气验收。",
        "- 接线前仍需核对板卡原理图、电压域、驱动能力和外设供电。",
        "",
    ])
    return "\n".join(lines).encode("utf-8")


def _summary(board: dict, project_sha256: str,
             resource_set_sha256: str, entries: list[dict]) -> dict:
    counts: dict[str, int] = {}
    pin_usage: list[dict] = []
    peripherals: set[str] = set()
    mock_only: list[str] = []
    for entry in entries:
        counts[entry["kind"]] = counts.get(entry["kind"], 0) + 1
        if entry["backend_status"] == "mock_only":
            mock_only.append(entry["key"])
        parameters = entry["parameters"]
        for key in ("controller", "timer", "timer_group", "dma_resource"):
            if parameters.get(key):
                peripherals.add(str(parameters[key]))
        for binding in entry["bindings"]:
            value = binding["value"]
            if isinstance(value, str) and len(value) in (3, 4) and \
                    value.startswith("P") and value[2:].isdigit():
                pin_usage.append({
                    "pin": value,
                    "resource_key": entry["key"],
                    "role": binding["role"],
                    **({"shared_from": binding["shared_from"]}
                       if binding.get("shared_from") else {}),
                })
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "format": "RemoteBSP Studio资源占用摘要",
        "board": {"id": board["id"], "label": board["label"],
                  "mcu": board["mcu"]},
        "project_sha256": project_sha256,
        "resource_set_sha256": resource_set_sha256,
        "resource_count": len(entries),
        "resource_counts": dict(sorted(counts.items())),
        "pin_usage": sorted(
            pin_usage,
            key=lambda item: (item["pin"], item["resource_key"], item["role"])),
        "reserved_pin_usage": sorted(
            ({"pin": item["pin"], "owner": item["owner"]}
             for item in board.get("reserved", [])),
            key=lambda item: (item["pin"], item["owner"])),
        "peripheral_usage": sorted(peripherals),
        "mock_only_resource_keys": sorted(mock_only),
        "motion_total_step_rate_hz": sum(
            entry["parameters"].get("maximum_step_rate_hz", 0)
            for entry in entries if entry["kind"] == "motion_axis"),
    }


def deterministic_zip(artifacts: tuple[ReportArtifact, ...]) -> bytes:
    """按固定顺序、时间戳、权限和无压缩方式生成稳定 ZIP。"""
    output = io.BytesIO()
    # 文件很小，使用 STORE 避免压缩库版本造成字节差异。
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED) as archive:
        for artifact in artifacts:
            info = zipfile.ZipInfo(artifact.filename, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            info.external_attr = 0o100644 << 16
            info.create_system = 3
            archive.writestr(info, artifact.content)
    return output.getvalue()


def generate_project_reports(project: dict, catalog: dict
                             ) -> ProjectReportBundle:
    """统一校验后生成接线表、占用摘要和稳定资源清单。"""
    inventory = collect_validated_project(project, catalog)
    prepared = inventory.prepared
    board = inventory.board
    entries = build_resource_entries(board, inventory.resources)
    identity = {
        "schema_version": REPORT_SCHEMA_VERSION,
        "board_id": board["id"],
        "resources": entries,
    }
    resource_set_sha256 = _sha256(_json_bytes(identity))
    manifest = {
        "schema_version": REPORT_SCHEMA_VERSION,
        "format": "RemoteBSP Studio稳定资源清单",
        "board": {"id": board["id"], "label": board["label"],
                  "mcu": board["mcu"]},
        "project": {
            "schema_version": prepared.schema_version,
            "original_schema_version": prepared.original_schema_version,
            "migrations": list(prepared.migrations),
            "sha256": prepared.sha256,
        },
        "resource_set_sha256": resource_set_sha256,
        "resources": entries,
    }
    contents = (
        (f"{board['id']}-接线表.md", "text/markdown; charset=utf-8",
         _markdown(board, prepared.schema_version, prepared.sha256,
                   resource_set_sha256, entries)),
        (f"{board['id']}-资源占用摘要.json", "application/json",
         _json_bytes(_summary(board, prepared.sha256,
                              resource_set_sha256, entries))),
        (f"{board['id']}-稳定资源清单.json", "application/json",
         _json_bytes(manifest)),
    )
    artifacts = tuple(ReportArtifact(name, content_type, content,
                                     _sha256(content))
                      for name, content_type, content in contents)
    checksums = "".join(
        f"{artifact.sha256}  {artifact.filename}\n"
        for artifact in artifacts).encode("utf-8")
    checksum_artifact = ReportArtifact(
        "SHA256SUMS", "text/plain; charset=utf-8", checksums,
        _sha256(checksums))
    artifacts += (checksum_artifact,)
    archive = deterministic_zip(artifacts)
    return ProjectReportBundle(
        board_id=board["id"], project_sha256=prepared.sha256,
        project_schema_version=prepared.schema_version,
        original_schema_version=prepared.original_schema_version,
        migrations=prepared.migrations, resource_count=len(entries),
        resource_set_sha256=resource_set_sha256, artifacts=artifacts,
        archive_filename=f"{board['id']}-Studio工程资料包.zip",
        archive=archive, archive_sha256=_sha256(archive))
