#!/usr/bin/env bash
set -euo pipefail

mock_mcu_bin="$1"
toolbusd_bin="$2"
remote_cli_bin="$3"
client_api_bin="$4"
can_interface="$5"
can_mode="$6"

socket_path="/tmp/remotebsp-e2e-${can_mode}-$$.sock"
mock_log="/tmp/remotebsp-mock-${can_mode}-$$.log"
daemon_log="/tmp/remotebsp-daemon-${can_mode}-$$.log"
trace_log="/tmp/remotebsp-can-${can_mode}-$$.log"
mock_pid=""
daemon_pid=""
trace_pid=""

cleanup() {
    local result=$?
    if [[ -n "$trace_pid" ]]; then
        kill "$trace_pid" 2>/dev/null || true
        wait "$trace_pid" 2>/dev/null || true
    fi
    if [[ -n "$daemon_pid" ]]; then
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
    fi
    if [[ -n "$mock_pid" ]]; then
        kill "$mock_pid" 2>/dev/null || true
        wait "$mock_pid" 2>/dev/null || true
    fi
    if [[ $result -ne 0 ]]; then
        printf '%s\n' '--- mock_mcu 日志 ---'
        sed -n '1,200p' "$mock_log" 2>/dev/null || true
        printf '%s\n' '--- toolbusd 日志 ---'
        sed -n '1,200p' "$daemon_log" 2>/dev/null || true
        printf '%s\n' '--- vcan0 帧记录 ---'
        sed -n '1,300p' "$trace_log" 2>/dev/null || true
    fi
    rm -f -- "$socket_path" "$mock_log" "$daemon_log" "$trace_log"
    exit "$result"
}
trap cleanup EXIT INT TERM

if command -v candump >/dev/null 2>&1; then
    candump -L "$can_interface" >"$trace_log" 2>&1 &
    trace_pid=$!
fi
"$mock_mcu_bin" "$can_interface" "$can_mode" --uart-stream \
    >"$mock_log" 2>&1 &
mock_pid=$!
"$toolbusd_bin" "$can_interface" "$can_mode" "$socket_path" \
    >"$daemon_log" 2>&1 &
daemon_pid=$!

for _ in $(seq 1 100); do
    [[ -S "$socket_path" ]] && break
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done
[[ -S "$socket_path" ]]

ping_output=""
for _ in $(seq 1 100); do
    if ping_output="$("$remote_cli_bin" --socket "$socket_path" \
        ping "端到端测试" 2>/dev/null)"; then
        break
    fi
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done
grep -Fq 'pong=端到端测试' <<<"$ping_output"

info_output="$("$remote_cli_bin" --socket "$socket_path" get-info)"
grep -Fq 'firmware=0.1.0' <<<"$info_output"
grep -Fq 'protocol_version=1' <<<"$info_output"

capability_output="$("$remote_cli_bin" --socket "$socket_path" get-capability)"
grep -Fq 'capabilities=0xff' <<<"$capability_output"

event_output="$("$remote_cli_bin" --socket "$socket_path" event-wait)"
grep -Fq 'command=0x280 resource_id=0x2000007' <<<"$event_output"
grep -Fq 'data_hex=6d6f636b2d75617274370a' <<<"$event_output"

resource_output="$("$remote_cli_bin" --socket "$socket_path" resource-list)"
[[ "$(grep -c ' type=uart ' <<<"$resource_output")" -eq 8 ]]
grep -Fq 'resource_id=0x2000007 type=uart instance=7 source=expanded rx_capacity=4096 tx_capacity=4096' \
    <<<"$resource_output"

describe_output="$("$remote_cli_bin" --socket "$socket_path" \
    resource-describe 0x02000007)"
grep -Fq 'instance=7 source=expanded rx_capacity=4096 tx_capacity=4096' \
    <<<"$describe_output"

status_output="$("$remote_cli_bin" --socket "$socket_path" \
    resource-status 0x02000007)"
grep -Fq 'health=0 error_flags=0x0' <<<"$status_output"

create_output="$("$remote_cli_bin" --socket "$socket_path" \
    gpio-create 13 output 0)"
object_id="${create_output#object_id=}"
[[ "$object_id" =~ ^[1-9][0-9]*$ ]]

write_output="$("$remote_cli_bin" --socket "$socket_path" \
    gpio-write "$object_id" 1)"
[[ "$write_output" == "ok" ]]

read_output="$("$remote_cli_bin" --socket "$socket_path" \
    gpio-read "$object_id")"
[[ "$read_output" == "value=1" ]]

uart_create_output="$("$remote_cli_bin" --socket "$socket_path" \
    uart-create 0 115200 8 none 1)"
uart_object_id="${uart_create_output#object_id=}"
[[ "$uart_object_id" =~ ^[1-9][0-9]*$ ]]

uart_write_output="$("$remote_cli_bin" --socket "$socket_path" \
    uart-write "$uart_object_id" "UART-E2E")"
[[ "$uart_write_output" == "ok" ]]

"$client_api_bin" "$socket_path"

parallel_pids=()
parallel_files=()
for index in $(seq 1 8); do
    output_file="/tmp/remotebsp-parallel-${can_mode}-$$-${index}.out"
    parallel_files+=("$output_file")
    "$remote_cli_bin" --socket "$socket_path" ping "parallel-${index}" \
        >"$output_file" &
    parallel_pids+=("$!")
done
for pid in "${parallel_pids[@]}"; do
    wait "$pid"
done
for index in $(seq 1 8); do
    grep -Fq "pong=parallel-${index}" \
        "${parallel_files[$((index - 1))]}"
done
rm -f -- "${parallel_files[@]}"

printf '%s 模式端到端测试通过\n' "$can_mode"
