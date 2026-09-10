#!/usr/bin/env python3
"""RemoteBSP 本地配置与数字孪生 GUI 服务。仅使用 Python 标准库。"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import mimetypes
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qsl, quote, unquote, urlparse

from firmware_builder import (
    DEFAULT_OUTPUT_ROOT,
    FirmwareBuildError,
    build_firmware_project,
    resolve_artifact,
)
from project_config import (
    ProjectConfigError,
    generate_mock_board_manifest,
    generate_project_config,
    validate_project,
)
from project_contract import CURRENT_PROJECT_SCHEMA_VERSION
from project_artifacts import generate_project_reports
from project_compare import compare_projects
from production_record import (
    MAX_BUILD_RECORD_BYTES,
    generate_production_record,
    production_record_response,
)
from comparison_export import (
    comparison_export_response,
    export_project_comparison,
)
from production_batch import (
    export_production_batch,
    production_batch_response,
    validate_production_batch_manifest,
)
from production_history import (
    DEFAULT_HISTORY_ROOT,
    MAX_HISTORY_RESULTS,
    ProductionHistoryStore,
)
from device_parameters import DeviceParameterError, DeviceParameterManager
from firmware_deployment import FirmwareDeploymentError, ToolbusdIdentityReader
from web_deployment import WebDeploymentController


GUI_ROOT = Path(__file__).resolve().parent
CATALOG_PATH = GUI_ROOT / "data" / "pin_catalog.json"
def demo_state() -> dict:
    return {
        "schema_version": 1,
        "board_name": "Mock Generic Board",
        "online": True,
        "elapsed_ms": 0,
        "source": "demo",
        "gpio": [
            {"pin": pin, "direction": "output" if pin < 8 else "input",
             "value": pin in {1, 4, 7, 10}}
            for pin in range(16)
        ],
        "motion": {
            "state": "idle", "fault": "none", "queue_depth": 0,
            "queue_capacity": 32,
            "axes": [
                {"resource_id": 200 + axis, "enabled": False,
                 "direction_positive": True, "step_level": False,
                 "steps_per_revolution": 200,
                 "position_steps": axis * 160, "emitted_steps": axis * 160}
                for axis in range(3)
            ],
        },
        "pwm": [
            {"resource_id": 0x06000000, "channel": 0,
             "frequency_hz": 20000, "duty": 4200,
             "active_low": False, "running": True},
            {"resource_id": 0x06000001, "channel": 1,
             "frequency_hz": 1000, "duty": 7500,
             "active_low": False, "running": False},
        ],
        "ws2812": {
            "supported": True,
            "strips": [{"resource_id": 300, "pixels": [
                "#ff6b35", "#ffb627", "#43d9bd", "#46a7ff",
                "#9b7bff", "#ff5ca8", "#111827", "#111827",
            ]}],
        },
    }


class GuiRequestHandler(SimpleHTTPRequestHandler):
    server_version = "RemoteBSP-GUI/0.1"

    def __init__(self, *args, directory=None, **kwargs):
        super().__init__(*args, directory=str(GUI_ROOT / "static"), **kwargs)

    @property
    def state_path(self) -> Path | None:
        return getattr(self.server, "state_path", None)

    @property
    def build_jobs(self) -> int:
        return getattr(self.server, "build_jobs", 32)

    @property
    def build_output_root(self) -> Path:
        return getattr(self.server, "build_output_root", DEFAULT_OUTPUT_ROOT)

    @property
    def production_history(self) -> ProductionHistoryStore:
        return getattr(self.server, "production_history")

    @property
    def deployment_controller(self) -> WebDeploymentController | None:
        return getattr(self.server, "deployment_controller")

    @property
    def device_parameter_manager(self) -> DeviceParameterManager | None:
        return getattr(self.server, "device_parameter_manager", None)

    def _send_json(self, value: object, status: HTTPStatus = HTTPStatus.OK) -> None:
        encoded = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def _send_artifact(self, path: Path) -> None:
        content_type = mimetypes.guess_type(path.name)[0] or \
            "application/octet-stream"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(path.stat().st_size))
        self.send_header(
            "Content-Disposition", f'attachment; filename="{path.name}"')
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        with path.open("rb") as stream:
            while chunk := stream.read(65536):
                self.wfile.write(chunk)

    def do_GET(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path == "/api/health":
            self._send_json({"ok": True})
            return
        if path == "/api/catalog":
            try:
                self._send_json(json.loads(CATALOG_PATH.read_text(encoding="utf-8")))
            except (OSError, json.JSONDecodeError) as error:
                self._send_json({"error": f"读取引脚目录失败：{error}"}, HTTPStatus.INTERNAL_SERVER_ERROR)
            return
        if path == "/api/state":
            state = demo_state()
            if self.state_path and self.state_path.exists():
                try:
                    state = json.loads(self.state_path.read_text(encoding="utf-8"))
                    state["source"] = "mock_mcu"
                except (OSError, json.JSONDecodeError) as error:
                    state["source"] = "demo"
                    state["warning"] = f"Mock 状态暂不可读：{error}"
            self._send_json(state)
            return
        if path == "/api/project/target":
            self._send_json({
                "api_version": 1,
                "mode": "static-firmware",
                "enabled": True,
                "build_enabled": True,
                "project_reports_enabled": True,
                "project_compare_enabled": True,
                "project_comparison_export_enabled": True,
                "production_record_enabled": True,
                "production_batch_enabled": True,
                "production_history_enabled": True,
                "stlink_deployment_enabled": self.deployment_controller is not None,
                "device_parameters_enabled":
                    self.device_parameter_manager is not None,
                "parallel_jobs": self.build_jobs,
                "project_schema_version": CURRENT_PROJECT_SCHEMA_VERSION,
                "runtime_control_enabled": False,
            })
            return
        if path == "/api/production-history/status":
            self._send_json(self.production_history.status())
            return
        if path == "/api/production-history/search":
            try:
                pairs = parse_qsl(urlparse(self.path).query,
                                  keep_blank_values=True, max_num_fields=8)
                allowed = {"query", "field", "limit"}
                names = [name for name, _ in pairs]
                unknown = sorted(set(names) - allowed)
                repeated = sorted(name for name in allowed
                                  if names.count(name) > 1)
                if unknown:
                    raise ProjectConfigError(
                        f"历史搜索包含未知参数：{','.join(unknown)}")
                if repeated:
                    raise ProjectConfigError(
                        f"历史搜索参数不能重复：{','.join(repeated)}")
                query = dict(pairs)
                limit = int(query.get("limit", str(MAX_HISTORY_RESULTS)))
                response = self.production_history.search(
                    query.get("query", ""), query.get("field", "all"), limit)
                self._send_json(response)
            except (TypeError, ValueError, ProjectConfigError) as error:
                self._send_json({"ok": False, "error": str(error)},
                                HTTPStatus.BAD_REQUEST)
            return
        if path.startswith("/api/production-history/record/"):
            parts = path.strip("/").split("/")
            if len(parts) != 4:
                self._send_json({"ok": False, "error": "历史记录地址无效"},
                                HTTPStatus.NOT_FOUND)
                return
            try:
                stored = self.production_history.get(unquote(parts[3]))
                self._send_json({
                    "ok": True, "format": "PRODUCTION_HISTORY_RECORD_V1",
                    "manifest_sha256": stored.manifest["manifest_sha256"],
                    "byte_count": stored.byte_count,
                    "filename": (stored.manifest["batch"]["batch_id"] +
                                 "-生产批次清单-v1.json"),
                    "manifest": stored.manifest,
                    "validation": validate_production_batch_manifest(
                        stored.manifest),
                })
            except (OSError, ProjectConfigError) as error:
                self._send_json({"ok": False, "error": str(error)},
                                HTTPStatus.NOT_FOUND)
            return
        if path.startswith("/api/project/artifacts/"):
            parts = path.strip("/").split("/")
            if len(parts) != 5:
                self._send_json({"error": "产物地址无效"}, HTTPStatus.NOT_FOUND)
                return
            try:
                artifact = resolve_artifact(
                    unquote(parts[3]), unquote(parts[4]),
                    self.build_output_root)
                self._send_artifact(artifact)
            except (OSError, FirmwareBuildError) as error:
                self._send_json({"error": str(error)}, HTTPStatus.NOT_FOUND)
            return
        if path == "/":
            self.path = "/index.html"
        super().do_GET()

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path not in ("/api/project/inspect", "/api/project/validate",
                        "/api/project/generate",
                        "/api/project/generate-reports",
                        "/api/project/generate-mock-manifest",
                        "/api/project/generate-production-record",
                        "/api/project/compare",
                        "/api/project/export-comparison",
                        "/api/production-batch/export",
                        "/api/production-batch/validate",
                        "/api/production-history/save",
                        "/api/device-parameters/read",
                        "/api/device-parameters/backup",
                        "/api/deployment/preflight",
                        "/api/deployment/execute",
                        "/api/project/build"):
            self._send_json({"error": "未知API"}, HTTPStatus.NOT_FOUND)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 262144:
                raise ProjectConfigError("请求长度无效或超过256 KiB")
            request = json.loads(self.rfile.read(length).decode("utf-8"))
            catalog = None if path.startswith((
                "/api/production-batch/", "/api/production-history/",
                "/api/device-parameters/", "/api/deployment/")) else \
                json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
            project = request.get("project")
            if path.startswith("/api/deployment/"):
                controller = self.deployment_controller
                if controller is None:
                    self._send_json(
                        {"ok": False, "error": "Studio未配置受控ST-Link部署入口"},
                        HTTPStatus.SERVICE_UNAVAILABLE)
                    return
                if path.endswith("/preflight"):
                    if set(request) != {"build_id", "expected_uuid"}:
                        raise FirmwareDeploymentError("Web部署预检字段集合无效")
                    response = controller.preflight(
                        build_id=request.get("build_id"),
                        expected_uuid=request.get("expected_uuid"))
                else:
                    if set(request) != {"confirmation_token", "confirmation",
                                       "flash_timeout", "reconnect_timeout",
                                       "poll_interval"}:
                        raise FirmwareDeploymentError("Web部署执行字段集合无效")
                    response = controller.execute(**request)
            elif path.startswith("/api/device-parameters/"):
                manager = self.device_parameter_manager
                if manager is None:
                    self._send_json(
                        {"ok": False, "error": "Studio未配置toolbusd设备参数接口"},
                        HTTPStatus.SERVICE_UNAVAILABLE)
                    return
                if path in ("/api/device-parameters/read",
                            "/api/device-parameters/backup"):
                    if set(request):
                        raise DeviceParameterError("只读请求不接受额外字段")
                    backup = manager.backup() if path.endswith("/backup") else None
                    snapshot = ({key: value for key, value in backup.items()
                                 if key not in ("format", "sha256")}
                                if backup is not None else manager.snapshot())
                    if backup is not None:
                        snapshot["schema_version"] = 1
                    response = {
                        "ok": True,
                        "format": ("DEVICE_PARAMETER_BACKUP_V2" if
                                   path.endswith("/backup") else
                                   "DEVICE_PARAMETER_SNAPSHOT_V1"),
                        "backup": backup,
                        "snapshot": snapshot,
                    }
                else:
                    raise AssertionError("未处理的设备参数只读路径")
            elif path == "/api/production-history/save":
                saved = self.production_history.save(request.get("manifest"))
                response = {
                    "ok": True, "format": "PRODUCTION_HISTORY_SAVE_V1",
                    **saved,
                    "declaration": (
                        "只保存已校验的软件批次清单；未执行构建、烧录或硬件访问。"),
                }
            elif path == "/api/production-batch/validate":
                response = validate_production_batch_manifest(
                    request.get("manifest"))
            elif path == "/api/production-batch/export":
                response = production_batch_response(export_production_batch(
                    batch_id=request.get("batch_id"),
                    name=request.get("name"), note=request.get("note", ""),
                    production_records=request.get("production_records"),
                    comparison_exports=request.get("comparison_exports", [])))
            elif path == "/api/project/export-comparison":
                response = comparison_export_response(
                    export_project_comparison(
                        request.get("left"), request.get("right"), catalog))
            elif path == "/api/project/compare":
                response = compare_projects(
                    request.get("left"), request.get("right"), catalog)
            elif path == "/api/project/generate-production-record":
                build_record = None
                build_record_sha256 = None
                build_id = request.get("build_id")
                if build_id is not None:
                    if not isinstance(build_id, str) or not build_id:
                        raise ProjectConfigError("构建ID必须是非空字符串")
                    record_path = resolve_artifact(
                        build_id, "build-record.json", self.build_output_root)
                    if record_path.stat().st_size > MAX_BUILD_RECORD_BYTES:
                        raise ProjectConfigError(
                            "构建记录源文件超过128 KiB上限")
                    record_bytes = record_path.read_bytes()
                    build_record_sha256 = hashlib.sha256(
                        record_bytes).hexdigest()
                    build_record = json.loads(record_bytes.decode("utf-8"))
                response = production_record_response(
                    generate_production_record(
                        project, catalog, build_record=build_record,
                        build_record_sha256=build_record_sha256,
                        deployment_record=request.get("deployment_record")))
            elif path == "/api/project/generate-reports":
                generated = generate_project_reports(project, catalog)
                response = {
                    "ok": True,
                    "format": "PROJECT_REPORTS_V1",
                    "board_id": generated.board_id,
                    "resource_count": generated.resource_count,
                    "project_schema_version":
                        generated.project_schema_version,
                    "project_original_schema_version":
                        generated.original_schema_version,
                    "project_migrations": list(generated.migrations),
                    "project_sha256": generated.project_sha256,
                    "resource_set_sha256": generated.resource_set_sha256,
                    "archive_filename": generated.archive_filename,
                    "archive_sha256": generated.archive_sha256,
                    "archive_byte_count": len(generated.archive),
                    "archive_base64": base64.b64encode(
                        generated.archive).decode("ascii"),
                    "artifacts": [{
                        "filename": artifact.filename,
                        "content_type": artifact.content_type,
                        "byte_count": len(artifact.content),
                        "sha256": artifact.sha256,
                    } for artifact in generated.artifacts],
                }
            elif path == "/api/project/validate":
                validated = validate_project(project, catalog)
                response = {
                    "ok": True,
                    "format": "PROJECT_VALIDATION",
                    "board_id": validated.board_id,
                    "resource_count": validated.resource_count,
                    "project_schema_version":
                        validated.project_schema_version,
                    "project_original_schema_version":
                        validated.original_schema_version,
                    "project_migrations": list(validated.migrations),
                    "project_sha256": validated.project_sha256,
                    "summary": validated.summary,
                }
            elif path == "/api/project/generate-mock-manifest":
                generated = generate_mock_board_manifest(project, catalog)
                manifest_bytes = (json.dumps(
                    generated.manifest, ensure_ascii=False, indent=2) +
                    "\n").encode("utf-8")
                response = {
                    "ok": True,
                    "format": "MOCK_BOARD_MANIFEST_V2",
                    "board_id": generated.board_id,
                    "resource_count": generated.resource_count,
                    "project_schema_version":
                        generated.project_schema_version,
                    "project_original_schema_version":
                        generated.original_schema_version,
                    "project_migrations": list(generated.migrations),
                    "project_sha256": generated.project_sha256,
                    "summary": generated.summary,
                    "manifest_sha256":
                        hashlib.sha256(manifest_bytes).hexdigest(),
                    "byte_count": len(manifest_bytes),
                    "filename": f"{generated.board_id}-mock-v2.json",
                    "manifest": generated.manifest,
                    "manifest_base64": base64.b64encode(
                        manifest_bytes).decode("ascii"),
                }
            elif path in ("/api/project/inspect", "/api/project/generate"):
                result = generate_project_config(project, catalog)
                response = {
                    "ok": True,
                    "format": "PROJECT_INSPECTION" if
                              path == "/api/project/inspect" else "KCONFIG",
                    "board_id": result.board_id,
                    "firmware_target": result.firmware_target,
                    "resource_count": result.resource_count,
                    "project_schema_version":
                        result.project_schema_version,
                    "project_original_schema_version":
                        result.original_schema_version,
                    "project_migrations": list(result.migrations),
                    "project_sha256": result.project_sha256,
                    "summary": result.summary,
                }
                if path == "/api/project/generate":
                    config_bytes = result.config.encode("utf-8")
                    response.update({
                        "config_sha256":
                            hashlib.sha256(config_bytes).hexdigest(),
                        "byte_count": len(config_bytes),
                        "filename": f"{result.board_id}.config",
                        "config_base64": base64.b64encode(
                            config_bytes).decode("ascii"),
                    })
            else:
                result = build_firmware_project(
                    project, catalog, jobs=self.build_jobs,
                    output_root=self.build_output_root)
                response = {
                    "ok": True,
                    "format": "FIRMWARE_BUILD",
                    "build_id": result.build_id,
                    "board_id": result.board_id,
                    "firmware_target": result.firmware_target,
                    "config_sha256": result.config_sha256,
                    "project_sha256":
                        result.record.get("project_sha256"),
                    "project_schema_version": result.record.get(
                        "project_schema_version",
                        CURRENT_PROJECT_SCHEMA_VERSION),
                    "project_migrations": result.record.get(
                        "project_migrations", []),
                    "summary": result.record.get("project_summary", {}),
                    "memory": result.record.get("memory", {}),
                    "artifacts": [{
                        "filename": artifact.filename,
                        "size": artifact.size,
                        "sha256": artifact.sha256,
                        "url": "/api/project/artifacts/" +
                               quote(result.build_id) + "/" +
                               quote(artifact.filename),
                    } for artifact in result.artifacts],
                }
            self._send_json(response)
        except FirmwareBuildError as error:
            self._send_json({"ok": False, "error": str(error)},
                            HTTPStatus.UNPROCESSABLE_ENTITY)
        except (OSError, AttributeError, json.JSONDecodeError,
                UnicodeDecodeError, TypeError, ValueError,
                ProjectConfigError, DeviceParameterError,
                FirmwareDeploymentError) as error:
            self._send_json({"ok": False, "error": str(error)},
                            HTTPStatus.BAD_REQUEST)

    def log_message(self, format: str, *args: object) -> None:
        # 保留错误日志，避免状态轮询刷满终端。
        if args and str(args[1]).startswith(("4", "5")):
            super().log_message(format, *args)


def make_server(host: str, port: int,
                state_path: Path | None, *, build_jobs: int = 32,
                build_output_root: Path = DEFAULT_OUTPUT_ROOT,
                history_root: Path = DEFAULT_HISTORY_ROOT,
                device_parameter_manager: DeviceParameterManager | None = None,
                deployment_controller: WebDeploymentController | None = None,
                ) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((host, port), GuiRequestHandler)
    server.state_path = state_path  # type: ignore[attr-defined]
    server.build_jobs = build_jobs  # type: ignore[attr-defined]
    server.build_output_root = build_output_root  # type: ignore[attr-defined]
    server.production_history = ProductionHistoryStore(  # type: ignore[attr-defined]
        history_root)
    server.device_parameter_manager = device_parameter_manager  # type: ignore[attr-defined]
    server.deployment_controller = deployment_controller  # type: ignore[attr-defined]
    return server


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="监听地址，默认仅本机")
    parser.add_argument("--port", type=int, default=8765, help="监听端口")
    parser.add_argument("--state", type=Path, help="mock_mcu --visual-state 输出的 JSON 文件")
    parser.add_argument("--build-jobs", type=int, default=32,
                        help="固件构建并行任务数，默认32")
    parser.add_argument("--history-root", type=Path,
                        default=DEFAULT_HISTORY_ROOT,
                        help="本地生产批次历史目录")
    parser.add_argument("--toolbusd-socket",
                        help="启用设备参数管理时使用的toolbusd本地socket")
    parser.add_argument("--node-id", type=int, default=1,
                        help="设备参数管理的单节点ID，默认1")
    parser.add_argument("--remote-cli", default="remote-cli",
                        help="remote-cli可执行文件")
    parser.add_argument("--enable-stlink-deployment", action="store_true",
                        help="显式启用本地Web两阶段ST-Link部署入口")
    parser.add_argument("--deployment-record-root", type=Path,
                        default=GUI_ROOT / "deployment-records",
                        help="已核验部署记录目录")
    args = parser.parse_args()
    if args.build_jobs < 1 or args.build_jobs > 64:
        parser.error("--build-jobs必须位于1～64")
    mimetypes.add_type("text/javascript", ".js")
    parameter_manager = None
    if args.toolbusd_socket:
        try:
            parameter_manager = DeviceParameterManager(
                args.toolbusd_socket, args.node_id,
                remote_cli=args.remote_cli)
        except DeviceParameterError as error:
            parser.error(str(error))
    deployment_controller = None
    if args.enable_stlink_deployment:
        if not args.toolbusd_socket:
            parser.error("启用Web部署必须同时配置--toolbusd-socket")
        try:
            deployment_controller = WebDeploymentController(
                output_root=DEFAULT_OUTPUT_ROOT,
                record_root=args.deployment_record_root,
                reader_factory=lambda expected_uuid: ToolbusdIdentityReader(
                    args.toolbusd_socket, args.node_id,
                    expected_uuid=expected_uuid,
                    remote_cli=args.remote_cli))
        except FirmwareDeploymentError as error:
            parser.error(str(error))
    server = make_server(
        args.host, args.port, args.state, build_jobs=args.build_jobs,
        history_root=args.history_root,
        device_parameter_manager=parameter_manager,
        deployment_controller=deployment_controller)
    print(f"RemoteBSP GUI 已启动：http://{args.host}:{args.port}")
    print("未连接 Mock 状态文件时会自动显示演示数据；按 Ctrl+C 退出。")
    print(f"固件构建使用{args.build_jobs}个并行任务。")
    print(server.production_history.status()["status_text"])  # type: ignore[attr-defined]
    if parameter_manager is not None:
        print("设备参数管理已启用；写入需要UUID、代数及显式维护确认。")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
