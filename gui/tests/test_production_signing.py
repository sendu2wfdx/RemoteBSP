import copy
import json
import sys
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch


GUI_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = GUI_ROOT.parent
sys.path.insert(0, str(GUI_ROOT))

from production_batch import export_production_batch  # noqa: E402
from production_record import generate_production_record  # noqa: E402
from production_signing import (  # noqa: E402
    add_trusted_key, create_trust_policy, generate_key_pair,
    revoke_trusted_key, sign_evidence, validate_trust_policy,
    verify_evidence_with_policy)
from project_config import ProjectConfigError  # noqa: E402


class ProductionSigningTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        catalog = json.loads((GUI_ROOT / "data" / "pin_catalog.json").read_text(
            encoding="utf-8"))
        project = json.loads((REPO_ROOT / "tests" / "data" /
                              "studio_bus_project_v2.json").read_text(
                                  encoding="utf-8"))
        record = generate_production_record(project, catalog)
        self.manifest = export_production_batch(
            batch_id="signed-001", name="离线签名", note="本机时间不可信",
            production_records=[record.record], comparison_exports=[]).manifest
        self.private = self.root / "offline-private.pem"
        self.public = self.root / "offline-public.pem"
        generate_key_pair(self.private, self.public)

    def tearDown(self):
        self.temporary.cleanup()

    def test_sign_verify_and_reject_tampering(self):
        envelope = sign_evidence(
            self.manifest, self.private,
            clock=lambda: datetime(2026, 9, 11, tzinfo=timezone.utc))
        self.assertFalse(envelope["recorded_time"]["trusted"])
        verified = verify_evidence_with_policy(
            self.manifest, envelope, create_trust_policy([self.public]))
        self.assertTrue(verified["valid"])
        self.assertFalse(verified["time_trusted"])

        changed = copy.deepcopy(self.manifest)
        changed["batch"]["note"] = "已被修改"
        with self.assertRaises(ProjectConfigError):
            verify_evidence_with_policy(
                changed, envelope, create_trust_policy([self.public]))

        forged_time = copy.deepcopy(envelope)
        forged_time["recorded_time"]["trusted"] = True
        with self.assertRaises(ProjectConfigError):
            verify_evidence_with_policy(
                self.manifest, forged_time, create_trust_policy([self.public]))

        changed_time = copy.deepcopy(envelope)
        changed_time["recorded_time"]["value_utc"] = "2026-09-12T00:00:00Z"
        with self.assertRaises(ProjectConfigError):
            verify_evidence_with_policy(
                self.manifest, changed_time, create_trust_policy([self.public]))

        changed_note = copy.deepcopy(envelope)
        changed_note["recorded_time"]["note"] = "仍声明非可信，但说明已被改写。"
        with self.assertRaises(ProjectConfigError):
            verify_evidence_with_policy(
                self.manifest, changed_note, create_trust_policy([self.public]))

        extra_time_field = copy.deepcopy(envelope)
        extra_time_field["recorded_time"]["authority"] = "none"
        with self.assertRaises(ProjectConfigError):
            verify_evidence_with_policy(
                self.manifest, extra_time_field,
                create_trust_policy([self.public]))

    def test_wrong_key_and_existing_key_outputs_fail_closed(self):
        envelope = sign_evidence(self.manifest, self.private)
        other_private = self.root / "other-private.pem"
        other_public = self.root / "other-public.pem"
        generate_key_pair(other_private, other_public)
        with self.assertRaises(ProjectConfigError):
            verify_evidence_with_policy(
                self.manifest, envelope, create_trust_policy([other_public]))
        with self.assertRaises(ProjectConfigError):
            generate_key_pair(self.private, self.root / "third-public.pem")

    def test_policy_authorization_rotation_and_revocation(self):
        first_signature = sign_evidence(self.manifest, self.private)
        policy = create_trust_policy([self.public])
        verified = verify_evidence_with_policy(
            self.manifest, first_signature, policy)
        self.assertTrue(verified["trust_authorized"])

        second_private = self.root / "rotated-private.pem"
        second_public = self.root / "rotated-public.pem"
        second = generate_key_pair(second_private, second_public)
        rotated = add_trusted_key(policy, second_public)
        self.assertTrue(verify_evidence_with_policy(
            self.manifest, first_signature, rotated)["valid"])
        second_signature = sign_evidence(self.manifest, second_private)
        self.assertTrue(verify_evidence_with_policy(
            self.manifest, second_signature, rotated)["valid"])

        revoked = revoke_trusted_key(rotated, second["key_id"])
        with self.assertRaisesRegex(ProjectConfigError, "撤销"):
            verify_evidence_with_policy(
                self.manifest, second_signature, revoked)
        self.assertTrue(verify_evidence_with_policy(
            self.manifest, first_signature, revoked)["valid"])

    def test_policy_rejects_unknown_scope_and_malformed_keyring(self):
        signature = sign_evidence(self.manifest, self.private)
        other_private = self.root / "unknown-private.pem"
        other_public = self.root / "unknown-public.pem"
        generate_key_pair(other_private, other_public)
        with self.assertRaisesRegex(ProjectConfigError, "未被"):
            verify_evidence_with_policy(
                self.manifest, signature, create_trust_policy([other_public]))
        scoped = create_trust_policy(
            [self.public], authorized_kinds=("deployment_record",))
        with self.assertRaisesRegex(ProjectConfigError, "无权"):
            verify_evidence_with_policy(self.manifest, signature, scoped)
        malformed = create_trust_policy([self.public])
        malformed["keys"][0]["key_id"] = "ed25519:" + "0" * 64
        with self.assertRaisesRegex(ProjectConfigError, "身份"):
            validate_trust_policy(malformed)
        unknown_field = create_trust_policy([self.public])
        unknown_field["keys"][0]["activated_at"] = "2026-09-11T00:00:00Z"
        with self.assertRaisesRegex(ProjectConfigError, "字段集合"):
            validate_trust_policy(unknown_field)
        with self.assertRaisesRegex(ProjectConfigError, "1到16"):
            create_trust_policy([self.public] * 17)

    def test_concurrent_keygen_has_one_winner_without_overwrite(self):
        private = self.root / "race-private.pem"
        public = self.root / "race-public.pem"
        barrier = threading.Barrier(2)

        def generate():
            barrier.wait()
            try:
                return generate_key_pair(private, public)
            except ProjectConfigError:
                return None

        with ThreadPoolExecutor(max_workers=2) as pool:
            outcomes = list(pool.map(lambda _: generate(), range(2)))
        self.assertEqual(sum(item is not None for item in outcomes), 1)
        envelope = sign_evidence(self.manifest, private)
        self.assertTrue(verify_evidence_with_policy(
            self.manifest, envelope, create_trust_policy([public]))["valid"])
        self.assertEqual(list(self.root.glob(".remotebsp-key-*.tmp")), [])

    def test_second_publish_failure_cleans_pair_and_staging(self):
        private = self.root / "failed-private.pem"
        public = self.root / "failed-public.pem"
        import production_signing
        real_link = production_signing.os.link
        calls = 0

        def fail_second(source, destination):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("injected public publish failure")
            return real_link(source, destination)

        with patch("production_signing.os.link", side_effect=fail_second):
            with self.assertRaises(ProjectConfigError):
                generate_key_pair(private, public)
        self.assertFalse(private.exists())
        self.assertFalse(public.exists())
        self.assertEqual(list(self.root.glob(".remotebsp-key-*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
