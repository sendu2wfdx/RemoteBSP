#!/usr/bin/env bash
set -euo pipefail

# 对真实 toolbusd/MCU 链路执行顺序请求压力与 GPIO 往返一致性测试。
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cli="${root_dir}/build-wsl/remote-cli"
socket_path="${1:-/tmp/toolbusd.sock}"
node_id="${2:-1}"
ping_count="${3:-200}"
gpio_cycles="${4:-50}"
existing_gpio_object="${5:-0}"
concurrent_workers="${6:-0}"
requests_per_worker="${7:-25}"
large_payload_size="${8:-0}"

if [[ ! -x "${cli}" ]]; then
    printf '找不到可执行程序：%s\n' "${cli}" >&2
    exit 1
fi
for value in "${node_id}" "${ping_count}" "${gpio_cycles}" \
             "${concurrent_workers}" "${requests_per_worker}"; do
    if ! [[ "${value}" =~ ^[0-9]+$ ]]; then
        printf '节点 ID、PING 次数和 GPIO 周期必须是非负整数\n' >&2
        exit 2
    fi
done
if ! [[ "${large_payload_size}" =~ ^[0-9]+$ ]] ||
    ((large_payload_size > 2024)); then
    printf '大载荷 PING 字节数必须在 0..2024 之间\n' >&2
    exit 2
fi
if [[ "${existing_gpio_object}" == "0" ]]; then
    existing_gpio_object=""
fi
if [[ -n "${existing_gpio_object}" ]] &&
    ! [[ "${existing_gpio_object}" =~ ^[1-9][0-9]*$ ]]; then
    printf '已有 GPIO 对象 ID 必须是正整数\n' >&2
    exit 2
fi
if ((node_id == 0 || ping_count == 0)); then
    printf '节点 ID 和 PING 次数必须大于零\n' >&2
    exit 2
fi

client=("${cli}" --socket "${socket_path}" --node "${node_id}")
declare -a latencies_ms=()
ping_failures=0

coproc MONOTONIC_CLOCK {
    python3 -u -c \
        'import sys, time
for _ in sys.stdin:
    print(time.monotonic_ns(), flush=True)'
}

read_monotonic_ns() {
    printf '\n' >&"${MONOTONIC_CLOCK[1]}"
    IFS= read -r monotonic_ns_value <&"${MONOTONIC_CLOCK[0]}"
}

for ((index = 1; index <= ping_count; ++index)); do
    payload="stress-${index}"
    read_monotonic_ns
    started_ns="${monotonic_ns_value}"
    if output="$("${client[@]}" ping "${payload}" 2>&1)" &&
        [[ "${output}" == "pong=${payload}" ]]; then
        read_monotonic_ns
        finished_ns="${monotonic_ns_value}"
        latencies_ms+=("$(((finished_ns - started_ns) / 1000000))")
    else
        ((ping_failures += 1))
        printf 'PING %u 失败：%s\n' "${index}" "${output}" >&2
    fi
done

if ((${#latencies_ms[@]} == 0)); then
    printf '全部 PING 请求失败\n' >&2
    exit 1
fi

mapfile -t sorted_ms < <(printf '%s\n' "${latencies_ms[@]}" | sort -n)
sum_ms=0
for latency in "${latencies_ms[@]}"; do
    ((sum_ms += latency))
done
p95_index="$(((${#sorted_ms[@]} * 95 + 99) / 100 - 1))"

printf 'PING：成功=%u 失败=%u 成功率=%u/%u\n' \
    "${#latencies_ms[@]}" "${ping_failures}" \
    "${#latencies_ms[@]}" "${ping_count}"
printf '延迟(ms)：min=%u avg=%u p95=%u max=%u\n' \
    "${sorted_ms[0]}" "$((sum_ms / ${#latencies_ms[@]}))" \
    "${sorted_ms[p95_index]}" \
    "${sorted_ms[$((${#sorted_ms[@]} - 1))]}"

gpio_failures=0
if ((gpio_cycles > 0)); then
    if [[ -n "${existing_gpio_object}" ]]; then
        object_id="${existing_gpio_object}"
    else
        create_output="$("${client[@]}" gpio-create 45 output 1)"
        if [[ ! "${create_output}" =~ ^object_id=([0-9]+)$ ]]; then
            printf '创建 PC13 GPIO 失败：%s\n' "${create_output}" >&2
            exit 1
        fi
        object_id="${BASH_REMATCH[1]}"
        printf '已创建 PC13 GPIO：object_id=%u\n' "${object_id}"
    fi

    for ((index = 0; index < gpio_cycles; ++index)); do
        expected="$((index % 2))"
        write_ok=1
        if ! "${client[@]}" gpio-write "${object_id}" "${expected}" \
                >/dev/null; then
            write_ok=0
        fi
        read_output="$("${client[@]}" gpio-read "${object_id}" 2>&1)" ||
            read_output="读取失败：${read_output}"
        if ((write_ok == 0)) ||
            [[ "${read_output}" != "value=${expected}" ]]; then
            ((gpio_failures += 1))
            printf 'GPIO 周期 %u 失败，期望值=%u，读取=%s\n' \
                "${index}" "${expected}" "${read_output}" >&2
        fi
    done

    # Bluepill 的 PC13 LED 低电平点亮，测试结束恢复高电平熄灭。
    "${client[@]}" gpio-write "${object_id}" 1 >/dev/null
fi

printf 'GPIO：周期=%u 失败=%u\n' "${gpio_cycles}" "${gpio_failures}"

concurrent_failures=0
if ((concurrent_workers > 0)); then
    run_worker() {
        local worker="$1"
        local request payload output
        for ((request = 1; request <= requests_per_worker; ++request)); do
            payload="worker-${worker}-${request}"
            if ! output="$("${client[@]}" ping "${payload}" 2>&1)" ||
                [[ "${output}" != "pong=${payload}" ]]; then
                printf '并发客户端 %u 请求 %u 失败：%s\n' \
                    "${worker}" "${request}" "${output}" >&2
                return 1
            fi
        done
    }

    declare -a worker_pids=()
    for ((worker = 1; worker <= concurrent_workers; ++worker)); do
        run_worker "${worker}" &
        worker_pids+=("$!")
    done
    for worker_pid in "${worker_pids[@]}"; do
        if ! wait "${worker_pid}"; then
            ((concurrent_failures += 1))
        fi
    done
fi
printf '并发：客户端=%u 每客户端请求=%u 失败客户端=%u\n' \
    "${concurrent_workers}" "${requests_per_worker}" \
    "${concurrent_failures}"

large_payload_failure=0
if ((large_payload_size > 0)); then
    printf -v large_payload '%*s' "${large_payload_size}" ''
    large_payload="${large_payload// /X}"
    read_monotonic_ns
    started_ns="${monotonic_ns_value}"
    if large_output="$("${client[@]}" ping "${large_payload}" 2>&1)" &&
        [[ "${large_output}" == "pong=${large_payload}" ]]; then
        read_monotonic_ns
        finished_ns="${monotonic_ns_value}"
        printf '大载荷 PING：字节=%u 往返延迟=%u ms 结果=成功\n' \
            "${large_payload_size}" \
            "$(((finished_ns - started_ns) / 1000000))"
    else
        large_payload_failure=1
        printf '大载荷 PING：字节=%u 结果=失败，详情=%s\n' \
            "${large_payload_size}" "${large_output}" >&2
    fi
fi

"${cli}" --socket "${socket_path}" node-list

if ((ping_failures != 0 || gpio_failures != 0 ||
      concurrent_failures != 0 || large_payload_failure != 0)); then
    exit 1
fi
