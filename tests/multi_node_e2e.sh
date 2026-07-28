#!/usr/bin/env bash
set -euo pipefail

mock_mcu_bin="$1"
toolbusd_bin="$2"
remote_cli_bin="$3"
can_interface="$4"
can_mode="$5"

socket_path="/tmp/remotebsp-multi-${can_mode}-$$.sock"
mock1_log="/tmp/remotebsp-multi-mock1-${can_mode}-$$.log"
mock2_log="/tmp/remotebsp-multi-mock2-${can_mode}-$$.log"
daemon_log="/tmp/remotebsp-multi-daemon-${can_mode}-$$.log"
node1_output="/tmp/remotebsp-node1-$$.out"
node2_output="/tmp/remotebsp-node2-$$.out"
mock1_pid=""
mock2_pid=""
daemon_pid=""

cleanup() {
    local result=$?
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
grep -Eq 'board_type=0x[12]' <<<"$node1_info"
grep -Eq 'board_type=0x[12]' <<<"$node2_info"
[[ "$node1_info" != "$node2_info" ]]
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

if grep -Fq 'board_type=0x2' <<<"$node1_info"; then
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
