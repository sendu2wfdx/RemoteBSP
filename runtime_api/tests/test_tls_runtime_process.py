import concurrent.futures
import hashlib
import json
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from urllib.request import urlopen

from runtime_api.tls_deployment import create_artifact


class TlsRuntimeProcessTest(unittest.TestCase):
    @staticmethod
    def _make_certificate(openssl: str, root: Path, name: str) -> tuple[Path, Path]:
        certificate = root / f"{name}.crt"
        private_key = root / f"{name}.key"
        subprocess.run([
            openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-days", "1", "-subj", f"/CN={name}",
            "-keyout", str(private_key), "-out", str(certificate)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=15, check=True)
        certificate.chmod(0o644)
        private_key.chmod(0o600)
        return certificate, private_key

    @staticmethod
    def _peer_digest(context: ssl.SSLContext, port: int) -> str:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as raw:
            with context.wrap_socket(raw, server_hostname="127.0.0.1") as tls:
                return hashlib.sha256(tls.getpeercert(binary_form=True)).hexdigest()

    def test_https_handshake_plaintext_rejection_and_drift_fail_closed(self):
        openssl = shutil.which("openssl")
        if openssl is None:
            self.skipTest("测试环境没有 openssl，无法生成仅供测试的临时证书")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            certificate = root / "test-only.crt"
            private_key = root / "test-only.key"
            subprocess.run([
                openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                "-days", "1", "-subj", "/CN=127.0.0.1",
                "-keyout", str(private_key), "-out", str(certificate)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=15, check=True)
            certificate.chmod(0o644)
            private_key.chmod(0o600)
            config = root / "tls.json"
            create_artifact(
                config, runtime_bind="127.0.0.1", proxy_bind="127.0.0.1",
                certificate_file=certificate.resolve(),
                private_key_file=private_key.resolve())

            probe = socket.socket()
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
            probe.close()
            process = subprocess.Popen([
                sys.executable, "-m", "runtime_api.server", "--host",
                "127.0.0.1", "--port", str(port),
                "--tls-baseline-config", str(config)],
                cwd=Path(__file__).resolve().parents[2],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            context = ssl.create_default_context()
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            try:
                deadline = time.monotonic() + 8
                while True:
                    try:
                        with urlopen(f"https://127.0.0.1:{port}/api/v1/",
                                     context=context, timeout=1) as response:
                            self.assertEqual(response.status, 200)
                        break
                    except OSError:
                        if process.poll() is not None or time.monotonic() >= deadline:
                            _, stderr = process.communicate(timeout=1)
                            self.fail(f"HTTPS Runtime 未启动：{stderr}")
                        time.sleep(0.05)

                with socket.create_connection(("127.0.0.1", port), timeout=2) as plain:
                    plain.sendall(b"GET /api/v1/ HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
                    try:
                        received = plain.recv(128)
                    except (ConnectionResetError, TimeoutError):
                        received = b""
                    self.assertNotIn(b"HTTP/1.1 200", received)
                    self.assertNotIn(b"HTTP/1.0 200", received)
            finally:
                process.terminate()
                process.communicate(timeout=5)

            # 工件漂移后，新的真实进程必须在建立监听前退出。
            config.write_bytes(config.read_bytes() + b" ")
            failed = subprocess.run([
                sys.executable, "-m", "runtime_api.server", "--host",
                "127.0.0.1", "--port", "0",
                "--tls-baseline-config", str(config)],
                cwd=Path(__file__).resolve().parents[2],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                timeout=8, check=False)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("TLS基线无效", failed.stderr)

    def test_sighup_rotation_switches_new_connections_and_rolls_back_failure(self):
        openssl = shutil.which("openssl")
        if openssl is None or not hasattr(signal, "SIGHUP"):
            self.skipTest("测试环境不支持 openssl 或 SIGHUP")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            certificate1, private_key1 = self._make_certificate(
                openssl, root, "rotation-one")
            certificate2, private_key2 = self._make_certificate(
                openssl, root, "rotation-two")
            config = root / "tls.json"
            create_artifact(
                config, runtime_bind="127.0.0.1", proxy_bind="127.0.0.1",
                certificate_file=certificate1.resolve(),
                private_key_file=private_key1.resolve())
            probe = socket.socket()
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
            probe.close()
            process = subprocess.Popen([
                sys.executable, "-m", "runtime_api.server", "--host",
                "127.0.0.1", "--port", str(port),
                "--tls-baseline-config", str(config)],
                cwd=Path(__file__).resolve().parents[2],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            context = ssl.create_default_context()
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            old_tls = None
            stderr = ""
            try:
                deadline = time.monotonic() + 8
                while True:
                    try:
                        raw = socket.create_connection(("127.0.0.1", port), timeout=1)
                        old_tls = context.wrap_socket(raw, server_hostname="127.0.0.1")
                        break
                    except OSError:
                        if process.poll() is not None or time.monotonic() >= deadline:
                            self.fail("TLS轮换测试进程未启动")
                        time.sleep(0.05)
                old_digest = hashlib.sha256(
                    old_tls.getpeercert(binary_form=True)).hexdigest()

                create_artifact(
                    config, runtime_bind="127.0.0.1", proxy_bind="127.0.0.1",
                    certificate_file=certificate2.resolve(),
                    private_key_file=private_key2.resolve())
                gate = __import__("threading").Event()

                def concurrent_handshake() -> str:
                    gate.wait()
                    return self._peer_digest(context, port)

                with concurrent.futures.ThreadPoolExecutor(
                        max_workers=12) as executor:
                    handshakes = [executor.submit(concurrent_handshake)
                                  for _ in range(24)]
                    gate.set()
                    process.send_signal(signal.SIGHUP)
                    observed_during_reload = [future.result(timeout=5)
                                              for future in handshakes]
                deadline = time.monotonic() + 8
                new_digest = old_digest
                while time.monotonic() < deadline:
                    new_digest = self._peer_digest(context, port)
                    if new_digest != old_digest:
                        break
                    time.sleep(0.05)
                self.assertNotEqual(new_digest, old_digest)
                self.assertTrue(set(observed_during_reload).issubset({
                    old_digest, new_digest}))

                # 已完成握手的连接仍绑定旧上下文，轮换只影响随后接受的连接。
                old_tls.sendall(
                    b"GET /api/v1/ HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
                self.assertIn(b"200 OK", old_tls.recv(4096))
                old_tls.close()
                old_tls = None

                # 损坏工件后再次请求轮换，必须保留刚才成功装载的上下文。
                config.write_bytes(config.read_bytes() + b" ")
                process.send_signal(signal.SIGHUP)
                deadline = time.monotonic() + 8
                status = None
                while time.monotonic() < deadline:
                    with urlopen(f"https://127.0.0.1:{port}/api/v1/",
                                 context=context, timeout=1) as response:
                        status = json.load(response)["data"]["capabilities"]["tls"]
                    if status["last_result"] == \
                            "reload_failed_old_context_retained":
                        break
                    time.sleep(0.05)
                self.assertEqual(self._peer_digest(context, port), new_digest)
                self.assertEqual(status["generation"], 2)
                self.assertTrue(status["last_error"])
            finally:
                if old_tls is not None:
                    old_tls.close()
                process.terminate()
                _, stderr = process.communicate(timeout=5)
            self.assertIn('"result": "success"', stderr)
            self.assertIn('"old_context_retained": true', stderr)


if __name__ == "__main__":
    unittest.main()
