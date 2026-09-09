#!/usr/bin/env bash
set -euo pipefail

mock_mcu_bin="$1"
toolbusd_bin="$2"
remote_cli_bin="$3"
can_interface="$4"
can_mode="$5"

if [[ "$can_mode" == "usb-mock" ]]; then
    can_interface="/tmp/remotebsp-multi-usb-link-$$.sock"
fi

# 此用例只使用 vcan 或本地 USB Mock，验证多进程事务语义，不承担实体
# 同步精度验收。WSL 在全量测试负载下的调度抖动和 100 ms 未来外推误差
# 可能超过生产门槛，因此只为纯软件测试使用 10 ms 上限；toolbusd 的生产
# 默认 100 us 保持不变，实体 CAN/CAN-FD 精度由硬件验收计划单独判定。
motion_clock_limit_ns=10000000
motion_clock_args=(--motion-max-clock-error-ns "$motion_clock_limit_ns")

socket_path="/tmp/remotebsp-multi-${can_mode}-$$.sock"
mock1_log="/tmp/remotebsp-multi-mock1-${can_mode}-$$.log"
mock2_log="/tmp/remotebsp-multi-mock2-${can_mode}-$$.log"
daemon_log="/tmp/remotebsp-multi-daemon-${can_mode}-$$.log"
node1_output="/tmp/remotebsp-node1-$$.out"
node2_output="/tmp/remotebsp-node2-$$.out"
mock1_pid=""
mock2_pid=""
daemon_pid=""
mock1_stopped=0

cleanup() {
    local result=$?
    if [[ "$mock1_stopped" -eq 1 && -n "$mock1_pid" ]]; then
        kill -CONT "$mock1_pid" 2>/dev/null || true
    fi
    for pid in "$daemon_pid" "$mock2_pid" "$mock1_pid"; do
        if [[ -n "$pid" ]]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    if [[ $result -ne 0 ]]; then
        printf '%s\n' '--- Mock MCU 1 日志 ---'
        sed -n '1,200p' "$mock1_log" 2>/dev/null || true
        printf '%s\n' '--- Mock MCU 2 日志 ---'
        sed -n '1,200p' "$mock2_log" 2>/dev/null || true
        printf '%s\n' '--- toolbusd 日志 ---'
        sed -n '1,300p' "$daemon_log" 2>/dev/null || true
    fi
    rm -f -- "$socket_path" "$mock1_log" "$mock2_log" "$daemon_log" \
        "$node1_output" "$node2_output"
    if [[ "$can_mode" == "usb-mock" ]]; then
        rm -f -- "$can_interface"
    fi
    exit "$result"
}
trap cleanup EXIT INT TERM

"$mock_mcu_bin" "$can_interface" "$can_mode" --instance 1 \
    >"$mock1_log" 2>&1 &
mock1_pid=$!
"$mock_mcu_bin" "$can_interface" "$can_mode" --instance 2 \
    >"$mock2_log" 2>&1 &
mock2_pid=$!
"$toolbusd_bin" "$can_interface" "$can_mode" "$socket_path" \
    "${motion_clock_args[@]}" \
    >"$daemon_log" 2>&1 &
daemon_pid=$!

for _ in $(seq 1 150); do
    if [[ -S "$socket_path" ]] &&
       "$remote_cli_bin" --socket "$socket_path" --node 1 get-info \
           >"$node1_output" 2>/dev/null &&
       "$remote_cli_bin" --socket "$socket_path" --node 2 get-info \
           >"$node2_output" 2>/dev/null; then
        break
    fi
    kill -0 "$mock1_pid"
    kill -0 "$mock2_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done

node1_info="$(cat "$node1_output")"
node2_info="$(cat "$node2_output")"
rm -f -- "$node1_output" "$node2_output"
grep -Fq 'board_type=0x4d4f434b' <<<"$node1_info"
grep -Fq 'board_type=0x4d4f434b' <<<"$node2_info"
[[ "$node1_info" != "$node2_info" ]]
[[ "$(stat -c '%a' "$socket_path")" == "660" ]]
node_list="$("$remote_cli_bin" --socket "$socket_path" node-list)"
[[ "$(grep -c 'online=1' <<<"$node_list")" -eq 2 ]]

node1_gpio="$("$remote_cli_bin" --socket "$socket_path" --node 1 \
    gpio-create 13 output 0)"
node2_gpio="$("$remote_cli_bin" --socket "$socket_path" --node 2 \
    gpio-create 13 output 0)"
node1_object="${node1_gpio#object_id=}"
node2_object="${node2_gpio#object_id=}"

"$remote_cli_bin" --socket "$socket_path" --node 1 \
    gpio-write "$node1_object" 1 >/dev/null
[[ "$("$remote_cli_bin" --socket "$socket_path" --node 1 \
    gpio-read "$node1_object")" == "value=1" ]]
[[ "$("$remote_cli_bin" --socket "$socket_path" --node 2 \
    gpio-read "$node2_object")" == "value=0" ]]

# 运动资源默认要求独占租约。运动组事务复用 toolbusd 的守护进程会话，
# 因此先为每个成员节点取得全部步进轴租约，再提交 PREPARE。
for node_id in 1 2; do
    for resource_id in 0x09000000 0x09000001 0x09000002; do
        lease_output="$("$remote_cli_bin" --socket "$socket_path" \
            --node "$node_id" resource-acquire "$resource_id" \
            30000 exclusive)"
        grep -Eq 'lease_id=0x[0-9a-fA-F]+' <<<"$lease_output"
    done
done

# 租约 RPC 完成后再等待两个节点的时钟模型达到跨板运动准入质量，
# 避免把先前合格、随后已经过期的快照用于事务冻结。
clock_ready=0
for _ in $(seq 1 150); do
    snapshot="$("$remote_cli_bin" --json --socket "$socket_path" \
        runtime-snapshot 64 2000 2>/dev/null || true)"
    if python3 -c '
import json, sys
value = json.load(sys.stdin)["data"]
clocks = value["clocks"]
assert len(clocks) == 2
assert all(c["state"] == "synced" for c in clocks)
assert all(c["error_bound_ns"] is not None and
           c["error_bound_ns"] <= int(sys.argv[1]) for c in clocks)
' "$motion_clock_limit_ns" <<<"$snapshot" 2>/dev/null; then
        clock_ready=1
        break
    fi
    kill -0 "$mock1_pid"
    kill -0 "$mock2_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done
[[ "$clock_ready" -eq 1 ]]

digest="0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
group_start_ns="$(python3 -c 'import time; print(time.monotonic_ns() + 100000000)')"
group_submit="$("$remote_cli_bin" --socket "$socket_path" \
    motion-group-submit 9001 77 1 "$group_start_ns" "$digest" \
    1/1/2000000/final/0x09000000:4,0x09000001:0,0x09000002:0 \
    2/1/2000000/final/0x09000000:0,0x09000001:-5,0x09000002:0)"
grep -Eq 'state=(preparing|committing|committed)' <<<"$group_submit"

group_status=""
for _ in $(seq 1 100); do
    group_status="$("$remote_cli_bin" --socket "$socket_path" \
        motion-group-status 9001 77 1)"
    grep -Fq 'state=committed' <<<"$group_status" && break
    if grep -Eq 'state=(aborting|aborted)' <<<"$group_status"; then
        echo "跨板运动组意外进入 ABORT: $group_status" >&2
        exit 1
    fi
    sleep 0.01
done
grep -Fq 'state=committed' <<<"$group_status"
grep -Fq 'committed=2' <<<"$group_status"
grep -Fq 'commit_dispatched=1' <<<"$group_status"
grep -Fq 'state=armed' <<<"$("$remote_cli_bin" --socket "$socket_path" \
    --node 1 motion-status)"
grep -Fq 'state=armed' <<<"$("$remote_cli_bin" --socket "$socket_path" \
    --node 2 motion-status)"

# 等待第一组完成，然后暂停一个 Mock，使第二组稳定停留在 PREPARE，
# 验证短连接提交者退出不会自动取消，而显式取消会全组 ABORT。
sleep 0.55
kill -STOP "$mock1_pid"
mock1_stopped=1
group_start_ns="$(python3 -c 'import time; print(time.monotonic_ns() + 100000000)')"
cancel_submit="$("$remote_cli_bin" --socket "$socket_path" \
    motion-group-submit 9002 77 2 "$group_start_ns" "$digest" \
    1/2/2000000/final/0x09000000:3,0x09000001:0,0x09000002:0 \
    2/2/2000000/final/0x09000000:0,0x09000001:3,0x09000002:0)"
grep -Fq 'state=preparing' <<<"$cancel_submit"
cancel_status="$("$remote_cli_bin" --socket "$socket_path" \
    motion-group-cancel 9002 77 2)"
grep -Fq 'state=aborting' <<<"$cancel_status"
grep -Fq 'commit_dispatched=0' <<<"$cancel_status"
grep -Fq 'abort_scope=pre-commit' <<<"$cancel_status"
kill -CONT "$mock1_pid"
mock1_stopped=0

for _ in $(seq 1 100); do
    cancel_status="$("$remote_cli_bin" --socket "$socket_path" \
        motion-group-status 9002 77 2)"
    grep -Fq 'state=aborted' <<<"$cancel_status" && break
    sleep 0.01
done
grep -Fq 'state=aborted' <<<"$cancel_status"

if grep -Eq 'uuid=[0-9a-f]{30}02' <<<"$node1_info"; then
    failed_node=1
    healthy_node=2
else
    failed_node=2
    healthy_node=1
fi

kill "$mock2_pid"
wait "$mock2_pid" 2>/dev/null || true
mock2_pid=""
sleep 2.2

node_list="$("$remote_cli_bin" --socket "$socket_path" node-list)"
[[ "$(grep -c 'online=0' <<<"$node_list")" -eq 1 ]]
[[ "$(grep -c 'online=1' <<<"$node_list")" -eq 1 ]]

if "$remote_cli_bin" --socket "$socket_path" --node "$failed_node" \
    ping offline >/dev/null 2>&1; then
    echo "离线节点仍然接受请求" >&2
    exit 1
fi
"$remote_cli_bin" --socket "$socket_path" --node "$healthy_node" \
    ping healthy | grep -Fq 'pong=healthy'

printf '%s 模式双节点寻址与掉线隔离测试通过\n' "$can_mode"
