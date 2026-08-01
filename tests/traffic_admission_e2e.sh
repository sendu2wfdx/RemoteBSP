#!/usr/bin/env bash
set -euo pipefail

mock_mcu_bin="$1"
toolbusd_bin="$2"
remote_cli_bin="$3"
can_interface="$4"

socket_path="/tmp/remotebsp-traffic-$$.sock"
mock_log="/tmp/remotebsp-traffic-mock-$$.log"
daemon_log="/tmp/remotebsp-traffic-daemon-$$.log"
reject_log="/tmp/remotebsp-traffic-reject-$$.out"
mock_pid=""
daemon_pid=""

cleanup() {
    local result=$?
    for pid in "$daemon_pid" "$mock_pid"; do
        if [[ -n "$pid" ]]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    if [[ $result -ne 0 ]]; then
        printf '%s\n' '--- Mock MCU 日志 ---'
        sed -n '1,200p' "$mock_log" 2>/dev/null || true
        printf '%s\n' '--- toolbusd 日志 ---'
        sed -n '1,300p' "$daemon_log" 2>/dev/null || true
    fi
    rm -f -- "$socket_path" "$mock_log" "$daemon_log" "$reject_log"
    exit "$result"
}
trap cleanup EXIT INT TERM

"$mock_mcu_bin" "$can_interface" classical \
    >"$mock_log" 2>&1 &
mock_pid=$!
"$toolbusd_bin" "$can_interface" classical "$socket_path" \
    --max-utilization-permille 100 \
    --burst-window-ms 20 \
    >"$daemon_log" 2>&1 &
daemon_pid=$!

for _ in $(seq 1 150); do
    if [[ -S "$socket_path" ]] &&
       "$remote_cli_bin" --socket "$socket_path" --node 1 \
           ping ready >/dev/null 2>&1; then
        break
    fi
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done
sleep 0.03
"$remote_cli_bin" --socket "$socket_path" --node 1 \
    ping ready | grep -Fq 'pong=ready'

large_payload="$(printf 'x%.0s' {1..1500})"
if "$remote_cli_bin" --socket "$socket_path" --node 1 \
    ping "$large_payload" >"$reject_log" 2>&1; then
    echo "超预算请求未被拒绝" >&2
    exit 1
fi
grep -Fq 'CAN 带宽准入拒绝' "$reject_log"

sleep 0.03
"$remote_cli_bin" --socket "$socket_path" --node 1 \
    ping healthy | grep -Fq 'pong=healthy'

traffic="$("$remote_cli_bin" --socket "$socket_path" traffic-status)"
grep -Eq 'rejected_packets=[1-9][0-9]*' <<<"$traffic"
grep -Eq 'class=interactive .*rejected_packets=[1-9][0-9]*' \
    <<<"$traffic"
grep -Fq 'max_utilization_permille=100' <<<"$traffic"

printf '%s\n' 'Classical CAN 带宽准入端到端测试通过'
