#!/usr/bin/env python3
"""RemoteBSP 本地配置与数字孪生 GUI 服务。仅使用 Python 标准库。"""

from __future__ import annotations

import argparse
import base64
import hashlib
import ipaddress
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
from web_deployment import (
    WebCanKatapultDeploymentController, WebDeploymentController,
    WebUsbKatapultDeploymentController)
from studio_deployment_workflow import StudioDeploymentWorkflow
from parameter_audit import ParameterAuditStore
from web_device_parameters import WebDeviceParameterController
from runtime_pwm_proxy import RuntimePwmProxy, RuntimePwmProxyError


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
        return getattr(self.server, "deployment_controller", None)

    @property
    def can_katapult_deployment_controller(self) -> \
            WebCanKatapultDeploymentController | None:
        return getattr(self.server, "can_katapult_deployment_controller", None)

    @property
    def usb_katapult_deployment_controller(self) -> \
            WebUsbKatapultDeploymentController | None:
        return getattr(self.server, "usb_katapult_deployment_controller", None)

    @property
    def studio_deployment_workflow(self) -> StudioDeploymentWorkflow | None:
        return getattr(self.server, "studio_deployment_workflow", None)

    @property
    def parameter_write_controller(self) -> WebDeviceParameterController | None:
        return getattr(self.server, "parameter_write_controller")

    @property
    def device_parameter_manager(self) -> DeviceParameterManager | None:
        return getattr(self.server, "device_parameter_manager", None)

    @property
    def runtime_pwm_proxy(self) -> RuntimePwmProxy | None:
        return getattr(self.server, "runtime_pwm_proxy", None)

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
        parsed = urlparse(self.path)
        path = parsed.path
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
            pwm_proxy = self.runtime_pwm_proxy
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
                "can_katapult_deployment_enabled":
                    self.can_katapult_deployment_controller is not None,
                "can_katapult_deployment": {
                    "available": self.can_katapult_deployment_controller is not None,
                    "can_interface": (self.can_katapult_deployment_controller.can_interface
                                      if self.can_katapult_deployment_controller is not None
                                      else None),
                },
                "usb_katapult_deployment_enabled":
                    self.usb_katapult_deployment_controller is not None,
                "usb_katapult_deployment": {
                    "available": self.usb_katapult_deployment_controller is not None,
                    "usb_device": (self.usb_katapult_deployment_controller.usb_device
                                   if self.usb_katapult_deployment_controller is not None
                                   else None),
                },
                "device_parameter_write_enabled":
                    self.parameter_write_controller is not None,
                "device_parameters_enabled":
                    self.device_parameter_manager is not None,
                "parallel_jobs": self.build_jobs,
                "project_schema_version": CURRENT_PROJECT_SCHEMA_VERSION,
                "runtime_control_enabled": False,
                "runtime_pwm": {
                    "available": pwm_proxy is not None,
                    "auth_proxy": pwm_proxy is not None,
                    "reason": (None if pwm_proxy is not None else
                               "runtime_pwm_auth_proxy_unconfigured"),
                    "configure_path": ("/api/runtime/pwm/configure" if
                                       pwm_proxy is not None else None),
                    "stop_path": ("/api/runtime/pwm/stop" if
                                  pwm_proxy is not None else None),
                    "lease_acquire_path": ("/api/runtime/pwm/lease/acquire" if
                                           pwm_proxy is not None else None),
                    "lease_release_path": ("/api/runtime/pwm/lease/release" if
                                           pwm_proxy is not None else None),
                    "snapshot_path": ("/api/runtime/pwm/snapshot" if
                                      pwm_proxy is not None else None),
                    "contract": {
                        "configure": "POST /api/v1/control/pwm/configure",
                        "stop": "POST /api/v1/control/pwm/stop",
                        "snapshot": "GET /api/v1/snapshot",
                        "result": "data.operation.result",
                    },
                },
                "runtime_timed_bitstream": {
                    "available": pwm_proxy is not None,
                    "auth_proxy": pwm_proxy is not None,
                    "maximum_pixels": 256,
                    "can_encode_ws2812": True,
                    "configure_path": ("/api/runtime/timed-bitstream/configure"
                                       if pwm_proxy is not None else None),
                    "frame_path": ("/api/runtime/timed-bitstream/frame"
                                   if pwm_proxy is not None else None),
                    "stop_path": ("/api/runtime/timed-bitstream/stop"
                                  if pwm_proxy is not None else None),
                    "snapshot_path": ("/api/runtime/timed-bitstream/snapshot"
                                      if pwm_proxy is not None else None),
                    "lease_acquire_path": (
                        "/api/runtime/timed-bitstream/lease/acquire"
                        if pwm_proxy is not None else None),
                    "lease_release_path": (
                        "/api/runtime/timed-bitstream/lease/release"
                        if pwm_proxy is not None else None),
                },
            })
            return
        if path in {"/api/runtime/pwm/snapshot",
                    "/api/runtime/timed-bitstream/snapshot"}:
            proxy = self.runtime_pwm_proxy
            if proxy is None:
                self._send_json({"ok": False, "error": "Runtime PWM代理未启用"},
                                HTTPStatus.SERVICE_UNAVAILABLE)
                return
            try:
                if parsed.query:
                    raise RuntimePwmProxyError("PWM快照端点不接受查询参数", 400)
                response = proxy.snapshot()
                self._send_json(response.document, HTTPStatus(response.status))
            except RuntimePwmProxyError as error:
                self._send_json({"ok": False, "error": str(error)},
                                HTTPStatus(error.status))
            return
        if path == "/api/production-history/status":
            self._send_json(self.production_history.status())
            return
        if path == "/api/deployment/history":
            workflow = self.studio_deployment_workflow
            if workflow is None:
                self._send_json({"ok": False, "error": "Studio部署历史未启用"},
                                HTTPStatus.SERVICE_UNAVAILABLE)
                return
            try:
                pairs = parse_qsl(parsed.query, keep_blank_values=True,
                                  max_num_fields=2)
                names = [name for name, _ in pairs]
                if set(names) - {"build_id", "backend"} or \
                        len(names) != len(set(names)):
                    raise FirmwareDeploymentError("部署历史筛选参数无效")
                query = dict(pairs)
                self._send_json(workflow.history(
                    build_id=query.get("build_id", ""),
                    backend=query.get("backend", "")))
            except FirmwareDeploymentError as error:
                self._send_json({"ok": False, "error": str(error)},
                                HTTPStatus.BAD_REQUEST)
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
        parsed = urlparse(self.path)
        path = parsed.path
        lease_groups = {
            "/api/runtime/pwm/lease/acquire": "pwm.write",
            "/api/runtime/timed-bitstream/lease/acquire":
                "timed-bitstream.write",
        }
        lease_paths = set(lease_groups) | {
            "/api/runtime/pwm/lease/release",
            "/api/runtime/timed-bitstream/lease/release",
        }
        if path in lease_paths:
            proxy = self.runtime_pwm_proxy
            if proxy is None:
                self._send_json({"ok": False, "error": "Runtime控制租约代理未启用"},
                                HTTPStatus.SERVICE_UNAVAILABLE)
                return
            try:
                if parsed.query:
                    raise RuntimePwmProxyError("控制租约端点不接受查询参数", 400)
                if self.headers.get_content_type() != "application/json":
                    raise RuntimePwmProxyError("控制租约请求必须使用application/json", 415)
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > proxy.MAXIMUM_BODY_BYTES:
                    raise RuntimePwmProxyError("控制租约请求长度无效或超过4096字节", 400)
                body = self.rfile.read(length)
                response = (proxy.acquire_lease(lease_groups[path], body)
                            if path in lease_groups else proxy.release_lease(body))
                self._send_json(response.document, HTTPStatus(response.status))
            except (ValueError, RuntimePwmProxyError) as error:
                status = error.status if isinstance(error, RuntimePwmProxyError) else 400
                self._send_json({"ok": False, "error": str(error)}, HTTPStatus(status))
            return
        bitstream_prefix = "/api/runtime/timed-bitstream/"
        operation = path[len(bitstream_prefix):] if path.startswith(bitstream_prefix) else None
        if operation in {"configure", "frame", "stop"}:
            proxy = self.runtime_pwm_proxy
            if proxy is None:
                self._send_json({"ok": False, "error": "Runtime定时位流代理未启用"},
                                HTTPStatus.SERVICE_UNAVAILABLE)
                return
            try:
                if parsed.query:
                    raise RuntimePwmProxyError("定时位流写入端点不接受查询参数", 400)
                if self.headers.get_content_type() != "application/json":
                    raise RuntimePwmProxyError("定时位流请求必须使用application/json", 415)
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > proxy.MAXIMUM_BODY_BYTES:
                    raise RuntimePwmProxyError("定时位流请求长度无效或超过4096字节", 400)
                response = proxy.timed_bitstream(operation, self.rfile.read(length))
                self._send_json(response.document, HTTPStatus(response.status))
            except (ValueError, RuntimePwmProxyError) as error:
                status = error.status if isinstance(error, RuntimePwmProxyError) else 400
                self._send_json({"ok": False, "error": str(error)}, HTTPStatus(status))
            return
        if path in {"/api/runtime/pwm/configure", "/api/runtime/pwm/stop"}:
            proxy = self.runtime_pwm_proxy
            if proxy is None:
                self._send_json({"ok": False, "error": "Runtime PWM代理未启用"},
                                HTTPStatus.SERVICE_UNAVAILABLE)
                return
            try:
                if parsed.query:
                    raise RuntimePwmProxyError("PWM写入端点不接受查询参数", 400)
                if self.headers.get_content_type() != "application/json":
                    raise RuntimePwmProxyError("PWM请求必须使用application/json", 415)
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > proxy.MAXIMUM_BODY_BYTES:
                    raise RuntimePwmProxyError("PWM请求长度无效或超过4096字节", 400)
                body = self.rfile.read(length)
                response = proxy.stop(body) if path.endswith("/stop") else \
                    proxy.configure(body)
                self._send_json(response.document, HTTPStatus(response.status))
            except (ValueError, RuntimePwmProxyError) as error:
                status = error.status if isinstance(error, RuntimePwmProxyError) else 400
                self._send_json({"ok": False, "error": str(error)}, HTTPStatus(status))
            return
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
                        "/api/deployment/can-katapult/preflight",
                        "/api/deployment/can-katapult/execute",
                        "/api/deployment/usb-katapult/preflight",
                        "/api/deployment/usb-katapult/execute",
                        "/api/deployment/history/export",
                        "/api/deployment/history/bundle",
                        "/api/deployment/history/bundle/import",
                        "/api/device-parameters/write-preflight",
                        "/api/device-parameters/restore-preflight",
                        "/api/device-parameters/execute",
                        "/api/project/build"):
            self._send_json({"error": "未知API"}, HTTPStatus.NOT_FOUND)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            maximum_request = (45 * 1024 * 1024 if path ==
                               "/api/deployment/history/bundle/import" else 262144)
            if length <= 0 or length > maximum_request:
                raise ProjectConfigError("请求长度无效或超过端点上限")
            request = json.loads(self.rfile.read(length).decode("utf-8"))
            catalog = None if path.startswith((
                "/api/production-batch/", "/api/production-history/",
                "/api/device-parameters/", "/api/deployment/")) else \
                json.loads(CATALOG_PATH.read_text(encoding="utf-8"))
            project = request.get("project")
            if path.startswith("/api/deployment/"):
                if path == "/api/deployment/history/bundle/import":
                    workflow = self.studio_deployment_workflow
                    if workflow is None:
                        self._send_json({"ok": False,
                            "error": "Studio部署历史未启用"},
                            HTTPStatus.SERVICE_UNAVAILABLE)
                        return
                    if set(request) != {"bundle_base64"}:
                        raise FirmwareDeploymentError("部署证据包导入字段无效")
                    response = workflow.import_evidence_bundle(
                        request["bundle_base64"])
                    self._send_json(response)
                    return
                if path == "/api/deployment/history/bundle":
                    workflow = self.studio_deployment_workflow
                    if workflow is None:
                        self._send_json({"ok": False,
                            "error": "Studio部署历史未启用"},
                            HTTPStatus.SERVICE_UNAVAILABLE)
                        return
                    if set(request) != {"attempt_filename"}:
                        raise FirmwareDeploymentError("部署证据包请求字段无效")
                    response = workflow.create_evidence_bundle(
                        request["attempt_filename"])
                    self._send_json(response)
                    return
                if path == "/api/deployment/history/export":
                    workflow = self.studio_deployment_workflow
                    if workflow is None:
                        self._send_json({"ok": False,
                            "error": "Studio部署历史未启用"},
                            HTTPStatus.SERVICE_UNAVAILABLE)
                        return
                    if set(request) != {"build_id", "backend"}:
                        raise FirmwareDeploymentError("部署证据清单筛选字段无效")
                    response = workflow.export_manifest(
                        build_id=request["build_id"], backend=request["backend"])
                    self._send_json(response)
                    return
                can_katapult = path.startswith("/api/deployment/can-katapult/")
                usb_katapult = path.startswith("/api/deployment/usb-katapult/")
                workflow = self.studio_deployment_workflow
                controller = (self.can_katapult_deployment_controller if
                              can_katapult else self.usb_katapult_deployment_controller
                              if usb_katapult else self.deployment_controller)
                if controller is None and workflow is None:
                    self._send_json(
                        {"ok": False, "error": ("Studio未配置受控CAN Katapult部署入口"
                         if can_katapult else "Studio未配置受控USB Katapult部署入口"
                         if usb_katapult else "Studio未配置受控ST-Link部署入口")},
                        HTTPStatus.SERVICE_UNAVAILABLE)
                    return
                if path.endswith("/preflight"):
                    expected_fields = ({"build_id", "expected_uuid", "can_interface",
                                        "katapult_uuid"} if can_katapult else
                                       {"build_id", "expected_uuid"})
                    if set(request) != expected_fields:
                        raise FirmwareDeploymentError("Web部署预检字段集合无效")
                    if workflow is not None:
                        backend = ("can-katapult" if can_katapult else
                                   "usb-katapult" if usb_katapult else
                                   "stlink-openocd")
                        target = ({"katapult_uuid": request["katapult_uuid"]}
                                  if can_katapult else None)
                        response = workflow.preflight(
                            backend=backend, build_id=request["build_id"],
                            expected_uuid=request["expected_uuid"], target=target)
                    else:
                        response = controller.preflight(**request)
                else:
                    if set(request) != {"confirmation_token", "confirmation",
                                       "flash_timeout", "reconnect_timeout",
                                       "poll_interval"}:
                        raise FirmwareDeploymentError("Web部署执行字段集合无效")
                    if workflow is not None:
                        response = workflow.execute(
                            confirmation_token=request["confirmation_token"],
                            execute=True, confirmation=request["confirmation"],
                            flash_timeout=request["flash_timeout"],
                            reconnect_timeout=request["reconnect_timeout"],
                            poll_interval=request["poll_interval"])
                    else:
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
                    controller = self.parameter_write_controller
                    if controller is None:
                        self._send_json(
                            {"ok": False,
                             "error": "Studio未显式启用受控设备参数写入"},
                            HTTPStatus.SERVICE_UNAVAILABLE)
                        return
                    if path.endswith("/write-preflight"):
                        if set(request) != {"expected_uuid",
                                           "expected_generation",
                                           "parameter_id", "value_base64"}:
                            raise DeviceParameterError("Web参数写入预检字段无效")
                        response = controller.preflight_write(**request)
                    elif path.endswith("/restore-preflight"):
                        if set(request) != {"expected_uuid",
                                           "expected_generation", "backup"}:
                            raise DeviceParameterError("Web参数恢复预检字段无效")
                        response = controller.preflight_restore(**request)
                    else:
                        if set(request) != {"confirmation_token",
                                           "confirmation"}:
                            raise DeviceParameterError("Web参数执行字段无效")
                        response = controller.execute(**request)
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
                can_katapult_deployment_controller:
                    WebCanKatapultDeploymentController | None = None,
                usb_katapult_deployment_controller:
                    WebUsbKatapultDeploymentController | None = None,
                studio_deployment_workflow:
                    StudioDeploymentWorkflow | None = None,
                parameter_write_controller:
                    WebDeviceParameterController | None = None,
                runtime_pwm_proxy: RuntimePwmProxy | None = None,
                ) -> ThreadingHTTPServer:
    if runtime_pwm_proxy is not None:
        try:
            if not ipaddress.ip_address(host).is_loopback:
                raise ValueError("启用Runtime PWM代理时Studio必须监听数字回环地址")
        except ValueError as error:
            raise ValueError("启用Runtime PWM代理时Studio必须监听数字回环地址") from error
    server = ThreadingHTTPServer((host, port), GuiRequestHandler)
    server.state_path = state_path  # type: ignore[attr-defined]
    server.build_jobs = build_jobs  # type: ignore[attr-defined]
    server.build_output_root = build_output_root  # type: ignore[attr-defined]
    server.production_history = ProductionHistoryStore(  # type: ignore[attr-defined]
        history_root)
    server.device_parameter_manager = device_parameter_manager  # type: ignore[attr-defined]
    server.deployment_controller = deployment_controller  # type: ignore[attr-defined]
    server.can_katapult_deployment_controller = can_katapult_deployment_controller  # type: ignore[attr-defined]
    server.usb_katapult_deployment_controller = usb_katapult_deployment_controller  # type: ignore[attr-defined]
    server.studio_deployment_workflow = studio_deployment_workflow  # type: ignore[attr-defined]
    server.parameter_write_controller = parameter_write_controller  # type: ignore[attr-defined]
    server.runtime_pwm_proxy = runtime_pwm_proxy  # type: ignore[attr-defined]
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
    parser.add_argument("--enable-can-katapult-deployment", action="store_true",
                        help="显式启用本地Web两阶段CAN Katapult部署入口")
    parser.add_argument("--can-katapult-interface",
                        help="Web CAN Katapult唯一允许使用的CAN接口")
    parser.add_argument("--katapult-flashtool", type=Path,
                        help="服务端固定的Katapult flashtool.py路径")
    parser.add_argument("--enable-usb-katapult-deployment", action="store_true",
                        help="显式启用本地Web两阶段USB Katapult部署入口")
    parser.add_argument("--usb-katapult-device",
                        help="Web USB Katapult唯一允许的/dev/serial/by-id设备")
    parser.add_argument("--enable-device-parameter-write", action="store_true",
                        help="显式启用本地Web设备参数写入与恢复")
    parser.add_argument("--parameter-audit-dir", type=Path,
                        help="设备参数写入审计目录")
    parser.add_argument("--parameter-audit-key-file", type=Path,
                        help="设备参数写入审计HMAC密钥文件")
    parser.add_argument("--enable-runtime-pwm-proxy", action="store_true",
                        help="显式启用Studio到Runtime的PWM认证代理")
    parser.add_argument("--runtime-api-url", default="http://127.0.0.1:8080",
                        help="Runtime本机HTTP地址（必须为数字回环地址）")
    parser.add_argument("--runtime-api-key-file", type=Path,
                        help="仅由Studio服务端读取的Runtime API key文件")
    parser.add_argument("--runtime-proxy-timeout-ms", type=int, default=3000,
                        help="Runtime PWM代理超时，默认3000毫秒")
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
    can_katapult_controller = None
    if args.enable_can_katapult_deployment:
        if not args.toolbusd_socket or not args.can_katapult_interface or \
                args.katapult_flashtool is None:
            parser.error("启用Web CAN Katapult部署必须配置toolbusd、CAN接口和flashtool")
        try:
            can_katapult_controller = WebCanKatapultDeploymentController(
                output_root=DEFAULT_OUTPUT_ROOT,
                record_root=args.deployment_record_root,
                can_interface=args.can_katapult_interface,
                flashtool=args.katapult_flashtool,
                reader_factory=lambda expected_uuid: ToolbusdIdentityReader(
                    args.toolbusd_socket, args.node_id,
                    expected_uuid=expected_uuid, remote_cli=args.remote_cli))
        except FirmwareDeploymentError as error:
            parser.error(str(error))
    usb_katapult_controller = None
    if args.enable_usb_katapult_deployment:
        if not args.toolbusd_socket or not args.usb_katapult_device or \
                args.katapult_flashtool is None:
            parser.error("启用Web USB Katapult部署必须配置toolbusd、USB设备和flashtool")
        try:
            usb_katapult_controller = WebUsbKatapultDeploymentController(
                output_root=DEFAULT_OUTPUT_ROOT,
                record_root=args.deployment_record_root,
                usb_device=args.usb_katapult_device,
                flashtool=args.katapult_flashtool,
                reader_factory=lambda expected_uuid: ToolbusdIdentityReader(
                    args.toolbusd_socket, args.node_id,
                    expected_uuid=expected_uuid, remote_cli=args.remote_cli))
        except FirmwareDeploymentError as error:
            parser.error(str(error))
    parameter_write_controller = None
    if args.enable_device_parameter_write:
        if parameter_manager is None or args.parameter_audit_dir is None or \
                args.parameter_audit_key_file is None:
            parser.error("启用Web参数写入必须配置toolbusd、audit-dir和audit-key-file")
        try:
            parameter_write_controller = WebDeviceParameterController(
                parameter_manager,
                ParameterAuditStore(args.parameter_audit_dir,
                                    args.parameter_audit_key_file))
        except DeviceParameterError as error:
            parser.error(str(error))
    runtime_pwm_proxy = None
    if args.enable_runtime_pwm_proxy:
        if args.runtime_api_key_file is None:
            parser.error("启用Runtime PWM代理必须配置--runtime-api-key-file")
        try:
            runtime_pwm_proxy = RuntimePwmProxy.from_key_file(
                args.runtime_api_url, args.runtime_api_key_file,
                timeout_seconds=args.runtime_proxy_timeout_ms / 1000.0)
        except ValueError as error:
            parser.error(str(error))
    studio_deployment_workflow = None
    backend_config = {}
    if deployment_controller is not None:
        backend_config["stlink-openocd"] = {"probe_serial": None}
    if can_katapult_controller is not None:
        backend_config["can-katapult"] = {
            "can_interface": args.can_katapult_interface,
            "flashtool": str(args.katapult_flashtool)}
    if usb_katapult_controller is not None:
        backend_config["usb-katapult"] = {
            "usb_device": args.usb_katapult_device,
            "flashtool": str(args.katapult_flashtool)}
    if backend_config:
        try:
            studio_deployment_workflow = StudioDeploymentWorkflow(
                output_root=DEFAULT_OUTPUT_ROOT,
                attempt_root=args.deployment_record_root / "attempts",
                reader_factory=lambda expected_uuid: ToolbusdIdentityReader(
                    args.toolbusd_socket, args.node_id,
                    expected_uuid=expected_uuid, remote_cli=args.remote_cli),
                backend_config=backend_config)
        except FirmwareDeploymentError as error:
            parser.error(str(error))
    server = make_server(
        args.host, args.port, args.state, build_jobs=args.build_jobs,
        history_root=args.history_root,
        device_parameter_manager=parameter_manager,
        deployment_controller=deployment_controller,
        can_katapult_deployment_controller=can_katapult_controller,
        usb_katapult_deployment_controller=usb_katapult_controller,
        studio_deployment_workflow=studio_deployment_workflow,
        parameter_write_controller=parameter_write_controller,
        runtime_pwm_proxy=runtime_pwm_proxy)
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
