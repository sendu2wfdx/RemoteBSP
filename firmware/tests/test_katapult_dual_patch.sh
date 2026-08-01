#!/usr/bin/env bash
set -euo pipefail

# 从固定的上游提交复制一份干净源码，证明仓库补丁不依赖当前 vendor 工作区。
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_dir="${root_dir}/vendor/katapult"
patch_file="${root_dir}/bootloader/patches/katapult-dual-can-usb.patch"
temporary_dir="$(mktemp -d "${TMPDIR:-/tmp}/remotebsp-katapult.XXXXXX")"

git clone --quiet --no-hardlinks "${source_dir}" "${temporary_dir}"
git -C "${temporary_dir}" checkout --quiet --detach \
    ec59b9bb9ad6c2ec8d4dc6831fbc77f0b308e29e
git -C "${temporary_dir}" apply "${patch_file}"

build_profile() {
    local profile="$1"
    local output="out-${profile}/"
    cp "${root_dir}/bootloader/configs/katapult_${profile}.config" \
        "${temporary_dir}/.config"
    make -C "${temporary_dir}" OUT="${output}" olddefconfig >/dev/null
    make -C "${temporary_dir}" OUT="${output}" -j"$(nproc)" >/dev/null

    local image="${temporary_dir}/${output}katapult.bin"
    local size
    size="$(wc -c < "${image}")"
    if (( size > 0x2000 )); then
        printf '%s 超过 8 KiB：%s 字节\n' "${profile}" "${size}" >&2
        exit 1
    fi
    grep -q '^CONFIG_DUAL_CAN_USB=y$' "${temporary_dir}/.config"
    grep -q '^CONFIG_USBSERIAL=y$' "${temporary_dir}/.config"
    grep -q '^CONFIG_CANSERIAL=y$' "${temporary_dir}/.config"
    grep -q '^CONFIG_STM32_CANBUS_PB8_PB9=y$' "${temporary_dir}/.config"
    if [[ "${profile}" == "stm32f072_dual" ]]; then
        grep -q '^# CONFIG_ENABLE_BUTTON is not set$' \
            "${temporary_dir}/.config"
        grep -q '^CONFIG_ENABLE_DOUBLE_RESET=y$' \
            "${temporary_dir}/.config"
    elif [[ "${profile}" == "stm32f103_dual" ]]; then
        grep -q '^CONFIG_ENABLE_BUTTON=y$' "${temporary_dir}/.config"
        grep -q '^CONFIG_BUTTON_PIN="~PA0"$' "${temporary_dir}/.config"
        grep -q '^# CONFIG_ENABLE_DOUBLE_RESET is not set$' \
            "${temporary_dir}/.config"
    else
        grep -q '^CONFIG_ENABLE_BUTTON=y$' "${temporary_dir}/.config"
        grep -q '^CONFIG_BUTTON_PIN="~PC13"$' "${temporary_dir}/.config"
        grep -q '^# CONFIG_ENABLE_DOUBLE_RESET is not set$' \
            "${temporary_dir}/.config"
    fi

    local object_dir="${temporary_dir}/${output}src"
    arm-none-eabi-nm "${object_dir}/generic/dual_interface.o" |
        grep ' T console_sendf$' >/dev/null
    arm-none-eabi-nm "${object_dir}/generic/canserial.o" |
        grep ' T can_console_sendf$' >/dev/null
    arm-none-eabi-nm "${object_dir}/generic/usb_cdc.o" |
        grep ' T usb_console_sendf$' >/dev/null
    arm-none-eabi-nm "${object_dir}/bootentry.o" |
        grep ' T bootentry_is_usb$' >/dev/null
    if [[ "${profile}" == "stm32f103_dual" ]]; then
        arm-none-eabi-nm "${object_dir}/generic/dual_interface.o" |
            grep ' T dual_usb_can_irq$' >/dev/null
    fi
    printf '%s：%s 字节，双模式配置有效\n' "${profile}" "${size}"
}

build_profile stm32f072_dual
build_profile stm32f103_dual
build_profile stm32g431_dual
printf '临时验证目录保留在：%s\n' "${temporary_dir}"
