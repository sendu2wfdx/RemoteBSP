#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="${1:-all}"
build_jobs="${RBSP_BUILD_JOBS:-32}"

build_one() {
    local board="$1"
    local config="$2"
    local artifact="$3"
    local output_artifact="${4:-${artifact}}"
    local build_dir="${root_dir}/build-${board}"

    cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${root_dir}/cmake/arm-none-eabi-toolchain.cmake" \
        -DRBSP_CONFIG="${root_dir}/${config}"
    cmake --build "${build_dir}" --parallel "${build_jobs}"
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
    f072)
        build_one f072 configs/stm32f072rbt6_defconfig \
            remotebsp-stm32f072rbt6
        ;;
    f072-pb)
        build_one f072-pb configs/stm32f072rbt6_pb8_pb9_defconfig \
            remotebsp-stm32f072rbt6 \
            remotebsp-stm32f072rbt6-pb8-pb9
        ;;
    f103)
        build_one f103 configs/stm32f103cbt6_defconfig \
            remotebsp-stm32f103cbt6
        ;;
    f103-motion)
        build_one f103-motion \
            configs/stm32f103cbt6_motion_5axis_tmc2209_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-motion-5axis-tmc2209
        ;;
    bluepill|weact-bluepill-plus)
        build_one f103-bluepill configs/stm32f103_bluepill_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-pb8-pb9
        ;;
    bluepill-motion|weact-bluepill-plus-motion)
        build_one f103-bluepill-motion \
            configs/stm32f103_bluepill_motion_5axis_tmc2209_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-motion-5axis-tmc2209
        ;;
    fly-d5|mellow-fly-d5)
        build_one f072-fly-d5 configs/stm32f072_fly_d5_defconfig \
            remotebsp-stm32f072rbt6 \
            remotebsp-stm32f072-fly-d5
        ;;
    fly-d5-katapult|mellow-fly-d5-katapult)
        build_one f072-fly-d5-katapult \
            configs/stm32f072_fly_d5_katapult_defconfig \
            remotebsp-stm32f072rbt6 \
            remotebsp-stm32f072-fly-d5-katapult
        ;;
    g431|weact-stm32g431cbu6-core)
        build_one g431 configs/stm32g431cbu6_defconfig \
            remotebsp-stm32g431cbu6
        ;;
    g431-motion|weact-stm32g431cbu6-core-motion)
        build_one g431-motion \
            configs/stm32g431cbu6_motion_2axis_tmc2209_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-motion-2axis-tmc2209
        ;;
    g431-motion-5axis|weact-stm32g431cbu6-core-motion-5axis)
        build_one g431-motion-5axis \
            configs/stm32g431cbu6_motion_5axis_tmc2209_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-motion-5axis-tmc2209
        ;;
    bluepill-katapult|weact-bluepill-plus-katapult)
        build_one f103-bluepill-katapult \
            configs/stm32f103_bluepill_katapult_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-katapult
        ;;
    g431-katapult|weact-stm32g431cbu6-core-katapult)
        build_one g431-katapult \
            configs/stm32g431cbu6_katapult_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-katapult
        ;;
    all)
        build_one f072 configs/stm32f072rbt6_defconfig \
            remotebsp-stm32f072rbt6
        build_one f103 configs/stm32f103cbt6_defconfig \
            remotebsp-stm32f103cbt6
        build_one f103-bluepill configs/stm32f103_bluepill_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-pb8-pb9
        build_one f072-fly-d5 configs/stm32f072_fly_d5_defconfig \
            remotebsp-stm32f072rbt6 \
            remotebsp-stm32f072-fly-d5
        build_one g431 configs/stm32g431cbu6_defconfig \
            remotebsp-stm32g431cbu6
        ;;
    *)
        printf '用法：%s [f072|f072-pb|f103|f103-motion|mellow-fly-d5|mellow-fly-d5-katapult|weact-bluepill-plus|weact-bluepill-plus-motion|weact-bluepill-plus-katapult|weact-stm32g431cbu6-core|weact-stm32g431cbu6-core-motion|weact-stm32g431cbu6-core-motion-5axis|weact-stm32g431cbu6-core-katapult|all]\n' \
            "$0" >&2
        exit 2
        ;;
esac
