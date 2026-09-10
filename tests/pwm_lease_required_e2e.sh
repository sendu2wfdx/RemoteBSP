#!/usr/bin/env bash
set -euo pipefail
mock_bin="$1"; daemon_bin="$2"; cli_bin="$3"; source_root="$4"
work="$(mktemp -d /tmp/remotebsp-pwm-lease-XXXXXX)"
link="$work/link.sock"; socket="$work/toolbusd.sock"
cleanup() {
    result=$?
    [[ -n "${daemon_pid:-}" ]] && kill "$daemon_pid" 2>/dev/null || true
    [[ -n "${mock_pid:-}" ]] && kill "$mock_pid" 2>/dev/null || true
    wait "${daemon_pid:-}" 2>/dev/null || true
    wait "${mock_pid:-}" 2>/dev/null || true
    if [[ $result -ne 0 ]]; then
        sed -n '1,200p' "$work/mock.log" 2>/dev/null || true
        sed -n '1,200p' "$work/daemon.log" 2>/dev/null || true
    fi
    rm -rf -- "$work"
    exit "$result"
}
trap cleanup EXIT INT TERM
mkdir "$work/ledger"
chmod 700 "$work/ledger"
python3 - "$source_root/boards/mock-generic-v1.json" "$work/board.json" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    board = json.load(source)
for group in board["resource_groups"]:
    if group["type"] == "pwm":
        group["contract"]["access"].append("lease_required")
with open(sys.argv[2], "w", encoding="utf-8") as output:
    json.dump(board, output)
PY
"$mock_bin" "$link" usb-mock --board "$work/board.json" >"$work/mock.log" 2>&1 & mock_pid=$!
extra=()
mode="lease-required"
if [[ "${5:-}" == "quarantine" ]]; then
    extra=(--test-pwm-acquire-drop-response 1 --test-pwm-remote-lease-ttl-ms 300)
    mode="quarantine"
fi
"$daemon_bin" "$link" usb-mock "$socket" --runtime-operation-ledger-dir "$work/ledger" "${extra[@]}" >"$work/daemon.log" 2>&1 & daemon_pid=$!
for _ in $(seq 1 500); do
    [[ -S "$socket" ]] && "$cli_bin" --socket "$socket" ping ready >/dev/null 2>&1 && break
    kill -0 "$mock_pid"; kill -0 "$daemon_pid"; sleep 0.02
done
[[ -S "$socket" ]]
PYTHONPATH="$source_root" python3 "$source_root/tests/pwm_runtime_process_e2e.py" \
    "$socket" "$cli_bin" "$mode"
