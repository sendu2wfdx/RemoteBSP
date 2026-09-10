import json
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from urllib.request import Request, urlopen

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from deployment_attempt import EXECUTION_CONFIRMATION  # noqa: E402
from server import make_server  # noqa: E402
from studio_deployment_workflow import StudioDeploymentWorkflow  # noqa: E402


class StudioDeploymentWorkflowHttpTests(unittest.TestCase):
    def test_real_http_preflight_is_offline_and_execute_projects_attempt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reader_factory = Mock()
            executor = Mock(return_value={
                "sha256": "e" * 64, "outcome": "failed",
                "hardware_success_claimed": False,
                "execution": {"status": "failed", "tool_invoked": True,
                              "error_type": "MockFlashError"},
                "readback": {"status": "absent", "error_type": None}})
            workflow = StudioDeploymentWorkflow(
                output_root=root, attempt_root=root / "attempts",
                reader_factory=reader_factory,
                backend_config={"stlink-openocd": {"probe_serial": None}},
                executor=executor)
            plan = {"format": "REMOTEBSP_STLINK_DEPLOYMENT_PLAN_V1",
                    "sha256": "a" * 64,
                    "build_id": "board-build-01234567",
                    "backend": "stlink-openocd", "hardware_access": False,
                    "flash_performed": False, "expected_identity": {
                        "board_id": "board", "project_sha256": "b" * 64,
                        "config_sha256": "c" * 64,
                        "firmware_identity_sha256": "d" * 64}}
            server = make_server("127.0.0.1", 0, None,
                                 studio_deployment_workflow=workflow)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"

            def post(path, value):
                request = Request(base + path,
                    data=json.dumps(value).encode(), method="POST",
                    headers={"Content-Type": "application/json"})
                return json.loads(urlopen(request).read())

            try:
                with patch(
                    "studio_deployment_workflow.create_stlink_deployment_plan",
                    return_value=plan):
                    preflight = post("/api/deployment/preflight", {
                        "build_id": plan["build_id"],
                        "expected_uuid": "ab" * 16})
                self.assertEqual(preflight["plan_sha256"], "a" * 64)
                self.assertEqual(preflight["attempt"]["execution_status"],
                                 "absent")
                self.assertFalse(preflight["hardware_access_performed"])
                reader_factory.assert_not_called()
                result = post("/api/deployment/execute", {
                    "confirmation_token": preflight["confirmation_token"],
                    "confirmation": EXECUTION_CONFIRMATION,
                    "flash_timeout": 120, "reconnect_timeout": 10,
                    "poll_interval": .25})
                self.assertEqual(result["attempt"]["execution_status"], "failed")
                self.assertEqual(result["attempt"]["readback_status"], "absent")
                self.assertEqual(result["attempt"]["failure_type"],
                                 "MockFlashError")
                reader_factory.assert_called_once_with("ab" * 16)
                with patch.object(workflow, "history", return_value={
                        "ok": True, "format": "STUDIO_DEPLOYMENT_HISTORY_V1",
                        "items": [], "damaged": [], "read_only": True,
                        "reexecution_allowed": False}) as history:
                    restored = json.loads(urlopen(
                        base + "/api/deployment/history?backend=stlink-openocd"
                    ).read())
                self.assertTrue(restored["read_only"])
                history.assert_called_once_with(build_id="",
                                                backend="stlink-openocd")
                with patch.object(workflow, "export_manifest", return_value={
                        "format": "STUDIO_DEPLOYMENT_EVIDENCE_MANIFEST_V1",
                        "references": [], "sha256": "f" * 64}) as export:
                    manifest = post("/api/deployment/history/export", {
                        "build_id": "", "backend": "stlink-openocd"})
                self.assertEqual(manifest["sha256"], "f" * 64)
                export.assert_called_once_with(build_id="",
                                               backend="stlink-openocd")
                with patch.object(workflow, "create_evidence_bundle",
                        return_value={"ok": True,
                        "format": "REMOTEBSP_DEPLOYMENT_EVIDENCE_BUNDLE_V1",
                        "package_sha256": "1" * 64}) as bundle:
                    packaged = post("/api/deployment/history/bundle", {
                        "attempt_filename": "build-0000-部署尝试-v1.json"})
                self.assertEqual(packaged["package_sha256"], "1" * 64)
                bundle.assert_called_once_with("build-0000-部署尝试-v1.json")
                with patch.object(workflow, "import_evidence_bundle",
                        return_value={"ok": True, "imported": True,
                                      "hardware_access": False}) as importer:
                    imported = post("/api/deployment/history/bundle/import", {
                        "bundle_base64": "UEs="})
                self.assertTrue(imported["imported"])
                self.assertFalse(imported["hardware_access"])
                importer.assert_called_once_with("UEs=")
            finally:
                server.shutdown(); server.server_close(); thread.join()


if __name__ == "__main__":
    unittest.main()
