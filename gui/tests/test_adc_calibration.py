import base64
import json
import sys
import unittest
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from adc_calibration import (  # noqa: E402
    VALUE_SIZE, create_adc_calibration, preflight_adc_calibration,
    validate_adc_calibration)
from device_parameters import DeviceParameterError, DeviceParameterManager  # noqa: E402


class AdcCalibrationTests(unittest.TestCase):
    def gain_spec(self):
        return {"channel": 2, "resource_id": 0x05000003, "adc_bits": 12,
                "range_min_uv": 0, "range_max_uv": 3300000,
                "reference_uv": 3300000, "mode": "gain_offset",
                "gain_q16_16": 65536, "offset_uv": -1200, "points": []}

    def test_gain_artifact_is_deterministic_and_tamper_evident(self):
        artifact = create_adc_calibration(self.gain_spec())
        self.assertEqual(artifact["parameter_id"], 0x1002)
        self.assertEqual(len(base64.b64decode(artifact["value_base64"])), VALUE_SIZE)
        self.assertEqual(validate_adc_calibration(artifact), artifact)
        changed = json.loads(json.dumps(artifact))
        changed["specification"]["offset_uv"] = 0
        with self.assertRaisesRegex(DeviceParameterError, "SHA-256"):
            validate_adc_calibration(changed)

    def test_points_are_bounded_monotonic_and_exclusive(self):
        specification = {**self.gain_spec(), "mode": "points",
                         "gain_q16_16": None, "offset_uv": None,
                         "points": [{"raw_code": 10, "voltage_uv": 10000},
                                    {"raw_code": 4090, "voltage_uv": 3290000}]}
        validate_adc_calibration(create_adc_calibration(specification))
        specification["points"][1]["raw_code"] = 10
        with self.assertRaisesRegex(DeviceParameterError, "严格递增"):
            create_adc_calibration(specification)

    def test_preflight_binds_backup_uuid_generation_and_descriptor(self):
        artifact = create_adc_calibration(self.gain_spec())
        backup = {"format": "REMOTEBSP_DEVICE_PARAMETERS",
                  "schema_version": 2, "node_uuid": "ab" * 16,
                  "status": {"generation": 7},
                  "parameters": [{"id": 0x1002, "type": 5,
                                  "byte_count": 12}]}
        backup["sha256"] = DeviceParameterManager._backup_digest(backup)
        result = preflight_adc_calibration(artifact, backup)
        self.assertTrue(result["ready_to_write"])
        self.assertEqual(result["expected_generation"], 7)
        self.assertFalse(result["hardware_access"])
        self.assertFalse(result["resource_contract_verified"])
        self.assertEqual(result["resource_binding"], "artifact_only")
        changed = json.loads(json.dumps(backup))
        changed["status"]["generation"] = 8
        with self.assertRaisesRegex(DeviceParameterError, "完整性"):
            preflight_adc_calibration(artifact, changed)

    def test_preflight_allows_absent_first_write_and_verifies_contract(self):
        artifact = create_adc_calibration(self.gain_spec())
        backup = {"schema_version": 1, "node_uuid": "ab" * 16,
                  "status": {"generation": 0},
                  "parameters": [{"id": 0x1002, "type": 5,
                                  "byte_count": 0}]}
        evidence = {
            "format": "REMOTEBSP_ADC_RESOURCE_EVIDENCE_V1",
            "schema_version": 1, "verified": True,
            "node_uuid": "ab" * 16, "resource_id": 0x05000003,
            "channel": 2, "resolution_bits": 12,
            "reference_uv": 3300000}
        result = preflight_adc_calibration(artifact, backup, evidence)
        self.assertTrue(result["ready_to_write"])
        self.assertTrue(result["resource_contract_verified"])
        self.assertEqual(result["resource_binding"],
                         "verified_runtime_contract")
        evidence["resolution_bits"] = 16
        with self.assertRaisesRegex(DeviceParameterError, "不一致"):
            preflight_adc_calibration(artifact, backup, evidence)

    def test_rejects_unknown_fields_ranges_and_mismatched_channel(self):
        for change, message in (({"channel": 16}, "通道"),
                                ({"reference_uv": 999999}, "参考电压"),
                                ({"adc_bits": 17}, "位宽")):
            with self.assertRaisesRegex(DeviceParameterError, message):
                create_adc_calibration({**self.gain_spec(), **change})
        with self.assertRaisesRegex(DeviceParameterError, "量程"):
            create_adc_calibration({**self.gain_spec(),
                                    "range_min_uv": 4000000,
                                    "range_max_uv": 5000000})
        with self.assertRaisesRegex(DeviceParameterError, "字段"):
            create_adc_calibration({**self.gain_spec(), "comment": "未绑定文本"})


if __name__ == "__main__":
    unittest.main()
