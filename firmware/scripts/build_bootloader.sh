#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
katapult_dir="${root_dir}/vendor/katapult"
output_dir="${root_dir}/out"
target="${1:-all}"

bash "${root_dir}/scripts/fetch_katapult.sh"

build_one() {
    local profile="$1"
    local config="${root_dir}/bootloader/configs/katapult_${profile}.config"
    local katapult_output="out-${profile}/"

    if [[ ! -f "${config}" ]]; then
        printf '找不到 Bootloader 配置：%s\n' "${config}" >&2
        exit 1
    fi

    cp "${config}" "${katapult_dir}/.config"
    make -C "${katapult_dir}" OUT="${katapult_output}" olddefconfig
    make -C "${katapult_dir}" OUT="${katapult_output}" \
        -j"$(nproc)"

    cp "${katapult_dir}/${katapult_output}katapult.bin" \
        "${output_dir}/katapult-${profile}.bin"
    cp "${katapult_dir}/${katapult_output}katapult.elf" \
        "${output_dir}/katapult-${profile}.elf"
    cp "${katapult_dir}/.config" \
        "${output_dir}/katapult-${profile}.config"

    printf '已生成：%s\n' "${output_dir}/katapult-${profile}.bin"
}

case "${target}" in
    stm32f103_dual|stm32g431_dual|stm32f103_can|stm32f103_usb|stm32g431_can|stm32g431_usb)
        build_one "${target}"
        ;;
    all)
        build_one stm32f103_dual
        build_one stm32g431_dual
        ;;
    *)
        printf '用法：%s [stm32f103_dual|stm32g431_dual|stm32f103_can|stm32f103_usb|stm32g431_can|stm32g431_usb|all]\n' \
            "$0" >&2
        exit 2
        ;;
esac
