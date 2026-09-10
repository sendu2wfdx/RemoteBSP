import sys
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
        return {"format": "PLAN", "sha256": "a" * 64,
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
            self.assertEqual(view["plan_sha256"], "a" * 64)
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


if __name__ == "__main__":
    unittest.main()
