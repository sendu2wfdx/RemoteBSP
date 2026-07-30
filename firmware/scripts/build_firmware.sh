#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="${1:-all}"

build_one() {
    local board="$1"
    local config="$2"
    local artifact="$3"
    local output_artifact="${4:-${artifact}}"
    local build_dir="${root_dir}/build-${board}"

    cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${root_dir}/cmake/arm-none-eabi-toolchain.cmake" \
        -DRBSP_CONFIG="${root_dir}/${config}"
    cmake --build "${build_dir}"
    cp "${build_dir}/${artifact}.elf" \
        "${root_dir}/out/${output_artifact}.elf"
    cp "${build_dir}/${artifact}.hex" \
        "${root_dir}/out/${output_artifact}.hex"
    cp "${build_dir}/${artifact}.bin" \
        "${root_dir}/out/${output_artifact}.bin"
    cp "${build_dir}/${artifact}.map" \
        "${root_dir}/out/${output_artifact}.map"
}

case "${target}" in
    f103)
        build_one f103 configs/stm32f103cbt6_defconfig \
            remotebsp-stm32f103cbt6
        ;;
    bluepill)
        build_one f103-bluepill configs/stm32f103_bluepill_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-pb8-pb9
        ;;
    g431)
        build_one g431 configs/stm32g431cbu6_defconfig \
            remotebsp-stm32g431cbu6
        ;;
    bluepill-katapult)
        build_one f103-bluepill-katapult \
            configs/stm32f103_bluepill_katapult_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-katapult
        ;;
    g431-katapult)
        build_one g431-katapult \
            configs/stm32g431cbu6_katapult_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-katapult
        ;;
    all)
        build_one f103 configs/stm32f103cbt6_defconfig \
            remotebsp-stm32f103cbt6
        build_one f103-bluepill configs/stm32f103_bluepill_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-pb8-pb9
        build_one g431 configs/stm32g431cbu6_defconfig \
            remotebsp-stm32g431cbu6
        ;;
    *)
        printf '用法：%s [f103|bluepill|g431|bluepill-katapult|g431-katapult|all]\n' \
            "$0" >&2
        exit 2
        ;;
esac
