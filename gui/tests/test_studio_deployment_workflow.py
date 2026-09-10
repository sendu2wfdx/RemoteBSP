import sys
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from deployment_attempt import EXECUTION_CONFIRMATION  # noqa: E402
from firmware_deployment import FirmwareDeploymentError  # noqa: E402
from studio_deployment_workflow import StudioDeploymentWorkflow  # noqa: E402


class StudioDeploymentWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.reader_factory = Mock()
        self.executor = Mock()
        self.workflow = StudioDeploymentWorkflow(
            output_root=self.root, attempt_root=self.root / "attempts",
            reader_factory=self.reader_factory,
            backend_config={"stlink-openocd": {"probe_serial": "probe"},
                "can-katapult": {"can_interface": "can0",
                                  "flashtool": str(self.root / "flash.py")},
                "usb-katapult": {"usb_device": "/dev/serial/by-id/test",
                                  "flashtool": str(self.root / "flash.py")}},
            executor=self.executor)

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def plan(backend):
        formats = {"stlink-openocd": "REMOTEBSP_STLINK_DEPLOYMENT_PLAN_V1",
                   "can-katapult": "REMOTEBSP_CAN_KATAPULT_DEPLOYMENT_PLAN_V1",
                   "usb-katapult": "REMOTEBSP_USB_KATAPULT_DEPLOYMENT_PLAN_V1"}
        hashes = {"stlink-openocd": "a", "can-katapult": "e",
                  "usb-katapult": "f"}
        return {"format": formats[backend], "sha256": hashes[backend] * 64,
                "build_id": "board-build-01234567", "backend": backend,
                "hardware_access": False, "flash_performed": False,
                "expected_identity": {"board_id": "board",
                    "project_sha256": "b" * 64, "config_sha256": "c" * 64,
                    "firmware_identity_sha256": "d" * 64}}

    def test_three_preflights_are_offline_and_expose_absent_state(self):
        cases = (("stlink-openocd", "create_stlink_deployment_plan", None),
                 ("can-katapult", "create_can_katapult_deployment_plan",
                  {"katapult_uuid": "abcdef"}),
                 ("usb-katapult", "create_usb_katapult_deployment_plan", None))
        for backend, creator, target in cases:
            with self.subTest(backend=backend), patch(
                    f"studio_deployment_workflow.{creator}",
                    return_value=self.plan(backend)):
                view = self.workflow.preflight(
                    backend=backend, build_id="board-build-01234567",
                    expected_uuid="ab" * 16, target=target)
            self.assertEqual(len(view["plan_sha256"]), 64)
            self.assertEqual(view["attempt"]["execution_status"], "absent")
            self.assertEqual(view["attempt"]["readback_status"], "absent")
            self.assertFalse(view["hardware_access_performed"])
        self.reader_factory.assert_not_called()
        self.executor.assert_not_called()

    def test_execute_requires_second_confirmation_and_returns_attempt_state(self):
        with patch("studio_deployment_workflow.create_stlink_deployment_plan",
                   return_value=self.plan("stlink-openocd")):
            view = self.workflow.preflight(
                backend="stlink-openocd", build_id="board-build-01234567",
                expected_uuid="ab" * 16)
        with self.assertRaisesRegex(FirmwareDeploymentError, "二次确认"):
            self.workflow.execute(
                confirmation_token=view["confirmation_token"], execute=False,
                confirmation=EXECUTION_CONFIRMATION, expected_uuid="ab" * 16)
        self.reader_factory.assert_not_called()
        # 确认失败不消耗令牌；用户仍可修正确认内容。
        failed = {"sha256": "e" * 64, "outcome": "failed",
                  "hardware_success_claimed": False,
                  "execution": {"status": "failed", "tool_invoked": True,
                                "error_type": "MockToolError"},
                  "readback": {"status": "absent", "error_type": None}}
        self.executor.return_value = failed
        result = self.workflow.execute(
            confirmation_token=view["confirmation_token"], execute=True,
            confirmation=EXECUTION_CONFIRMATION, expected_uuid="ab" * 16)
        self.assertEqual(result["attempt"]["outcome"], "failed")
        self.assertEqual(result["attempt"]["failure_type"], "MockToolError")
        self.assertFalse(result["attempt"]["hardware_success_claimed"])
        self.reader_factory.assert_called_once_with("ab" * 16)

    def test_restart_history_is_read_only_filters_and_isolates_damage(self):
        plan = self.plan("stlink-openocd")
        with patch("studio_deployment_workflow.create_stlink_deployment_plan",
                   return_value=plan):
            self.workflow.preflight(
                backend="stlink-openocd", build_id=plan["build_id"],
                expected_uuid="ab" * 16)
        from deployment_attempt import create_absent_attempt
        attempt = create_absent_attempt(plan)
        attempt_path = self.workflow.attempt_root / \
            f"{plan['build_id']}-0000000000000000-部署尝试-v1.json"
        attempt_path.write_text(json.dumps(attempt), encoding="utf-8")
        (self.workflow.attempt_root / "broken-部署尝试-v1.json").write_text(
            "{", encoding="utf-8")
        restarted = StudioDeploymentWorkflow(
            output_root=self.root, attempt_root=self.workflow.attempt_root,
            reader_factory=self.reader_factory,
            backend_config=self.workflow.backend_config,
            executor=self.executor)
        with patch("studio_deployment_workflow.validate_stlink_deployment_plan",
                   return_value=plan):
            history = restarted.history(backend="stlink-openocd")
            manifest = restarted.export_manifest(backend="stlink-openocd")
        self.assertEqual(len(history["items"]), 1)
        self.assertEqual(len(history["damaged"]), 1)
        self.assertFalse(history["reexecution_allowed"])
        self.assertEqual(len(manifest["references"]), 2)
        self.assertFalse(manifest["files_copied"])
        self.assertEqual(len(manifest["sha256"]), 64)
        self.reader_factory.assert_not_called()
        self.executor.assert_not_called()


if __name__ == "__main__":
    unittest.main()
