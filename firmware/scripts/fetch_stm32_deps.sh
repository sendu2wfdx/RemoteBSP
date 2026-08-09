#!/usr/bin/env bash
set -euo pipefail

# 仅下载 ST 官方 MCU 支持组件，不包含中间件、USB 或板级示例。
# 每个版本均与对应 STM32Cube 系列版本配套并固定到发布标签。
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
vendor_dir="${root_dir}/vendor"
mkdir -p "${vendor_dir}"

clone_component() {
    local name="$1"
    local tag="$2"
    local url="$3"
    local target="${vendor_dir}/${name}"

    if [[ -d "${target}/.git" ]]; then
        printf '%s 已存在，保留现有目录\n' "${name}"
        return
    fi
    if [[ -e "${target}" ]]; then
        printf '错误：%s 已存在但不是 Git 仓库，请人工确认\n' "${target}" >&2
        exit 1
    fi
    git clone --depth 1 --branch "${tag}" "${url}" "${target}"
}

clone_component cmsis-core v5.9.0 \
    https://github.com/STMicroelectronics/cmsis-core.git
clone_component cmsis-device-f1 v4.3.5 \
    https://github.com/STMicroelectronics/cmsis-device-f1.git
clone_component stm32f1xx-hal-driver v1.1.10 \
    https://github.com/STMicroelectronics/stm32f1xx-hal-driver.git
clone_component cmsis-device-f0 v2.3.7 \
    https://github.com/STMicroelectronics/cmsis-device-f0.git
clone_component stm32f0xx-hal-driver v1.7.8 \
    https://github.com/STMicroelectronics/stm32f0xx-hal-driver.git
clone_component cmsis-device-g4 v1.2.6 \
    https://github.com/STMicroelectronics/cmsis-device-g4.git
clone_component stm32g4xx-hal-driver v1.2.6 \
    https://github.com/STMicroelectronics/stm32g4xx-hal-driver.git
printf 'STM32 官方依赖准备完成：%s\n' "${vendor_dir}"
