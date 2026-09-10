import shutil
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


if __name__ == "__main__":
    unittest.main()
