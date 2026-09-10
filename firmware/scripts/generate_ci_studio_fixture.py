#!/usr/bin/env python3
"""生成 CI 使用的真实 Studio G431 默认工程输入。"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "gui"))

from project_config import generate_project_config  # noqa: E402
from project_contract import CURRENT_PROJECT_SCHEMA_VERSION  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--firmware-input-sha256", required=True)
    args = parser.parse_args()
    if len(args.firmware_input_sha256) != 64 or any(
            value not in "0123456789abcdef" for value in
            args.firmware_input_sha256):
        parser.error("固件输入身份必须为64位小写十六进制")

    catalog_path = ROOT / "gui" / "data" / "pin_catalog.json"
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    board = next(item for item in catalog["boards"]
                 if item["id"] == "weact-g431-core-v10")
    project = {
        "schema_version": CURRENT_PROJECT_SCHEMA_VERSION,
        "board_id": board["id"],
        "gpio": {"resources": board["gpio_defaults"]},
        "uart": {"ports": board.get("uart_defaults", [])},
        "motion": {"axes": board["motion_defaults"]},
        "pwm": {"channels": [item for item in board["waveform"]["pwm"]
                              if item["enabled"]]},
        "timed_bitstream": {"ws2812": [
            item for item in board["waveform"]["ws2812"]
            if item["enabled"]]},
        "i2c": {"buses": [], "devices": []},
        "spi": {"buses": [], "devices": []},
    }
    generated = generate_project_config(project, catalog)
    args.output.mkdir(parents=True, exist_ok=True)
    config = args.output / "firmware.config"
    resources = args.output / "remotebsp_static_resources.h"
    identity = args.output / "identity.json"
    config.write_text(generated.config, encoding="utf-8")
    resources.write_text(generated.static_resource_header, encoding="utf-8")
    identity.write_text(json.dumps({
        "board_id": generated.board_id,
        "firmware_target": generated.firmware_target,
        "project_sha256": generated.project_sha256,
        "config_sha256": hashlib.sha256(config.read_bytes()).hexdigest(),
        "static_resource_sha256": hashlib.sha256(
            resources.read_bytes()).hexdigest(),
        "firmware_input_sha256": args.firmware_input_sha256,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
