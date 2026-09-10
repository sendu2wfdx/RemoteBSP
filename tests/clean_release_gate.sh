#!/usr/bin/env bash
# 在一次性空构建目录中执行无需实体板的关键发布门禁。
set -euo pipefail

source_root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
build_root="$(mktemp -d /tmp/remotebsp-release-gate-XXXXXX)"
cleanup() {
    case "$build_root" in
        /tmp/remotebsp-release-gate-*) rm -rf -- "$build_root" ;;
        *) echo "拒绝清理非门禁临时目录：$build_root" >&2 ;;
    esac
}
trap cleanup EXIT

cmake -S "$source_root" -B "$build_root" -DBUILD_TESTING=ON
cmake --build "$build_root" --target \
    toolbusd_operation_ledger_process_tests remote-cli -j32
ctest --test-dir "$build_root" -R deployment_evidence_offline_tests \
    --output-on-failure
ctest --test-dir "$build_root" -R toolbusd_operation_ledger_process_tests \
    --output-on-failure
ctest --test-dir "$build_root" -R bus_reset_runtime_http_process_tests \
    --output-on-failure
