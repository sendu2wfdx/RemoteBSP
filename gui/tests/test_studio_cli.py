import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch


GUI_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = GUI_ROOT.parent
sys.path.insert(0, str(GUI_ROOT))

from firmware_builder import (  # noqa: E402
    BuildArtifact,
    FirmwareBuildError,
    FirmwareBuildResult,
)
from firmware_deployment import (  # noqa: E402
    DeploymentResult,
    DeviceIdentity,
    FirmwareDeploymentError,
    FirmwareIdentity,
    IdentityCapabilityError,
)
from device_parameters import DeviceParameterError  # noqa: E402
from production_batch import export_production_batch  # noqa: E402
from production_record import generate_production_record  # noqa: E402
from studio_cli import (  # noqa: E402
    EXIT_INPUT,
    EXIT_OK,
    EXIT_OPERATION,
    EXIT_USAGE,
    main,
)


CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
PROJECT_PATH = REPO_ROOT / "tests" / "data" / "studio_bus_project_v2.json"


class StudioCliTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.directory = Path(self.temporary.name)
        self.catalog = json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
        self.project = json.loads(PROJECT_PATH.read_text(encoding="utf-8"))
        self.audit_key = self.directory / "audit.key"
        self.audit_key.write_bytes(b"k" * 32)
        record = generate_production_record(self.project, self.catalog)
        self.record_path = self.directory / "production-record.json"
        self.record_path.write_bytes(record.content)
        batch = export_production_batch(
            batch_id="cli-batch-001", name="CLI批次", note="CI测试",
            production_records=[record.record], comparison_exports=[])
        self.manifest_path = self.directory / "batch-manifest.json"
        self.manifest_path.write_text(json.dumps(
            batch.manifest, ensure_ascii=False, sort_keys=True, indent=2) +
            "\n", encoding="utf-8")

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def _call(arguments):
        stdout = io.StringIO()
        stderr = io.StringIO()
        code = main(arguments, stdout=stdout, stderr=stderr)
        output = stdout.getvalue() or stderr.getvalue()
        return code, json.loads(output), stdout.getvalue(), stderr.getvalue()

    def test_project_validation_is_stable_and_strict(self):
        arguments = ["project-validate", "--project", str(PROJECT_PATH)]
        first = self._call(arguments)
        second = self._call(arguments)
        self.assertEqual(first[0], EXIT_OK)
        self.assertEqual(first[2], second[2])
        self.assertTrue(first[1]["ok"])
        self.assertEqual(first[1]["format"],
                         "STUDIO_CLI_PROJECT_VALIDATION_V1")
        self.assertEqual(first[1]["execution_status"]["firmware_flash"],
                         "not_performed")
        self.assertFalse(first[1]["execution_status"]["hardware_access"])

        code, error, _, stderr = self._call(["unknown-command"])
        self.assertEqual(code, EXIT_USAGE)
        self.assertEqual(error["exit_code"], EXIT_USAGE)
        self.assertEqual(stderr.count("\n"), 1)
        code, error, _, _ = self._call([
            "project-validate", "--project", "x" * 513])
        self.assertEqual(code, EXIT_INPUT)
        self.assertEqual(error["exit_code"], EXIT_INPUT)

    def test_build_dry_run_never_invokes_builder_or_creates_output(self):
        build_root = self.directory / "build"
        output_root = self.directory / "out"
        with patch("studio_cli.build_firmware_project") as builder:
            code, response, _, _ = self._call([
                "build", "--project", str(PROJECT_PATH),
                "--build-root", str(build_root),
                "--output-root", str(output_root), "--dry-run"])
        self.assertEqual(code, EXIT_OK)
        builder.assert_not_called()
        self.assertTrue(response["dry_run"])
        self.assertFalse(response["build_output_written"])
        self.assertFalse(build_root.exists())
        self.assertFalse(output_root.exists())
        self.assertEqual(response["execution_status"]["firmware_flash"],
                         "not_performed")
        code, error, _, _ = self._call([
            "build", "--project", str(PROJECT_PATH), "--dry-run",
            "--build-root", "x" * 513])
        self.assertEqual(code, EXIT_INPUT)
        self.assertIn("512", error["error"])

    def test_explicit_build_reuses_backend_and_reports_operation_exit(self):
        fake = FirmwareBuildResult(
            build_id="board-0123456789abcdef", board_id="board",
            firmware_target="target", config_sha256="c" * 64,
            output_dir=self.directory / "out" / "build",
            artifacts=(BuildArtifact("firmware.bin", 4, "a" * 64),),
            record={"project_sha256": "b" * 64, "memory": {}})
        arguments = ["build", "--project", str(PROJECT_PATH),
                     "--build-root", str(self.directory / "build"),
                     "--output-root", str(self.directory / "out")]
        with patch("studio_cli.build_firmware_project", return_value=fake) \
                as builder:
            code, response, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OK)
        builder.assert_called_once()
        self.assertEqual(response["build_id"], fake.build_id)
        self.assertEqual(response["execution_status"]["software_build"],
                         "performed")
        self.assertEqual(response["execution_status"]["firmware_flash"],
                         "not_performed")

        with patch("studio_cli.build_firmware_project",
                   side_effect=FirmwareBuildError("模拟构建失败")):
            code, error, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OPERATION)
        self.assertEqual(error["exit_code"], EXIT_OPERATION)

    def test_explicit_stlink_deployment_uses_identity_file_and_reports_result(self):
        identity_file = self.directory / "identity.json"
        identity_file.write_text("{}", encoding="utf-8")
        expected = FirmwareIdentity(
            "weact-g431-core-v10", "a" * 64, "b" * 64, "c" * 64)
        observed = DeviceIdentity(
            "weact-g431-core-v10", "a" * 64, "b" * 64, "c" * 64,
            "ab" * 16)
        result = DeploymentResult(
            "weact-g431-core-v10-01234567", "stlink-openocd", expected,
            observed, 2, True)
        arguments = [
            "deploy-stlink", "--build-id", result.build_id,
            "--output-root", str(self.directory / "out"),
            "--identity-file", str(identity_file),
            "--probe-serial", "probe-1", "--flash-timeout", "30",
            "--reconnect-timeout", "4", "--poll-interval", "0.1",
            "--record-output", str(self.directory / "deployment.json"),
        ]
        deployment_record = type("DeploymentRecord", (), {
            "record": {"status": "firmware_flash_verified"},
            "sha256": "d" * 64, "filename": "deployment.json",
            "content": b"{}\n",
        })()
        with patch("studio_cli.deploy_stlink", return_value=result) as deploy, \
                patch("studio_cli.create_deployment_record",
                      return_value=deployment_record):
            code, response, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OK)
        reader = deploy.call_args.args[1]
        self.assertEqual(reader.path, identity_file.resolve())
        self.assertEqual(deploy.call_args.kwargs["probe_serial"], "probe-1")
        self.assertEqual(deploy.call_args.kwargs["flash_timeout"], 30)
        self.assertTrue(response["verified"])
        self.assertEqual(response["deployment_record_sha256"], "d" * 64)
        self.assertEqual((self.directory / "deployment.json").read_bytes(),
                         b"{}\n")
        self.assertEqual(response["execution_status"]["firmware_flash"],
                         "performed_and_verified")
        self.assertTrue(response["execution_status"]["hardware_access"])

        with patch("studio_cli.deploy_stlink", side_effect=
                   FirmwareDeploymentError("模拟身份核对失败")):
            code, error, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OPERATION)
        self.assertIn("身份核对失败", error["error"])

    def test_explicit_can_katapult_deployment_passes_target_and_records(self):
        identity_file = self.directory / "identity.json"
        identity_file.write_text("{}", encoding="utf-8")
        flashtool = self.directory / "flashtool.py"
        flashtool.write_text("# test", encoding="utf-8")
        expected = FirmwareIdentity(
            "weact-g431-core-v10", "a" * 64, "b" * 64, "c" * 64)
        observed = DeviceIdentity(
            "weact-g431-core-v10", "a" * 64, "b" * 64, "c" * 64,
            "ab" * 16)
        result = DeploymentResult(
            "weact-g431-core-v10-01234567", "can-katapult", expected,
            observed, 1, True)
        record = type("DeploymentRecord", (), {
            "record": {"status": "firmware_flash_verified"},
            "sha256": "e" * 64, "filename": "deployment.json",
            "content": b"{}\n",
        })()
        arguments = [
            "deploy-can-katapult", "--build-id", result.build_id,
            "--output-root", str(self.directory / "out"),
            "--identity-file", str(identity_file),
            "--can-interface", "can0", "--katapult-uuid", "ABCDEF123456",
            "--flashtool", str(flashtool),
            "--record-output", str(self.directory / "katapult.json"),
        ]
        with patch("studio_cli.deploy_can_katapult",
                   return_value=result) as deploy, \
                patch("studio_cli.create_deployment_record",
                      return_value=record):
            code, response, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OK)
        self.assertEqual(response["backend"], "can-katapult")
        self.assertTrue(response["verified"])
        self.assertEqual(deploy.call_args.kwargs["can_interface"], "can0")
        self.assertEqual(deploy.call_args.kwargs["katapult_uuid"],
                         "ABCDEF123456")
        self.assertEqual((self.directory / "katapult.json").read_bytes(),
                         b"{}\n")

    def test_runtime_identity_inspection_is_complete_but_not_deployment(self):
        arguments = [
            "inspect-runtime-identity",
            "--toolbusd-socket", "/tmp/toolbusd.sock", "--node-id", "7",
            "--node-uuid", "ab" * 16, "--remote-cli", "/bin/remote-cli",
            "--identity-timeout", "1.25",
        ]
        runtime_node = type("RuntimeNode", (), {
            "node_id": 7, "board_id": "weact-g431-core-v10",
            "device_uuid": "ab" * 16, "online": True, "ready": True,
            "firmware_version": (0, 2, 0), "protocol_version": 1,
        })()
        with patch("studio_cli.ToolbusdIdentityReader") as reader_type:
            reader_type.return_value.read_identity.return_value = DeviceIdentity(
                "weact-g431-core-v10", "a" * 64, "b" * 64,
                "c" * 64, "ab" * 16)
            reader_type.return_value.read_runtime_node.return_value = runtime_node
            code, response, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OK)
        reader_type.assert_called_once_with(
            "/tmp/toolbusd.sock", 7, expected_uuid="ab" * 16,
            remote_cli="/bin/remote-cli", timeout=1.25)
        self.assertTrue(response["identity_complete"])
        self.assertFalse(response["deployment_verified"])
        self.assertEqual(response["firmware"], {
            "major": 0, "minor": 2, "patch": 0})
        self.assertEqual(response["project_sha256"], "a" * 64)
        self.assertEqual(response["config_sha256"], "b" * 64)
        self.assertEqual(response["firmware_identity_sha256"], "c" * 64)
        self.assertEqual(response["capabilities_missing"], [])
        self.assertFalse(response["execution_status"]["hardware_access"])

        with patch("studio_cli.ToolbusdIdentityReader") as reader_type:
            reader_type.return_value.read_identity.side_effect = \
                IdentityCapabilityError(
                    "缺字段", ("config_sha256",))
            reader_type.return_value.read_runtime_node.return_value = runtime_node
            code, response, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OK)
        self.assertFalse(response["identity_complete"])
        self.assertIsNone(response["project_sha256"])
        self.assertEqual(response["capabilities_missing"], [
            "config_sha256"])
        self.assertFalse(response["deployment_verified"])

        code, error, _, _ = self._call([
            "deploy-stlink", "--build-id", "weact-g431-core-v10-01234567",
            "--toolbusd-socket", "/tmp/toolbusd.sock"])
        self.assertEqual(code, EXIT_USAGE)
        self.assertEqual(error["exit_code"], EXIT_USAGE)

    def test_device_parameter_write_is_explicit_cli_only(self):
        snapshot = {"schema_version": 1, "node_uuid": "ab" * 16,
                    "status": {"generation": 8}, "parameters": []}
        arguments = [
            "device-parameter-write", "--toolbusd-socket", "/tmp/toolbusd.sock",
            "--node-id", "7", "--expected-uuid", "ab" * 16,
            "--expected-generation", "7", "--parameter-id", "0x100",
            "--value-base64", "bmV3", "--confirmation",
            "WRITE_DEVICE_PARAMETERS", "--remote-cli", "/bin/remote-cli",
            "--parameter-timeout", "1.5",
            "--audit-dir", str(self.directory / "write-audit"),
            "--audit-key-file", str(self.audit_key),
        ]
        with patch("studio_cli.DeviceParameterManager") as manager_type:
            manager_type.return_value.write.return_value = snapshot
            code, response, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OK)
        manager_type.assert_called_once_with(
            "/tmp/toolbusd.sock", 7, remote_cli="/bin/remote-cli", timeout=1.5)
        manager_type.return_value.write.assert_called_once_with(
            expected_uuid="ab" * 16, expected_generation=7,
            parameter_id=0x100, value_base64="bmV3",
            confirmation="WRITE_DEVICE_PARAMETERS")
        self.assertTrue(response["execution_status"]["hardware_access"])
        self.assertRegex(response["audit_operation_id"], r"^[0-9a-f]{32}$")

        failed_arguments = list(arguments)
        failed_arguments[failed_arguments.index(str(
            self.directory / "write-audit"))] = str(
                self.directory / "failed-write-audit")
        with patch("studio_cli.DeviceParameterManager") as manager_type:
            manager_type.return_value.write.side_effect = DeviceParameterError(
                "设备参数恢复在完成1项后中止；模拟错误")
            code, error, _, _ = self._call(failed_arguments)
        self.assertEqual(code, EXIT_OPERATION)
        records = list((self.directory / "failed-write-audit").glob(
            "audit-*.json"))
        self.assertEqual(len(records), 1)
        audit = json.loads(records[0].read_text(encoding="utf-8"))
        self.assertEqual(audit["events"][1]["outcome"], "partial_failure")
        self.assertEqual(audit["events"][1]["applied_count"], 1)
        self.assertNotIn("bmV3", records[0].read_text(encoding="utf-8"))

    def test_device_parameter_restore_reads_bounded_backup(self):
        backup = self.directory / "parameters.json"
        backup.write_text(json.dumps({
            "schema_version": 1, "node_uuid": "ab" * 16,
            "parameters": [{"id": 0x100, "value_base64": "b2xk"}]}),
            encoding="utf-8")
        arguments = [
            "device-parameter-restore", "--toolbusd-socket", "/tmp/toolbusd.sock",
            "--expected-uuid", "ab" * 16, "--expected-generation", "7",
            "--confirmation", "WRITE_DEVICE_PARAMETERS",
            "--backup", str(backup),
            "--audit-dir", str(self.directory / "restore-audit"),
            "--audit-key-file", str(self.audit_key),
        ]
        with patch("studio_cli.DeviceParameterManager") as manager_type:
            manager_type.return_value.restore.return_value = {
                "status": {"generation": 7}}
            code, response, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OK)
        passed = manager_type.return_value.restore.call_args.args[0]
        self.assertEqual(passed["node_uuid"], "ab" * 16)
        self.assertEqual(response["format"],
                         "STUDIO_CLI_DEVICE_PARAMETER_RESTORE_V1")

    def test_batch_create_validate_and_atomic_archive_output(self):
        archive = self.directory / "batch.zip"
        arguments = [
            "batch-create", "--batch-id", "cli-batch-001",
            "--name", "CLI批次", "--production-record",
            str(self.record_path), "--archive-output", str(archive),
            "--dry-run",
        ]
        code, dry, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OK)
        self.assertTrue(dry["dry_run"])
        self.assertFalse(dry["archive_written"])
        self.assertFalse(archive.exists())

        arguments.remove("--dry-run")
        code, created, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OK)
        self.assertTrue(created["archive_written"])
        self.assertTrue(zipfile.is_zipfile(archive))
        code, error, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_INPUT)
        self.assertIn("--force", error["error"])

        code, validated, _, _ = self._call([
            "batch-validate", "--manifest", str(self.manifest_path)])
        self.assertEqual(code, EXIT_OK)
        self.assertTrue(validated["validation"]["valid"])

    def test_offline_ed25519_signing_cli_and_tamper_failure(self):
        private = self.directory / "signing-private.pem"
        public = self.directory / "signing-public.pem"
        signature = self.directory / "batch-signature.json"
        code, generated, _, _ = self._call([
            "signing-keygen", "--private-key-output", str(private),
            "--public-key-output", str(public)])
        self.assertEqual(code, EXIT_OK)
        self.assertEqual(generated["time_authority"], "none")

        code, signed, _, _ = self._call([
            "evidence-sign", "--evidence", str(self.manifest_path),
            "--private-key", str(private), "--signature-output", str(signature)])
        self.assertEqual(code, EXIT_OK)
        self.assertFalse(signed["time_trusted"])
        code, verified, _, _ = self._call([
            "evidence-verify", "--evidence", str(self.manifest_path),
            "--signature", str(signature), "--public-key", str(public)])
        self.assertEqual(code, EXIT_OK)
        self.assertTrue(verified["verification"]["valid"])

        changed = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        changed["batch"]["name"] = "篡改批次"
        changed_path = self.directory / "changed.json"
        changed_path.write_text(json.dumps(changed), encoding="utf-8")
        code, error, _, _ = self._call([
            "evidence-verify", "--evidence", str(changed_path),
            "--signature", str(signature), "--public-key", str(public)])
        self.assertEqual(code, EXIT_INPUT)
        self.assertFalse(error["ok"])

    def test_history_save_dry_run_save_and_search(self):
        history = self.directory / "history"
        code, error, _, _ = self._call([
            "history-search", "--history-root", str(history)])
        self.assertEqual(code, EXIT_INPUT)
        self.assertIn("history-save", error["error"])
        self.assertFalse(history.exists())
        dry_arguments = [
            "history-save", "--manifest", str(self.manifest_path),
            "--history-root", str(history), "--dry-run",
        ]
        code, dry, _, _ = self._call(dry_arguments)
        self.assertEqual(code, EXIT_OK)
        self.assertTrue(dry["dry_run"])
        self.assertFalse(history.exists())
        too_long = list(dry_arguments)
        too_long[4] = "x" * 513
        code, error, _, _ = self._call(too_long)
        self.assertEqual(code, EXIT_INPUT)
        self.assertIn("512", error["error"])

        dry_arguments.remove("--dry-run")
        code, saved, _, _ = self._call(dry_arguments)
        self.assertEqual(code, EXIT_OK)
        self.assertTrue(saved["stored"])
        code, repeated, _, _ = self._call(dry_arguments)
        self.assertEqual(code, EXIT_OK)
        self.assertTrue(repeated["already_exists"])

        board = self.batch_board_id()
        code, search, _, _ = self._call([
            "history-search", "--history-root", str(history),
            "--field", "board_id", "--query", board])
        self.assertEqual(code, EXIT_OK)
        self.assertEqual(search["total_matches"], 1)

    def batch_board_id(self):
        manifest = json.loads(self.manifest_path.read_text(encoding="utf-8"))
        return manifest["records"][0]["board_id"]

    def test_oversized_input_has_validation_exit_code(self):
        oversized = self.directory / "oversized.json"
        oversized.write_bytes(b"{" + b" " * (128 * 1024) + b"}")
        code, error, _, _ = self._call([
            "project-validate", "--project", str(oversized)])
        self.assertEqual(code, EXIT_INPUT)
        self.assertIn("128 KiB", error["error"])

    def test_duplicate_json_and_atomic_output_race_are_rejected(self):
        duplicate = self.directory / "duplicate.json"
        duplicate.write_text('{"schema_version":2,"schema_version":2}',
                             encoding="utf-8")
        code, error, _, _ = self._call([
            "project-validate", "--project", str(duplicate)])
        self.assertEqual(code, EXIT_INPUT)
        self.assertIn("重复字段", error["error"])

        archive = self.directory / "race.zip"
        arguments = [
            "batch-create", "--batch-id", "cli-batch-race",
            "--name", "并发保护", "--production-record",
            str(self.record_path), "--archive-output", str(archive),
        ]
        with patch("studio_cli.os.link", side_effect=FileExistsError(
                "模拟并发创建")):
            code, error, _, _ = self._call(arguments)
        self.assertEqual(code, EXIT_OPERATION)
        self.assertFalse(archive.exists())
        self.assertIn("模拟并发创建", error["error"])


if __name__ == "__main__":
    unittest.main()
