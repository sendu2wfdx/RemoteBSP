#!/usr/bin/env bash
set -euo pipefail

mock_mcu_bin="$1"
toolbusd_bin="$2"
remote_cli_bin="$3"
client_api_bin="$4"
can_interface="$5"
can_mode="$6"

if [[ "$can_mode" == "usb-mock" ]]; then
    can_interface="/tmp/remotebsp-usb-link-$$.sock"
fi

socket_path="/tmp/remotebsp-e2e-${can_mode}-$$.sock"
mock_log="/tmp/remotebsp-mock-${can_mode}-$$.log"
daemon_log="/tmp/remotebsp-daemon-${can_mode}-$$.log"
trace_log="/tmp/remotebsp-can-${can_mode}-$$.log"
ledger_dir="$(mktemp -d "/tmp/remotebsp-ledger-${can_mode}-XXXXXX")"
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
    rm -rf -- "$ledger_dir"
    if [[ "$can_mode" == "usb-mock" ]]; then
        rm -f -- "$can_interface"
    fi
    exit "$result"
}
trap cleanup EXIT INT TERM

if [[ "$can_mode" != "usb-mock" ]] &&
   command -v candump >/dev/null 2>&1; then
    candump -L "$can_interface" >"$trace_log" 2>&1 &
    trace_pid=$!
fi
"$mock_mcu_bin" "$can_interface" "$can_mode" --uart-stream \
    >"$mock_log" 2>&1 &
mock_pid=$!
"$toolbusd_bin" "$can_interface" "$can_mode" "$socket_path" \
    --runtime-operation-ledger-dir "$ledger_dir" \
    >"$daemon_log" 2>&1 &
daemon_pid=$!

# GitHub 托管 Runner 的共享双核负载会显著拉长首次进程调度。这里采用
# 有界 10 秒就绪窗口，并在每次等待时确认两个进程仍然存活。
for _ in $(seq 1 500); do
    [[ -S "$socket_path" ]] && break
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done
[[ -S "$socket_path" ]]
[[ "$(stat -c '%a' "$socket_path")" == "660" ]]

ping_output=""
for _ in $(seq 1 500); do
    if ping_output="$("$remote_cli_bin" --socket "$socket_path" \
        ping "端到端测试" 2>/dev/null)"; then
        break
    fi
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done
grep -Fq 'pong=端到端测试' <<<"$ping_output"

runtime_snapshot_output="$("$remote_cli_bin" --json --socket \
    "$socket_path" runtime-snapshot 64 2000)"
python3 -c '
import json, sys
value = json.load(sys.stdin)
assert value["schema_version"] == 1
assert value["command"] == "runtime-snapshot"
data = value["data"]
assert data["snapshot_version"] == 3
assert data["snapshot_sequence"] > 0
assert len(data["nodes"]) == 1
assert len(data["resources"]) == 35
assert sum(item["descriptor"]["type"] == "adc"
           for item in data["resources"]) == 2
assert sum(item["descriptor"]["type"] == "storage"
           for item in data["resources"]) == 1
assert all(item["status_valid"] for item in data["resources"])
assert len(data["clocks"]) == 1
clock = data["clocks"][0]
assert clock["node_id"] == data["nodes"][0]["node_id"]
assert clock["state"] in {"unregistered", "unsynced", "synced", "degraded"}
if clock["registered"]:
    assert clock["boot_epoch"] > 0
    assert clock["model_generation"] > 0
else:
    assert clock["state"] == "unregistered"
    assert clock["boot_epoch"] is None
    assert clock["model_generation"] is None
if not clock["estimate_valid"]:
    assert clock["rate_deviation_ppb"] is None
    assert clock["error_bound_ns"] is None
' <<<"$runtime_snapshot_output"

health_snapshot_output="$("$remote_cli_bin" --json --socket \
    "$socket_path" health-snapshot)"
python3 -c '
import json, sys
value = json.load(sys.stdin)
assert value["schema_version"] == 1
assert value["command"] == "health-snapshot"
data = value["data"]
assert data["ipc_version"] == 2
assert len(data["daemon_instance_id"]) == 32
ipc = data["ipc"]
assert 0 <= ipc["active_clients"] <= ipc["peak_clients"] <= ipc["maximum_clients"]
assert ipc["accepted_total"] >= 1
assert ipc["capacity_rejected_total"] >= 0
assert ipc["oversized_frame_total"] >= 0
assert ipc["timeout_total"] >= 0
assert ipc["thread_creation_failed_total"] >= 0
health = data["health"]
assert health["contract_version"] == 1
assert health["source"] == 3
assert health["node_id"] == 0
assert health["producer_generation"] > 0
assert health["sample_sequence"] > 0
assert 1 <= len(health["metrics"]) <= 48
' <<<"$health_snapshot_output"

source_root="$(cd "$(dirname "$0")/.." && pwd)"
PYTHONPATH="$source_root" python3 \
    "$source_root/tests/health_runtime_process_e2e.py" \
    "$socket_path" "$remote_cli_bin"
# 完整快照压力结束后，以 Mock 的真实周期心跳重新确认节点在线；不使用
# 固定 sleep，也不绕过 provider/toolbusd 的在线性判定。
for _ in $(seq 1 100); do
    if "$remote_cli_bin" --socket "$socket_path" node-list 2>/dev/null | \
            grep -Fq 'node_id=1 online=1 ready=1'; then
        break
    fi
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.02
done
"$remote_cli_bin" --socket "$socket_path" node-list | \
    grep -Fq 'node_id=1 online=1 ready=1'
PYTHONPATH="$source_root" python3 \
    "$source_root/tests/pwm_runtime_process_e2e.py" \
    "$socket_path" "$remote_cli_bin"

# 2023 字节 PING 加 24 字节协议头仍位于 2048 字节最大包内，覆盖完整长包分片。
printf -v long_ping '%*s' 2023 ''
long_ping="${long_ping// /x}"
long_ping_output="$("$remote_cli_bin" --socket "$socket_path" \
    ping "$long_ping")"
[[ "$long_ping_output" == "pong=$long_ping" ]]

info_output="$("$remote_cli_bin" --socket "$socket_path" get-info)"
grep -Fq 'firmware=0.2.0' <<<"$info_output"
grep -Fq 'protocol_version=1' <<<"$info_output"

capability_output="$("$remote_cli_bin" --socket "$socket_path" get-capability)"
grep -Fq 'capabilities=0x17f3' <<<"$capability_output"

# 设备身份与校准参数使用独立持久化接口，不与资源清单混在一起。
parameter_status_output="$("$remote_cli_bin" --socket "$socket_path" \
    param-status)"
grep -Fq 'version=1 generation=0 stored=0 definitions=22' \
    <<<"$parameter_status_output"
parameter_list_output="$("$remote_cli_bin" --socket "$socket_path" \
    param-list)"
grep -Fq 'id=0x1 name=serial-number type=2' \
    <<<"$parameter_list_output"
parameter_write_output="$("$remote_cli_bin" --socket "$socket_path" \
    param-set serial-number MOCK-E2E-001)"
grep -Fq 'generation=1 stored=1' <<<"$parameter_write_output"
grep -Fq 'restart_required=yes' <<<"$parameter_write_output"
parameter_read_output="$("$remote_cli_bin" --socket "$socket_path" \
    param-get serial-number)"
grep -Fq 'generation=1 type=2 value=MOCK-E2E-001' \
    <<<"$parameter_read_output"
# CAS 入口不得用内部重新读取的新代数替换调用方看到的旧代数。
if "$remote_cli_bin" --socket "$socket_path" \
        param-set-cas serial-number 0 STALE-WRITE >/dev/null 2>&1; then
    echo "过期参数代数写入意外成功" >&2
    exit 1
fi
parameter_read_after_stale="$("$remote_cli_bin" --socket "$socket_path" \
    param-get serial-number)"
grep -Fq 'generation=1 type=2 value=MOCK-E2E-001' \
    <<<"$parameter_read_after_stale"
parameter_cas_output="$("$remote_cli_bin" --socket "$socket_path" \
    param-set-cas device-name 1 MOCK-CAS)"
grep -Fq 'generation=2 stored=2' <<<"$parameter_cas_output"

traffic_output="$("$remote_cli_bin" --socket "$socket_path" traffic-status)"
expected_traffic_mode="$can_mode"
if [[ "$can_mode" == "usb-mock" ]]; then
    expected_traffic_mode="usb"
fi
grep -Fq "mode=${expected_traffic_mode}" <<<"$traffic_output"
grep -Fq 'max_utilization_permille=700' <<<"$traffic_output"
grep -Fq 'class=system admitted_packets=' <<<"$traffic_output"
grep -Fq 'class=streaming admitted_packets=' <<<"$traffic_output"

event_output="$("$remote_cli_bin" --socket "$socket_path" event-wait)"
grep -Fq 'command=0x280 resource_id=0x2000007' <<<"$event_output"
grep -Fq 'data_hex=6d6f636b2d75617274370a' <<<"$event_output"

resource_output="$("$remote_cli_bin" --socket "$socket_path" resource-list)"
[[ "$(grep -c ' type=uart ' <<<"$resource_output")" -eq 8 ]]
grep -Fq 'resource_id=0x2000007 type=uart instance=7 source=expanded rx_capacity=4096 tx_capacity=4096' \
    <<<"$resource_output"
grep -Fq 'resource_id=0x6000000 type=pwm instance=0' <<<"$resource_output"
grep -Fq 'resource_id=0xa000000 type=timed-bitstream instance=0' <<<"$resource_output"

# Runtime 专用 GPIO 控制必须先在同一 toolbusd 实例登记租约；写请求本身
# 不能隐式创建租约。同一幂等键的重试不重复下发远端写命令。
daemon_identity_output="$("$remote_cli_bin" --socket "$socket_path" \
    daemon-identity)"
daemon_instance_id="${daemon_identity_output##*instance_id=}"
[[ "$daemon_instance_id" =~ ^[0-9a-f]{32}$ ]]
runtime_node_uuid="$($remote_cli_bin --socket "$socket_path" node-list |
    sed -n 's/.* uuid=\([0-9a-fA-F]\{32\}\).*/\1/p' | head -n 1)"
[[ "$runtime_node_uuid" =~ ^[0-9a-fA-F]{32}$ ]]
runtime_lease_id="11223344556677889900aabbccddeeff"
unknown_runtime_lease_id="ffeeddccbbaa00998877665544332211"
runtime_gpio_resource="0x1000005"
set +e
runtime_unknown_error="$($remote_cli_bin --socket "$socket_path" --node 1 \
    --json \
    runtime-gpio-write-operation "$daemon_instance_id" "$unknown_runtime_lease_id" \
    "$runtime_node_uuid" e2e-runtime "$runtime_gpio_resource" \
    unknown-before-acquire 1 \
    2>/dev/null)"
runtime_unknown_status=$?
set -e
if [[ "$runtime_unknown_status" -eq 0 ]]; then
    echo "未登记的 Runtime 控制租约不应允许 GPIO 写入" >&2
    exit 1
fi
grep -Fq '"command":"runtime-gpio-write-operation"' <<<"$runtime_unknown_error"
grep -Fq '"code":103' <<<"$runtime_unknown_error"
grep -Fq '"category":4' <<<"$runtime_unknown_error"
grep -Fq '"retryable":false' <<<"$runtime_unknown_error"
grep -Fq '"possibly_committed":false' <<<"$runtime_unknown_error"
"$remote_cli_bin" --socket "$socket_path" --node 1 \
    runtime-control-acquire "$daemon_instance_id" "$runtime_lease_id" \
    "$runtime_node_uuid" e2e-runtime "$runtime_gpio_resource" 5000 >/dev/null
runtime_gpio_first="$("$remote_cli_bin" --socket "$socket_path" --node 1 \
    --json runtime-gpio-write-operation "$daemon_instance_id" "$runtime_lease_id" \
    "$runtime_node_uuid" e2e-runtime "$runtime_gpio_resource" \
    gpio-e2e-command 1)"
grep -Fq '"state":"committed"' <<<"$runtime_gpio_first"
grep -Fq '"value":true' <<<"$runtime_gpio_first"
grep -Fq '"replayed":false' <<<"$runtime_gpio_first"
runtime_operation_id="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["operation_id"])' \
    <<<"$runtime_gpio_first")"
[[ "$runtime_operation_id" =~ ^[0-9a-f]{64}$ ]]
runtime_gpio_replay="$("$remote_cli_bin" --socket "$socket_path" --node 1 \
    --json runtime-gpio-write-operation "$daemon_instance_id" "$runtime_lease_id" \
    "$runtime_node_uuid" e2e-runtime "$runtime_gpio_resource" \
    gpio-e2e-command 1)"
grep -Fq '"state":"committed"' <<<"$runtime_gpio_replay"
grep -Fq '"replayed":true' <<<"$runtime_gpio_replay"
runtime_status="$("$remote_cli_bin" --socket "$socket_path" --json \
    runtime-operation-status "$daemon_instance_id" e2e-runtime \
    "$runtime_operation_id")"
grep -Fq '"state":"committed"' <<<"$runtime_status"
grep -Fq '"replayed":true' <<<"$runtime_status"
runtime_lookup="$("$remote_cli_bin" --socket "$socket_path" --json \
    runtime-operation-lookup "$daemon_instance_id" e2e-runtime gpio_write \
    "$runtime_lease_id" gpio-e2e-command)"
grep -Fq "\"operation_id\":\"$runtime_operation_id\"" <<<"$runtime_lookup"
set +e
runtime_release_error="$($remote_cli_bin --socket "$socket_path" --json \
    runtime-control-release-operation "$daemon_instance_id" "$runtime_lease_id" \
    other-runtime-owner 2>/dev/null)"
runtime_release_status=$?
set -e
[[ "$runtime_release_status" -ne 0 ]]
grep -Fq '"command":"runtime-control-release-operation"' <<<"$runtime_release_error"
grep -Fq '"code":101' <<<"$runtime_release_error"
grep -Fq '"category":3' <<<"$runtime_release_error"
grep -Fq '"possibly_committed":false' <<<"$runtime_release_error"
runtime_release="$("$remote_cli_bin" --socket "$socket_path" --node 1 --json \
    runtime-control-release-operation "$daemon_instance_id" \
    "$runtime_lease_id" e2e-runtime)"
grep -Fq '"kind":"control_release"' <<<"$runtime_release"
grep -Fq '"state":"committed"' <<<"$runtime_release"
grep -Fq '"recovery":"safe_closed"' <<<"$runtime_release"

pwm_create_output="$("$remote_cli_bin" --socket "$socket_path" \
    pwm-create 0 20000 4200 active-high)"
pwm_object_id="${pwm_create_output#object_id=}"
[[ "$pwm_object_id" =~ ^[1-9][0-9]*$ ]]
[[ "$("$remote_cli_bin" --socket "$socket_path" \
    pwm-write "$pwm_object_id" 7500)" == "ok" ]]
[[ "$("$remote_cli_bin" --socket "$socket_path" \
    pwm-stop "$pwm_object_id")" == "ok" ]]

ws2812_create_output="$("$remote_cli_bin" --socket "$socket_path" \
    ws2812-create 0)"
ws2812_object_id="${ws2812_create_output#object_id=}"
[[ "$ws2812_object_id" =~ ^[1-9][0-9]*$ ]]
[[ "$("$remote_cli_bin" --socket "$socket_path" \
    ws2812-write "$ws2812_object_id" FF000000FF000000FF)" == \
    "ok pixels=3" ]]

describe_output="$("$remote_cli_bin" --socket "$socket_path" \
    resource-describe 0x02000007)"
grep -Fq 'instance=7 source=expanded rx_capacity=4096 tx_capacity=4096' \
    <<<"$describe_output"

status_output="$("$remote_cli_bin" --socket "$socket_path" \
    resource-status 0x02000007)"
grep -Fq 'health=0 error_flags=0x0' <<<"$status_output"

contract_output="$("$remote_cli_bin" --socket "$socket_path" \
    resource-contract 0x0100000d)"
grep -Fq 'timing_resolution_ns=1000' <<<"$contract_output"
grep -Fq 'max_operations_per_second=10000' <<<"$contract_output"

lease_output="$("$remote_cli_bin" --socket "$socket_path" \
    resource-acquire 0x0100000d 5000 exclusive)"
lease_id="$(sed -n \
    's/.* lease_id=\(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
    <<<"$lease_output")"
[[ "$lease_id" =~ ^0x[0-9a-fA-F]+$ ]]
grep -Fq 'mode=exclusive' <<<"$lease_output"
grep -Fq 'active_count=1' <<<"$lease_output"

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

lease_status_output="$("$remote_cli_bin" --socket "$socket_path" \
    resource-lease-status 0x0100000d)"
grep -Fq 'lease_id=0x0' <<<"$lease_status_output"
grep -Fq 'active_count=1' <<<"$lease_status_output"
"$remote_cli_bin" --socket "$socket_path" \
    resource-release 0x0100000d "$lease_id" | grep -Fxq 'ok'

uart_create_output="$("$remote_cli_bin" --socket "$socket_path" \
    uart-create 0 115200 8 none 1)"
uart_object_id="${uart_create_output#object_id=}"
[[ "$uart_object_id" =~ ^[1-9][0-9]*$ ]]

uart_write_output="$("$remote_cli_bin" --socket "$socket_path" \
    uart-write "$uart_object_id" "UART-E2E")"
[[ "$uart_write_output" == "ok" ]]

uart_stream_create_output="$("$remote_cli_bin" --socket "$socket_path" \
    uart-create 3 115200 8 none 1 stream)"
uart_stream_object_id="${uart_stream_create_output#object_id=}"
[[ "$uart_stream_object_id" =~ ^[1-9][0-9]*$ ]]
uart_stream_output="$("$remote_cli_bin" --socket "$socket_path" \
    uart-stream-read "$uart_stream_object_id" 64 2000)"
grep -Fq 'data_hex=' <<<"$uart_stream_output"
grep -Fq 'dropped_bytes=0 lost_events=0' <<<"$uart_stream_output"

motion_lease_ids=()
for resource_id in 0x09000000 0x09000001 0x09000002; do
    motion_lease_output="$("$remote_cli_bin" --socket "$socket_path" \
        resource-acquire "$resource_id" 5000 exclusive)"
    motion_lease_id="$(sed -n \
        's/.* lease_id=\(0x[0-9a-fA-F][0-9a-fA-F]*\).*/\1/p' \
        <<<"$motion_lease_output")"
    [[ "$motion_lease_id" =~ ^0x[0-9a-fA-F]+$ ]]
    motion_lease_ids+=("$motion_lease_id")
done

motion_contract_output="$("$remote_cli_bin" --socket "$socket_path" \
    motion-contract)"
grep -Fq 'version=1 axes=3 queue_capacity=32' \
    <<<"$motion_contract_output"
grep -Fq 'maximum_total_step_rate_hz=200000' \
    <<<"$motion_contract_output"
grep -Fq 'axis=0x9000000 maximum_step_rate_hz=100000' \
    <<<"$motion_contract_output"

motion_enqueue_output="$("$remote_cli_bin" --socket "$socket_path" \
    motion-enqueue 1 auto 1000000 final \
    0x09000000:3 0x09000001:-2 0x09000002:0)"
grep -Fq 'sequence=1' <<<"$motion_enqueue_output"
grep -Fq 'duration_ns=1000000 final=1 axes=3' \
    <<<"$motion_enqueue_output"
motion_status_output=""
for _ in $(seq 1 100); do
    motion_status_output="$("$remote_cli_bin" --socket "$socket_path" \
        motion-status)"
    if grep -Fq 'last_completed=1' <<<"$motion_status_output"; then
        break
    fi
    kill -0 "$mock_pid"
    kill -0 "$daemon_pid"
    sleep 0.01
done
grep -Fq 'state=idle fault=none' <<<"$motion_status_output"
grep -Fq 'last_completed=1' <<<"$motion_status_output"
grep -Fq 'axis=0x9000000' <<<"$motion_status_output"
grep -Fq 'position_steps=3 emitted_steps=3' <<<"$motion_status_output"
grep -Fq 'axis=0x9000001' <<<"$motion_status_output"
grep -Fq 'position_steps=-2 emitted_steps=2' <<<"$motion_status_output"

"$client_api_bin" "$socket_path"

if [[ "$can_mode" == "fd" && -n "$trace_pid" ]]; then
    # candump 的 CAN-FD 标志半字节中 0x1 为 BRS、0x4 为 FDF。
    grep -Eq '##5[0-9A-F]' "$trace_log"
fi

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
