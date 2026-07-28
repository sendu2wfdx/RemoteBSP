#!/usr/bin/env bash
set -euo pipefail

# 为 CANable2 原厂 SLCAN 固件建立经典 CAN SocketCAN 接口。
# 默认使用稳定的 /dev/serial/by-id 路径，避免 ttyACM 编号变化。
interface="${1:-can0}"
bitrate_preset="${2:-6}"
tx_queue_length="${3:-1024}"

if ! [[ "${bitrate_preset}" =~ ^[0-9]$ ]]; then
    printf 'SLCAN 波特率预设必须是 0..9\n' >&2
    exit 2
fi
if ! [[ "${tx_queue_length}" =~ ^[1-9][0-9]*$ ]]; then
    printf '发送队列长度必须是正整数\n' >&2
    exit 2
fi

mapfile -t devices < <(
    find /dev/serial/by-id -maxdepth 1 -type l \
        -name 'usb-Openlight_Labs_CANable2_*' 2>/dev/null
)
if ((${#devices[@]} != 1)); then
    printf '需要且只能连接一个 CANable2，当前找到 %u 个\n' \
        "${#devices[@]}" >&2
    exit 1
fi
if ip link show "${interface}" >/dev/null 2>&1; then
    printf '接口 %s 已存在，请勿重复启动\n' "${interface}" >&2
    exit 1
fi

modprobe can
modprobe can_raw
modprobe can_dev
modprobe slcan
slcand -o -c "-s${bitrate_preset}" "${devices[0]}" "${interface}"

for _ in {1..20}; do
    if ip link show "${interface}" >/dev/null 2>&1; then
        break
    fi
    sleep 0.05
done
if ! ip link show "${interface}" >/dev/null 2>&1; then
    printf 'slcand 未能创建接口 %s\n' "${interface}" >&2
    exit 1
fi

# 经典 CAN 每片只有 3 字节协议载荷，2048 字节远程包最多约 683 片。
# slcan 默认队列只有 10，必须在接口启动前调大，避免静默丢失尾片。
ip link set "${interface}" txqueuelen "${tx_queue_length}"
ip link set "${interface}" up
ip -details -statistics link show "${interface}"
