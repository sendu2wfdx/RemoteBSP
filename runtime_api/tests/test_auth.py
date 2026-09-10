import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from runtime_api.auth import (
    CONTROL_LEASE_ACQUIRE_PERMISSION,
    CONTROL_LEASE_RELEASE_PERMISSION,
    CONTROL_LEASE_REVOKE_PERMISSION,
    MAXIMUM_AUTH_CONFIG_BYTES,
    MAXIMUM_API_KEYS,
    RUNTIME_READ_PERMISSION,
    ApiKeyCredential,
    ApiKeyAuthenticator,
    AuthConfigurationError,
    load_api_key_authenticator,
    load_reloading_api_key_authenticator,
)


class ApiKeyAuthenticatorTest(unittest.TestCase):
    KEY_A = "a" * 32
    KEY_B = "b" * 48

    def _write(self, directory: str, value: object) -> Path:
        path = Path(directory) / "runtime-auth.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    @staticmethod
    def _entry(key_id: str, api_key: str,
               permissions: list[str] | None = None) -> dict:
        return {
            "key_id": key_id,
            "api_key": api_key,
            "permissions": ([RUNTIME_READ_PERMISSION] if permissions is None
                            else permissions),
        }

    def test_load_and_verify_all_configured_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            authenticator = load_api_key_authenticator(self._write(
                directory, {"schema_version": 2, "keys": [
                    self._entry("reader-a", self.KEY_A),
                    self._entry("reader-b", self.KEY_B),
                ]}))
        self.assertEqual(authenticator.key_count, 2)
        principal = authenticator.authenticate(self.KEY_A)
        self.assertIsNotNone(principal)
        self.assertEqual(principal.key_id, "reader-a")
        self.assertEqual(principal.permissions,
                         frozenset({RUNTIME_READ_PERMISSION}))
        self.assertEqual(authenticator.authenticate(self.KEY_B).key_id,
                         "reader-b")
        self.assertFalse(authenticator.verify("x" * 32))

    def test_control_permissions_are_explicit_and_loadable(self):
        permissions = [
            CONTROL_LEASE_ACQUIRE_PERMISSION,
            CONTROL_LEASE_RELEASE_PERMISSION,
            CONTROL_LEASE_REVOKE_PERMISSION,
        ]
        with tempfile.TemporaryDirectory() as directory:
            authenticator = load_api_key_authenticator(self._write(
                directory, {"schema_version": 2, "keys": [
                    self._entry("controller", self.KEY_A, permissions),
                ]}))
        self.assertEqual(authenticator.authenticate(self.KEY_A).permissions,
                         frozenset(permissions))

    def test_verification_does_not_stop_at_first_match(self):
        authenticator = ApiKeyAuthenticator([
            ApiKeyCredential("reader-a", self.KEY_A,
                             frozenset({RUNTIME_READ_PERMISSION})),
            ApiKeyCredential("reader-b", self.KEY_B,
                             frozenset({RUNTIME_READ_PERMISSION})),
        ])
        calls: list[tuple[bytes, bytes]] = []

        def compare(left: bytes, right: bytes) -> bool:
            calls.append((left, right))
            return left == right

        with patch("runtime_api.auth.hmac.compare_digest",
                   side_effect=compare):
            self.assertTrue(authenticator.verify(self.KEY_A))
        self.assertEqual(len(calls), 2)
        self.assertTrue(all(len(left) == 32 and len(right) == 32
                            for left, right in calls))
        self.assertNotIn(self.KEY_A.encode("ascii"),
                         [right for _, right in calls])

    def test_configuration_is_bounded_and_closed(self):
        invalid_values = (
            {"schema_version": 2, "keys": []},
            {"schema_version": 2, "keys": [
                self._entry(f"reader-{index}", str(index).zfill(32))
                for index in range(MAXIMUM_API_KEYS + 1)]},
            {"schema_version": 2,
             "keys": [self._entry("reader", "short")]},
            {"schema_version": 2,
             "keys": [self._entry("reader", "a" * 31 + " ")]},
            {"schema_version": 2, "keys": [
                self._entry("reader-a", self.KEY_A),
                self._entry("reader-b", self.KEY_A),
            ]},
            {"schema_version": 2, "keys": [
                self._entry("reader", self.KEY_A),
                self._entry("reader", self.KEY_B),
            ]},
            {"schema_version": 2, "keys": [
                self._entry("reader", self.KEY_A, ["runtime.write"]),
            ]},
            {"schema_version": 2, "keys": [{
                **self._entry("reader", self.KEY_A), "unknown": True,
            }]},
            {"schema_version": 2, "keys": [{
                "key_id": "bad\nidentity", "api_key": self.KEY_A,
                "permissions": [RUNTIME_READ_PERMISSION],
            }]},
            {"schema_version": 2, "keys": [{
                "key_id": "reader", "api_key": self.KEY_A,
                "permissions": RUNTIME_READ_PERMISSION,
            }]},
            {"schema_version": 2, "keys": [
                self._entry("reader", self.KEY_A,
                            [RUNTIME_READ_PERMISSION,
                             RUNTIME_READ_PERMISSION]),
            ]},
            {"schema_version": 1, "keys": [
                self._entry("reader", self.KEY_A),
            ]},
            {"schema_version": True, "keys": [
                self._entry("reader", self.KEY_A),
            ]},
            {"schema_version": 2, "keys": [
                self._entry("reader", self.KEY_A),
            ], "extra": 1},
        )
        with tempfile.TemporaryDirectory() as directory:
            for index, value in enumerate(invalid_values):
                with self.subTest(index=index):
                    with self.assertRaises(AuthConfigurationError):
                        load_api_key_authenticator(self._write(directory,
                                                               value))

            path = Path(directory) / "duplicate.json"
            path.write_text(
                '{"schema_version":2,"schema_version":2,"keys":[]}',
                encoding="utf-8")
            with self.assertRaisesRegex(AuthConfigurationError, "重复字段"):
                load_api_key_authenticator(path)

            nested = Path(directory) / "nested-duplicate.json"
            nested.write_text(
                '{"schema_version":2,"keys":[{'
                '"key_id":"reader","key_id":"other",'
                f'"api_key":"{self.KEY_A}","permissions":[]'
                '}]}', encoding="utf-8")
            with self.assertRaisesRegex(AuthConfigurationError, "重复字段"):
                load_api_key_authenticator(nested)

            oversized = Path(directory) / "oversized.json"
            oversized.write_bytes(b" " * (MAXIMUM_AUTH_CONFIG_BYTES + 1))
            with self.assertRaisesRegex(AuthConfigurationError, "不得超过"):
                load_api_key_authenticator(oversized)

    def test_hot_rotation_revokes_old_key_and_changes_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._write(directory, {"schema_version": 2, "keys": [
                self._entry("old-reader", self.KEY_A),
            ]})
            authenticator = load_reloading_api_key_authenticator(path)
            self.assertEqual(authenticator.authenticate(self.KEY_A).key_id,
                             "old-reader")

            replacement = Path(directory) / "runtime-auth.next"
            replacement.write_text(json.dumps({
                "schema_version": 2,
                "keys": [self._entry(
                    "new-controller", self.KEY_B,
                    [CONTROL_LEASE_ACQUIRE_PERMISSION])],
            }), encoding="utf-8")
            os.replace(replacement, path)

            self.assertIsNone(authenticator.authenticate(self.KEY_A))
            principal = authenticator.authenticate(self.KEY_B)
            self.assertEqual(principal.key_id, "new-controller")
            self.assertEqual(principal.permissions,
                             frozenset({CONTROL_LEASE_ACQUIRE_PERMISSION}))

    def test_hot_reload_failure_closes_access_until_repaired(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self._write(directory, {"schema_version": 2, "keys": [
                self._entry("reader", self.KEY_A),
            ]})
            authenticator = load_reloading_api_key_authenticator(path)
            path.write_text("{broken", encoding="utf-8")
            self.assertIsNone(authenticator.authenticate(self.KEY_A))
            self.assertEqual(authenticator.key_count, 0)

            path.unlink()
            self.assertIsNone(authenticator.authenticate(self.KEY_A))
            self._write(directory, {"schema_version": 2, "keys": [
                self._entry("repaired", self.KEY_B),
            ]})
            self.assertEqual(authenticator.authenticate(self.KEY_B).key_id,
                             "repaired")
            self.assertIsNone(authenticator.authenticate(self.KEY_A))


if __name__ == "__main__":
    unittest.main()
