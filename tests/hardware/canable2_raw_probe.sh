#!/usr/bin/env bash
set -euo pipefail

# 直接使用 CANable2 原厂 SLCAN 命令进行物理链路诊断。
# 运行前必须停止占用该串口的 slcand。
device="${1:-}"
frame_count="${2:-1}"
if [[ -z "${device}" || ! -e "${device}" ]]; then
    printf '用法：%s <CANable2 串口设备> [测试帧数量]\n' "$0" >&2
    exit 2
fi
if ! [[ "${frame_count}" =~ ^[1-9][0-9]*$ ]] ||
    ((frame_count > 100)); then
    printf '测试帧数量必须在 1..100 之间\n' >&2
    exit 2
fi

stty -F "${device}" 115200 raw -echo
exec 3<>"${device}"

# 关闭通道、设置经典 CAN 500k、正常模式、自动重发，再打开通道。
printf 'C\rS6\rM0\rA1\rO\r' >&3
sleep 1

# 查询固件版本，发送一个测试帧，然后读取 CANable2 错误寄存器。
printf 'V\r' >&3
sleep 1
for ((index = 0; index < frame_count; ++index)); do
    printf 't12380102030405060708\r' >&3
    sleep 0.01
done
sleep 1
printf 'E\r' >&3

# 固件的 V/E 响应没有固定长度，用短超时收集当前全部输出。
timeout 2 dd bs=1 count=256 <&3 2>/dev/null || true
printf '\n'
