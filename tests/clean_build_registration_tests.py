#!/usr/bin/env python3
"""在全新 CMake 缓存中验证解释器和关键发布门禁确实注册。"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


REQUIRED_TESTS = {
    "gui_tests",
    "runtime_api_tests",
    "firmware_config_tests",
    "firmware_flash_layout_tests",
    "toolbusd_operation_ledger_process_tests",
    "bus_reset_runtime_http_process_tests",
    "deployment_evidence_offline_tests",
}


def main() -> int:
    cmake, ctest, source = sys.argv[1:4]
    with tempfile.TemporaryDirectory(prefix="remotebsp-clean-config-") as path:
        build = Path(path) / "build"
        subprocess.run([cmake, "-S", source, "-B", str(build),
                        "-DBUILD_TESTING=ON"], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, timeout=45)
        result = subprocess.run([ctest, "--test-dir", str(build),
                                 "--show-only=json-v1"], check=True,
                                capture_output=True, text=True, timeout=15)
        document = json.loads(result.stdout)
        tests = {item["name"]: item for item in document["tests"]}
        missing = REQUIRED_TESTS - tests.keys()
        if missing:
            raise RuntimeError("干净配置缺少关键测试：" + ", ".join(sorted(missing)))
        python = str(Path(sys.executable).resolve())
        for name in ("gui_tests", "runtime_api_tests",
                     "bus_reset_runtime_http_process_tests"):
            command = tests[name]["command"]
            if not command or str(Path(command[0]).resolve()) != python:
                raise RuntimeError(f"{name} 未绑定本次明确的Python解释器")
        properties = {item["name"]: item["value"] for item in
                      tests["bus_reset_runtime_http_process_tests"]["properties"]}
        if properties.get("RUN_SERIAL") is not True:
            raise RuntimeError("BusReset真实进程测试必须串行运行")
    print("干净构建测试注册门禁通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
