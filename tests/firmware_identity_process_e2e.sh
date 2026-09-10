#!/usr/bin/env bash
set -euo pipefail

mock_mcu_bin="$1"
toolbusd_bin="$2"
remote_cli_bin="$3"
full_manifest="$4"
unavailable_manifest="$5"

link_path="/tmp/remotebsp-identity-link-$$.sock"
socket_path="/tmp/remotebsp-identity-ipc-$$.sock"
ledger_dir="$(mktemp -d /tmp/remotebsp-identity-ledger-XXXXXX)"
mock_log="/tmp/remotebsp-identity-mock-$$.log"
daemon_log="/tmp/remotebsp-identity-daemon-$$.log"
mock_pid=""
daemon_pid=""

cleanup() {
    local result=$?
    for pid in "$mock_pid" "$daemon_pid"; do
        if [[ -n "$pid" ]]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    if [[ $result -ne 0 ]]; then
        printf '%s\n' '--- Mock MCU 日志 ---'
        sed -n '1,160p' "$mock_log" 2>/dev/null || true
        printf '%s\n' '--- toolbusd 日志 ---'
        sed -n '1,200p' "$daemon_log" 2>/dev/null || true
    fi
    rm -f -- "$link_path" "$socket_path" "$mock_log" "$daemon_log"
    rm -rf -- "$ledger_dir"
    exit "$result"
}
trap cleanup EXIT INT TERM

"$toolbusd_bin" "$link_path" usb-mock "$socket_path" \
    --runtime-operation-ledger-dir "$ledger_dir" >"$daemon_log" 2>&1 &
daemon_pid=$!
"$mock_mcu_bin" "$link_path" usb-mock --instance 1 --board "$full_manifest" \
    >"$mock_log" 2>&1 &
mock_pid=$!

identity=""
for _ in $(seq 1 500); do
    if [[ -S "$socket_path" ]] &&
       identity="$("$remote_cli_bin" --json --socket "$socket_path" \
           --node 1 firmware-identity 2>/dev/null)"; then
        break
    fi
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done

python3 -c '
import json, sys
v=json.load(sys.stdin)
assert v["schema_version"] == 1
assert v["command"] == "firmware-identity"
assert v["identity_schema_version"] == 1
assert v["board_type"] == 66577
assert v["uuid"] == "00112233445566778899aabbccddee01"
assert v["project_sha256"] == "11" * 32
assert v["config_sha256"] == "22" * 32
assert v["firmware_input_sha256"] == "33" * 32
' <<<"$identity"

if "$remote_cli_bin" --json --socket "$socket_path" --node 127 \
        firmware-identity >/dev/null 2>&1; then
    echo "错误节点意外返回了固件身份" >&2
    exit 1
fi

# 同一 UUID 的节点以逐字段 unavailable 的描述重启后，不能沿用重启前的三项哈希。
kill "$mock_pid"
wait "$mock_pid" 2>/dev/null || true
mock_pid=""
offline=0
for _ in $(seq 1 160); do
    node_list="$("$remote_cli_bin" --socket "$socket_path" node-list 2>/dev/null || true)"
    if grep -Fq 'online=0' <<<"$node_list"; then
        offline=1
        break
    fi
    kill -0 "$daemon_pid"
    sleep 0.02
done
[[ "$offline" -eq 1 ]]
if "$remote_cli_bin" --socket "$socket_path" --node 1 \
        firmware-identity >/dev/null 2>&1; then
    echo "离线节点意外返回了固件身份" >&2
    exit 1
fi

"$mock_mcu_bin" "$link_path" usb-mock --instance 1 --board "$unavailable_manifest" \
    >>"$mock_log" 2>&1 &
mock_pid=$!
legacy_identity=""
for _ in $(seq 1 500); do
    if legacy_identity="$("$remote_cli_bin" --json --socket "$socket_path" \
        --node 1 firmware-identity 2>/dev/null)"; then
        break
    fi
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done
python3 -c '
import json, sys
v=json.load(sys.stdin)
assert v["board_type"] == 66577
assert v["uuid"] == "00112233445566778899aabbccddee01"
assert v["project_sha256"] is None
assert v["config_sha256"] is None
assert v["firmware_input_sha256"] is None
' <<<"$legacy_identity"
