#!/usr/bin/env bash
set -euo pipefail

mock_bin="$1"
daemon_bin="$2"
cli_bin="$3"
source_root="$4"
work="$(mktemp -d /tmp/remotebsp-timed-bitstream-XXXXXX)"
link="$work/link.sock"
socket="$work/toolbusd.sock"

cleanup() {
    result=$?
    [[ -n "${daemon_pid:-}" ]] && kill "$daemon_pid" 2>/dev/null || true
    [[ -n "${mock_pid:-}" ]] && kill "$mock_pid" 2>/dev/null || true
    wait "${daemon_pid:-}" 2>/dev/null || true
    wait "${mock_pid:-}" 2>/dev/null || true
    if [[ $result -ne 0 ]]; then
        sed -n '1,240p' "$work/mock.log" 2>/dev/null || true
        sed -n '1,240p' "$work/daemon.log" 2>/dev/null || true
    fi
    rm -rf -- "$work"
    exit "$result"
}
trap cleanup EXIT INT TERM

mkdir "$work/ledger"
chmod 700 "$work/ledger"
python3 - "$source_root/tests/data/mock_timed_bitstream_board_v4.json" \
    "$work/board.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    board = json.load(source)
group = next(item for item in board["resource_groups"]
             if item["type"] == "timed_bitstream")
group["contract"]["access"].append("lease_required")
with open(sys.argv[2], "w", encoding="utf-8") as output:
    json.dump(board, output)
PY

"$mock_bin" "$link" usb-mock --board "$work/board.json" \
    >"$work/mock.log" 2>&1 &
mock_pid=$!
"$daemon_bin" "$link" usb-mock "$socket" \
    --runtime-operation-ledger-dir "$work/ledger" \
    >"$work/daemon.log" 2>&1 &
daemon_pid=$!

for _ in $(seq 1 500); do
    if [[ -S "$socket" ]] && \
       "$cli_bin" --socket "$socket" ping ready >/dev/null 2>&1; then
        break
    fi
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done
[[ -S "$socket" ]]

daemon_identity="$($cli_bin --socket "$socket" daemon-identity)"
daemon_instance_id="${daemon_identity#*instance_id=}"
daemon_instance_id="${daemon_instance_id%% *}"
node_uuid="00112233445566778899aabbccddee01"
lease_id="0123456789abcdeffedcba9876543210"
owner="timed-bitstream-e2e"
resource_id="0x0a000000"

health_is() {
    local expected="$1"
    local status
    status="$($cli_bin --socket "$socket" --node 1 resource-status "$resource_id")"
    grep -Fq "health_name=$expected" <<<"$status"
}

operation_field() {
    local field="$1"
    python3 -c 'import json,sys; print(json.load(sys.stdin)["data"][sys.argv[1]])' \
        "$field"
}

health_is normal
"$cli_bin" --socket "$socket" --node 1 \
    runtime-timed-bitstream-acquire "$daemon_instance_id" "$lease_id" \
    "$node_uuid" "$owner" "$resource_id" 10000 >/dev/null

configured="$($cli_bin --socket "$socket" --node 1 --json \
    runtime-timed-bitstream-configure-operation \
    "$daemon_instance_id" "$lease_id" "$node_uuid" "$owner" \
    "$resource_id" timed-configure 1250 400 800 80)"
grep -Fq '"kind":"timed_bitstream_configure"' <<<"$configured"
grep -Fq '"state":"committed"' <<<"$configured"
grep -Fq '"replayed":false' <<<"$configured"
configure_id="$(operation_field operation_id <<<"$configured")"

maximum_hex="$(python3 -c 'print("a5" * 2022)')"
frame="$($cli_bin --socket "$socket" --node 1 --json \
    runtime-timed-bitstream-frame-operation \
    "$daemon_instance_id" "$lease_id" "$node_uuid" "$owner" \
    "$resource_id" timed-frame-maximum 16176 "$maximum_hex")"
grep -Fq '"kind":"timed_bitstream_frame"' <<<"$frame"
grep -Fq '"state":"committed"' <<<"$frame"
grep -Fq '"replayed":false' <<<"$frame"
frame_id="$(operation_field operation_id <<<"$frame")"
[[ "$frame_id" =~ ^[0-9a-f]{64}$ ]]
health_is busy

frame_replay="$($cli_bin --socket "$socket" --node 1 --json \
    runtime-timed-bitstream-frame-operation \
    "$daemon_instance_id" "$lease_id" "$node_uuid" "$owner" \
    "$resource_id" timed-frame-maximum 16176 "$maximum_hex")"
grep -Fq "\"operation_id\":\"$frame_id\"" <<<"$frame_replay"
grep -Fq '"replayed":true' <<<"$frame_replay"

lookup="$($cli_bin --socket "$socket" --json runtime-operation-lookup \
    "$daemon_instance_id" "$owner" timed_bitstream_frame \
    "$lease_id" timed-frame-maximum)"
grep -Fq "\"operation_id\":\"$frame_id\"" <<<"$lookup"

# 端点和协议允许的最大值是 16176 bits；多一个 bit 必须在进入执行层前失败。
set +e
oversized="$($cli_bin --socket "$socket" --node 1 --json \
    runtime-timed-bitstream-frame-operation \
    "$daemon_instance_id" "$lease_id" "$node_uuid" "$owner" \
    "$resource_id" timed-frame-oversized 16177 "${maximum_hex}80" 2>&1)"
oversized_status=$?
set -e
[[ $oversized_status -ne 0 ]]
grep -Eq '16176|超限|过大|invalid|无效' <<<"$oversized"

stopped="$($cli_bin --socket "$socket" --node 1 --json \
    runtime-timed-bitstream-stop-operation \
    "$daemon_instance_id" "$lease_id" "$node_uuid" "$owner" \
    "$resource_id" timed-stop)"
grep -Fq '"kind":"timed_bitstream_stop"' <<<"$stopped"
grep -Fq '"state":"committed"' <<<"$stopped"
grep -Fq '"recovery":"safe_closed"' <<<"$stopped"

# STOP 只销毁波形对象；LeaseRequired 的远端租约仍应使资源保持 Busy。
health_is busy
released="$($cli_bin --socket "$socket" --node 1 --json \
    runtime-control-release-operation "$daemon_instance_id" "$lease_id" "$owner")"
grep -Fq '"kind":"control_release"' <<<"$released"
grep -Fq '"state":"committed"' <<<"$released"
health_is normal

# 保证 configure 也能由 operation ID 查询，防止测试只覆盖 frame 账本映射。
status="$($cli_bin --socket "$socket" --json runtime-operation-status \
    "$daemon_instance_id" "$owner" "$configure_id")"
grep -Fq "\"operation_id\":\"$configure_id\"" <<<"$status"
