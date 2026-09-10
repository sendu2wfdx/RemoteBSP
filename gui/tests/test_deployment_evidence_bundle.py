import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

GUI_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GUI_ROOT))

from deployment_attempt import create_absent_attempt  # noqa: E402
from deployment_evidence_bundle import (  # noqa: E402
    create_bundle, import_bundle, verify_bundle, _digest)
from firmware_deployment import FirmwareDeploymentError  # noqa: E402


class DeploymentEvidenceBundleTests(unittest.TestCase):
    def test_deterministic_bundle_and_offline_verify_preserve_absent(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = {}
            for name, data in {"studio-project.json": b"{}\n",
                               "firmware.config": b"CONFIG_TEST=y\n",
                               "build-record.json": b"{}\n",
                               "firmware.elf": b"ELF-test"}.items():
                path = root / name; path.write_bytes(data); files[name] = path
            import hashlib
            plan = {"format": "REMOTEBSP_STLINK_DEPLOYMENT_PLAN_V1",
                "schema_version": 1, "backend": "stlink-openocd",
                "build_id": "board-build-01234567", "board_id": "board",
                "probe_serial": None, "command_argv": ["openocd"],
                "expected_identity": {"board_id": "board",
                    "project_sha256": "b" * 64, "config_sha256": "c" * 64,
                    "firmware_identity_sha256": "d" * 64},
                "evidence": {"studio_project_sha256": hashlib.sha256(files["studio-project.json"].read_bytes()).hexdigest(),
                    "firmware_config_sha256": hashlib.sha256(files["firmware.config"].read_bytes()).hexdigest(),
                    "build_record_sha256": hashlib.sha256(files["build-record.json"].read_bytes()).hexdigest(),
                    "firmware_elf_sha256": hashlib.sha256(files["firmware.elf"].read_bytes()).hexdigest()},
                "hardware_access": False, "flash_performed": False}
            plan["sha256"] = _digest(plan)
            attempt = create_absent_attempt(plan)
            one, two = root / "one.zip", root / "two.zip"
            with patch("deployment_evidence_bundle.validate_stlink_deployment_plan",
                       return_value=plan), patch(
                       "deployment_evidence_bundle._VALIDATORS",
                       {plan["format"]: lambda value, output_root: plan}), patch(
                       "deployment_evidence_bundle.resolve_artifact",
                       side_effect=lambda build_id, name, output_root: files[name]):
                first = create_bundle(plan=plan, attempt=attempt,
                    output_root=root, output=one)
                second = create_bundle(plan=plan, attempt=attempt,
                    output_root=root, output=two)
            self.assertEqual(one.read_bytes(), two.read_bytes())
            self.assertEqual(first["package_sha256"], second["package_sha256"])
            verified = verify_bundle(one)
            self.assertEqual(verified["outcome"], "absent")
            self.assertFalse(verified["hardware_success_claimed"])
            imported = import_bundle(one, root / "imported")
            duplicate = import_bundle(two, root / "imported")
            self.assertTrue(imported["imported"])
            self.assertFalse(imported["deduplicated"])
            self.assertTrue(duplicate["deduplicated"])
            self.assertFalse(imported["hardware_access"])
            with zipfile.ZipFile(one) as archive:
                self.assertEqual(archive.namelist(), sorted(archive.namelist()))

    def test_path_traversal_zip_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.zip"
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("../escape", b"x")
            with self.assertRaisesRegex(FirmwareDeploymentError, "路径"):
                verify_bundle(path)


if __name__ == "__main__":
    unittest.main()
