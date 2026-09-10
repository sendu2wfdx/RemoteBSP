#!/usr/bin/env python3
"""ADC 生产校准工件：只管理数据，不访问或采样实体 ADC。"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import struct
import zlib

from device_parameters import DeviceParameterError


FORMAT = "REMOTEBSP_ADC_CALIBRATION_V1"
VALUE_MAGIC = b"ADCC"
VALUE_VERSION = 1
VALUE_SIZE = 64
MAX_POINTS = 4
ADC_PARAMETER_BASE = 0x1000
ADC_CHANNEL_COUNT = 16


def _integer(value: object, name: str, minimum: int, maximum: int) -> int:
    if type(value) is not int or not minimum <= value <= maximum:
        raise DeviceParameterError(f"ADC校准{name}必须位于{minimum}～{maximum}")
    return value


def _digest(artifact: dict) -> str:
    canonical = json.dumps(
        {key: value for key, value in artifact.items() if key != "sha256"},
        ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def _encode(specification: dict) -> bytes:
    channel = _integer(specification.get("channel"), "通道", 0, 15)
    resource_id = _integer(specification.get("resource_id"), "资源ID", 1, 0xffffffff)
    if resource_id >> 24 != 0x05:
        raise DeviceParameterError("ADC校准资源ID必须属于ADC资源类别0x05")
    adc_bits = _integer(specification.get("adc_bits"), "位宽", 8, 16)
    minimum_uv = _integer(specification.get("range_min_uv"), "量程下限", 0, 100000000)
    maximum_uv = _integer(specification.get("range_max_uv"), "量程上限", 1, 100000000)
    reference_uv = _integer(specification.get("reference_uv"), "参考电压", 1000000, 5000000)
    if minimum_uv >= maximum_uv or not minimum_uv <= reference_uv <= maximum_uv:
        raise DeviceParameterError("ADC校准量程或参考电压关系无效")
    mode = specification.get("mode")
    points = specification.get("points", [])
    if not isinstance(points, list):
        raise DeviceParameterError("ADC校准点必须是数组")
    gain = offset = 0
    encoded_points: list[tuple[int, int]] = []
    if mode == "gain_offset":
        if points:
            raise DeviceParameterError("增益偏移模式不能同时包含校准点")
        gain = _integer(specification.get("gain_q16_16"), "增益Q16.16", 1, 0x7fffffff)
        offset = _integer(specification.get("offset_uv"), "偏移", -100000000, 100000000)
        mode_code = 1
    elif mode == "points":
        if specification.get("gain_q16_16") is not None or specification.get("offset_uv") is not None:
            raise DeviceParameterError("校准点模式不能同时包含增益偏移")
        if not 2 <= len(points) <= MAX_POINTS:
            raise DeviceParameterError("ADC校准点数量必须位于2～4")
        previous_code = -1
        previous_uv = -1
        maximum_code = (1 << adc_bits) - 1
        for point in points:
            if not isinstance(point, dict) or set(point) != {"raw_code", "voltage_uv"}:
                raise DeviceParameterError("ADC校准点字段无效")
            code = _integer(point["raw_code"], "点原始码", 0, maximum_code)
            uv = _integer(point["voltage_uv"], "点电压", minimum_uv, maximum_uv)
            if code <= previous_code or uv <= previous_uv:
                raise DeviceParameterError("ADC校准点必须按原始码和电压严格递增")
            previous_code, previous_uv = code, uv
            encoded_points.append((code, uv))
        mode_code = 2
    else:
        raise DeviceParameterError("ADC校准模式只允许gain_offset或points")

    value = bytearray(VALUE_SIZE)
    struct.pack_into("<4sBBBBIIIIiiB3x", value, 0, VALUE_MAGIC,
                     VALUE_VERSION, mode_code, channel, adc_bits, resource_id,
                     minimum_uv, maximum_uv, reference_uv, gain, offset,
                     len(encoded_points))
    for index, (code, uv) in enumerate(encoded_points):
        struct.pack_into("<HI", value, 36 + index * 6, code, uv)
    struct.pack_into("<I", value, 60, zlib.crc32(value[:60]) & 0xffffffff)
    return bytes(value)


def create_adc_calibration(specification: object) -> dict:
    if not isinstance(specification, dict) or set(specification) - {
            "channel", "resource_id", "adc_bits", "range_min_uv",
            "range_max_uv", "reference_uv", "mode", "gain_q16_16",
            "offset_uv", "points"}:
        raise DeviceParameterError("ADC校准规格字段无效")
    value = _encode(specification)
    artifact = {
        "format": FORMAT, "schema_version": 1,
        "parameter_id": ADC_PARAMETER_BASE + specification["channel"],
        "value_base64": base64.b64encode(value).decode("ascii"),
        "value_crc32": f"{struct.unpack_from('<I', value, 60)[0]:08x}",
        "specification": specification,
        "sampling_performed": False,
        "calibration_claimed": False,
    }
    artifact["sha256"] = _digest(artifact)
    return artifact


def validate_adc_calibration(artifact: object) -> dict:
    if not isinstance(artifact, dict) or set(artifact) != {
            "format", "schema_version", "parameter_id", "value_base64",
            "value_crc32", "specification", "sampling_performed",
            "calibration_claimed", "sha256"}:
        raise DeviceParameterError("ADC校准工件字段无效")
    if artifact["format"] != FORMAT or artifact["schema_version"] != 1 or \
            artifact["sampling_performed"] is not False or \
            artifact["calibration_claimed"] is not False:
        raise DeviceParameterError("ADC校准工件版本或声明无效")
    if not isinstance(artifact["sha256"], str) or \
            not hmac.compare_digest(artifact["sha256"], _digest(artifact)):
        raise DeviceParameterError("ADC校准工件SHA-256不匹配")
    expected = create_adc_calibration(artifact["specification"])
    for field in ("parameter_id", "value_base64", "value_crc32"):
        if artifact[field] != expected[field]:
            raise DeviceParameterError("ADC校准工件编码或CRC不匹配")
    return artifact


def _verify_resource_evidence(artifact: dict, backup: dict,
                              evidence: object) -> None:
    specification = artifact["specification"]
    if not isinstance(evidence, dict) or set(evidence) != {
            "format", "schema_version", "verified", "node_uuid",
            "resource_id", "channel", "resolution_bits", "reference_uv"} or \
            evidence.get("format") != "REMOTEBSP_ADC_RESOURCE_EVIDENCE_V1" or \
            evidence.get("schema_version") != 1 or evidence.get("verified") is not True:
        raise DeviceParameterError("ADC资源合同证据格式或验证状态无效")
    if evidence.get("node_uuid") != backup["node_uuid"] or \
            evidence.get("resource_id") != specification["resource_id"] or \
            evidence.get("channel") != specification["channel"] or \
            evidence.get("resolution_bits") != specification["adc_bits"] or \
            evidence.get("reference_uv") != specification["reference_uv"]:
        raise DeviceParameterError("ADC资源合同证据与校准工件或目标节点不一致")


def preflight_adc_calibration(artifact: object, backup: object,
                              resource_evidence: object | None = None) -> dict:
    validated = validate_adc_calibration(artifact)
    if not isinstance(backup, dict) or backup.get("schema_version") not in (1, 2) or \
            not isinstance(backup.get("node_uuid"), str) or \
            not isinstance(backup.get("status"), dict) or \
            type(backup["status"].get("generation")) is not int or \
            not isinstance(backup.get("parameters"), list):
        raise DeviceParameterError("设备参数备份不能用于ADC校准预检")
    if backup.get("schema_version") == 2:
        from device_parameters import DeviceParameterManager
        if backup.get("format") != "REMOTEBSP_DEVICE_PARAMETERS" or \
                not isinstance(backup.get("sha256"), str) or \
                not hmac.compare_digest(
                    backup["sha256"], DeviceParameterManager._backup_digest(backup)):
            raise DeviceParameterError("设备参数备份完整性校验失败")
    parameter_id = validated["parameter_id"]
    matches = [item for item in backup["parameters"]
               if isinstance(item, dict) and item.get("id") == parameter_id]
    if len(matches) != 1 or matches[0].get("type") != 5 or \
            matches[0].get("byte_count") not in (0, 12, VALUE_SIZE):
        raise DeviceParameterError("目标ADC校准参数不存在、重复、类型或长度不兼容")
    if resource_evidence is not None:
        _verify_resource_evidence(validated, backup, resource_evidence)
    return {
        "format": "REMOTEBSP_ADC_CALIBRATION_PREFLIGHT_V1",
        "schema_version": 1, "ready_to_write": True,
        "node_uuid": backup["node_uuid"],
        "expected_generation": backup["status"]["generation"],
        "parameter_id": parameter_id,
        "value_base64": validated["value_base64"],
        "value_sha256": hashlib.sha256(base64.b64decode(
            validated["value_base64"], validate=True)).hexdigest(),
        "required_confirmation": "WRITE_DEVICE_PARAMETERS",
        "hardware_access": False, "sampling_performed": False,
        "resource_contract_verified": resource_evidence is not None,
        "resource_binding": ("verified_runtime_contract" if resource_evidence is not None
                             else "artifact_only"),
    }
