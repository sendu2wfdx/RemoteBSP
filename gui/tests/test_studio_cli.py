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
