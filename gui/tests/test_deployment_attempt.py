import json
import sys
import unittest
from pathlib import Path

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from deployment_attempt import (  # noqa: E402
    create_absent_attempt, finalize_attempt, validate_deployment_attempt)
from firmware_deployment import (  # noqa: E402
    DeploymentResult, DeviceIdentity, FirmwareDeploymentError,
    FirmwareIdentity)


class DeploymentAttemptTests(unittest.TestCase):
    def plan(self):
        return {
            "format": "REMOTEBSP_STLINK_DEPLOYMENT_PLAN_V1",
            "sha256": "a" * 64, "build_id": "board-build-01234567",
            "backend": "stlink-openocd", "hardware_access": False,
            "flash_performed": False,
            "expected_identity": {
                "board_id": "weact-g431-core-v10",
                "project_sha256": "b" * 64,
                "config_sha256": "c" * 64,
                "firmware_identity_sha256": "d" * 64}}

    def test_offline_attempt_can_only_be_absent(self):
        plan = self.plan()
        attempt = create_absent_attempt(plan)
        self.assertEqual(attempt["outcome"], "absent")
        self.assertFalse(attempt["hardware_success_claimed"])
        self.assertEqual(validate_deployment_attempt(attempt, plan), attempt)
        changed = json.loads(json.dumps(attempt))
        changed["outcome"] = "verified"
        with self.assertRaisesRegex(FirmwareDeploymentError, "SHA-256"):
            validate_deployment_attempt(changed, plan)

    def test_failed_tool_and_missing_readback_are_distinct(self):
        plan = self.plan()
        initial = create_absent_attempt(plan)
        failed = finalize_attempt(
            initial, plan, started_utc="2026-09-11T01:00:00Z",
            ended_utc="2026-09-11T01:00:01Z", tool_invoked=True,
            exit_code=3, stdout=b"openocd failed")
        self.assertEqual(failed["execution"]["status"], "failed")
        self.assertEqual(failed["readback"]["status"], "absent")
        self.assertEqual(failed["outcome"], "failed")
        with self.assertRaisesRegex(FirmwareDeploymentError, "回读失败原因"):
            finalize_attempt(
                initial, plan, started_utc="2026-09-11T01:00:00Z",
                ended_utc="2026-09-11T01:00:01Z", tool_invoked=True,
                exit_code=0, stdout=b"verified")

    def test_verified_requires_tool_success_and_four_way_identity(self):
        plan = self.plan()
        expected = FirmwareIdentity(
            "weact-g431-core-v10", "b" * 64, "c" * 64, "d" * 64)
        observed = DeviceIdentity(
            "weact-g431-core-v10", "b" * 64, "c" * 64, "d" * 64,
            "ab" * 16)
        result = DeploymentResult(
            plan["build_id"], plan["backend"], expected, observed, 1, True)
        verified = finalize_attempt(
            create_absent_attempt(plan), plan,
            started_utc="2026-09-11T01:00:00Z",
            ended_utc="2026-09-11T01:00:02Z", tool_invoked=True,
            exit_code=0, stdout=b"write verify reset", result=result)
        self.assertEqual(verified["outcome"], "verified")
        self.assertTrue(verified["hardware_success_claimed"])
        self.assertEqual(verified["readback"]["status"], "verified")
        self.assertEqual(verified["readback"]["observed_identity"]["device_uuid"],
                         "ab" * 16)
        bad = DeploymentResult(
            plan["build_id"], plan["backend"], expected,
            DeviceIdentity("weact-g431-core-v10", "0" * 64, "c" * 64,
                           "d" * 64, "ab" * 16), 1, True)
        with self.assertRaisesRegex(FirmwareDeploymentError, "四重身份"):
            finalize_attempt(
                create_absent_attempt(plan), plan,
                started_utc="2026-09-11T01:00:00Z",
                ended_utc="2026-09-11T01:00:02Z", tool_invoked=True,
                exit_code=0, stdout=b"ok", result=bad)


if __name__ == "__main__":
    unittest.main()
