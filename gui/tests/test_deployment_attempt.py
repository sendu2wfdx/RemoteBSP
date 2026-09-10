import json
import sys
import unittest
import tempfile
from pathlib import Path
from unittest.mock import patch

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from deployment_attempt import (  # noqa: E402
    EXECUTION_CONFIRMATION, create_absent_attempt, execute_deployment_plan,
    finalize_attempt, validate_deployment_attempt)
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

    def test_controlled_executor_routes_three_backends_and_saves_verified(self):
        expected = FirmwareIdentity(
            "weact-g431-core-v10", "b" * 64, "c" * 64, "d" * 64)
        observed = DeviceIdentity(
            "weact-g431-core-v10", "b" * 64, "c" * 64, "d" * 64,
            "ab" * 16)
        formats = (
            ("stlink-openocd", "REMOTEBSP_STLINK_DEPLOYMENT_PLAN_V1",
             "validate_stlink_deployment_plan", "deploy_stlink",
             {"probe_serial": None}),
            ("can-katapult", "REMOTEBSP_CAN_KATAPULT_DEPLOYMENT_PLAN_V1",
             "validate_can_katapult_deployment_plan", "deploy_can_katapult",
             {"can_interface": "can0", "katapult_uuid": "abcdef",
              "flashtool": "/tmp/flashtool.py"}),
            ("usb-katapult", "REMOTEBSP_USB_KATAPULT_DEPLOYMENT_PLAN_V1",
             "validate_usb_katapult_deployment_plan", "deploy_usb_katapult",
             {"usb_device": "/dev/serial/by-id/usb-test",
              "flashtool": "/tmp/flashtool.py"}),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, (backend, format_name, validator, deploy, extra) in enumerate(formats):
                plan = {**self.plan(), "format": format_name,
                        "backend": backend, **extra}
                result = DeploymentResult(
                    plan["build_id"], backend, expected, observed, 1, True)

                def fake_deploy(*args, **kwargs):
                    kwargs["runner"](("fake-tool",), 10)
                    args[1].read_identity()
                    return result

                class Reader:
                    def read_identity(self):
                        return observed

                output = root / f"attempt-{index}.json"
                with patch(f"deployment_attempt.{validator}", return_value=plan), \
                        patch(f"deployment_attempt.{deploy}", side_effect=fake_deploy):
                    attempt = execute_deployment_plan(
                        plan, output_root=root, reader=Reader(),
                        attempt_output=output, execute=True,
                        confirmation=EXECUTION_CONFIRMATION,
                        runner=lambda *_: (0, b"stdout", b"stderr"),
                        started_utc="2026-09-11T01:00:00Z",
                        ended_utc_provider=lambda: "2026-09-11T01:00:01Z")
                self.assertEqual(attempt["outcome"], "verified")
                self.assertEqual(attempt["execution"]["stderr_byte_count"], 6)
                self.assertEqual(json.loads(output.read_text(encoding="utf-8")),
                                 attempt)

    def test_controlled_executor_requires_confirmation_and_saves_tool_failure(self):
        plan = {**self.plan(), "probe_serial": None}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); output = root / "attempt.json"
            with self.assertRaisesRegex(FirmwareDeploymentError, "--execute"):
                execute_deployment_plan(
                    plan, output_root=root, reader=object(),
                    attempt_output=output, execute=False,
                    confirmation=EXECUTION_CONFIRMATION,
                    started_utc="2026-09-11T01:00:00Z",
                    ended_utc_provider=lambda: "2026-09-11T01:00:01Z")
            self.assertFalse(output.exists())

            def fake_deploy(*args, **kwargs):
                kwargs["runner"](("fake-tool",), 10)

            with patch("deployment_attempt.validate_stlink_deployment_plan",
                       return_value=plan), \
                    patch("deployment_attempt.deploy_stlink",
                          side_effect=fake_deploy):
                attempt = execute_deployment_plan(
                    plan, output_root=root, reader=object(),
                    attempt_output=output, execute=True,
                    confirmation=EXECUTION_CONFIRMATION,
                    runner=lambda *_: (7, b"partial", b"failed"),
                    started_utc="2026-09-11T01:00:00Z",
                    ended_utc_provider=lambda: "2026-09-11T01:00:01Z")
            self.assertEqual(attempt["outcome"], "failed")
            self.assertEqual(attempt["execution"]["exit_code"], 7)
            self.assertEqual(attempt["readback"]["status"], "absent")

    def test_controlled_executor_saves_failure_before_tool_invocation(self):
        plan = {**self.plan(), "probe_serial": None}
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); output = root / "attempt.json"
            with patch("deployment_attempt.validate_stlink_deployment_plan",
                       return_value=plan), \
                    patch("deployment_attempt.deploy_stlink",
                          side_effect=RuntimeError("mock setup failure")):
                attempt = execute_deployment_plan(
                    plan, output_root=root, reader=object(),
                    attempt_output=output, execute=True,
                    confirmation=EXECUTION_CONFIRMATION,
                    started_utc="2026-09-11T01:00:00Z",
                    ended_utc_provider=lambda: "2026-09-11T01:00:01Z")
            self.assertEqual(attempt["outcome"], "failed")
            self.assertFalse(attempt["execution"]["tool_invoked"])
            self.assertIsNone(attempt["execution"]["exit_code"])
            self.assertEqual(attempt["execution"]["error_type"], "RuntimeError")
            self.assertEqual(attempt["readback"]["status"], "absent")
            self.assertEqual(json.loads(output.read_text(encoding="utf-8")),
                             attempt)


if __name__ == "__main__":
    unittest.main()
