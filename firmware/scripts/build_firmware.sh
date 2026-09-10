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
    local build_dir="${root_dir}/build/${board}"

    cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${root_dir}/cmake/arm-none-eabi-toolchain.cmake" \
        -DRBSP_CONFIG="${root_dir}/${config}"
    cmake --build "${build_dir}" --parallel "${build_jobs}"
    python3 "${root_dir}/scripts/verify_flash_layout.py" \
        --config "${root_dir}/${config}" \
        --elf "${build_dir}/${artifact}.elf" \
        --map "${build_dir}/${artifact}.map"
    cp "${build_dir}/${artifact}.elf" \
        "${root_dir}/out/${output_artifact}.elf"
    cp "${build_dir}/${artifact}.hex" \
        "${root_dir}/out/${output_artifact}.hex"
    cp "${build_dir}/${artifact}.bin" \
        "${root_dir}/out/${output_artifact}.bin"
    cp "${build_dir}/${artifact}.map" \
        "${root_dir}/out/${output_artifact}.map"
}

build_studio_identity() {
    local input_dir="${root_dir}/build/ci-studio-input"
    local input_sha="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    python3 "${root_dir}/scripts/generate_ci_studio_fixture.py" \
        --output "${input_dir}" \
        --firmware-input-sha256 "${input_sha}"
    local build_dir="${root_dir}/build/g431-weact-core-studio-identity"
    cmake -S "${root_dir}" -B "${build_dir}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${root_dir}/cmake/arm-none-eabi-toolchain.cmake" \
        -DRBSP_CONFIG="${input_dir}/firmware.config" \
        -DRBSP_STATIC_RESOURCE_TABLE="${input_dir}/remotebsp_static_resources.h" \
        -DRBSP_FIRMWARE_INPUT_SHA256="${input_sha}"
    cmake --build "${build_dir}" --parallel "${build_jobs}"
    python3 "${root_dir}/scripts/verify_flash_layout.py" \
        --config "${input_dir}/firmware.config" \
        --elf "${build_dir}/remotebsp-stm32g431cbu6.elf" \
        --map "${build_dir}/remotebsp-stm32g431cbu6.map"
    for suffix in elf hex bin map; do
        cp "${build_dir}/remotebsp-stm32g431cbu6.${suffix}" \
            "${root_dir}/out/remotebsp-stm32g431-weact-core-studio-identity.${suffix}"
    done
    cp "${input_dir}/identity.json" \
        "${root_dir}/out/remotebsp-stm32g431-weact-core-studio-identity.identity.json"
}

case "${target}" in
    f072)
        build_one f072 configs/stm32f072rbt6_defconfig \
            remotebsp-stm32f072rbt6
        ;;
    f103)
        build_one f103 configs/stm32f103cbt6_defconfig \
            remotebsp-stm32f103cbt6
        ;;
    bluepill|weact-bluepill-plus)
        build_one f103-bluepill \
            configs/stm32f103_weact_bluepill_plus_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-pb8-pb9
        ;;
    bluepill-motion|weact-bluepill-plus-motion)
        build_one f103-bluepill-motion \
            tests/configs/stm32f103_weact_bluepill_plus_motion_5axis_tmc2209_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-motion-5axis-tmc2209
        ;;
    fly-d5|mellow-fly-d5)
        build_one f072-fly-d5 \
            configs/stm32f072_mellow_fly_d5_defconfig \
            remotebsp-stm32f072rbt6 \
            remotebsp-stm32f072-fly-d5
        ;;
    fly-d5-motion|mellow-fly-d5-motion)
        build_one f072-fly-d5-runtime-motion \
            tests/configs/stm32f072_mellow_fly_d5_runtime_motion_5axis_tmc2209_defconfig \
            remotebsp-stm32f072rbt6 \
            remotebsp-stm32f072-fly-d5-runtime-motion-5axis-tmc2209
        ;;
    fly-d5-katapult|mellow-fly-d5-katapult)
        build_one f072-fly-d5-katapult \
            configs/stm32f072_mellow_fly_d5_katapult_defconfig \
            remotebsp-stm32f072rbt6 \
            remotebsp-stm32f072-fly-d5-katapult
        ;;
    g431)
        build_one g431 configs/stm32g431cbu6_defconfig \
            remotebsp-stm32g431cbu6
        ;;
    weact-stm32g431cbu6-core)
        build_one g431-weact-core \
            configs/stm32g431_weact_core_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-weact-core
        ;;
    weact-stm32g431cbu6-core-dual-pwm)
        build_one g431-weact-core-dual-pwm \
            tests/configs/stm32g431_weact_core_dual_pwm_pb10_pb11_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-weact-core-dual-pwm
        ;;
    weact-stm32g431cbu6-core-bus-hal)
        build_one g431-weact-core-bus-hal \
            tests/configs/stm32g431_weact_core_bus_hal_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-weact-core-bus-hal
        ;;
    stm32f072-bus-hal)
        build_one f072-bus-hal tests/configs/stm32f072_bus_hal_defconfig \
            remotebsp-stm32f072rbt6
        ;;
    stm32f103-bus-hal)
        build_one f103-bus-hal tests/configs/stm32f103_bus_hal_defconfig \
            remotebsp-stm32f103cbt6
        ;;
    stm32-adc-core)
        build_one f072-adc-core tests/configs/stm32f072_adc_core_defconfig \
            remotebsp-stm32f072rbt6 remotebsp-stm32f072-adc-core
        build_one f103-adc-core tests/configs/stm32f103_adc_core_defconfig \
            remotebsp-stm32f103cbt6 remotebsp-stm32f103-adc-core
        build_one g431-adc-core tests/configs/stm32g431_adc_core_defconfig \
            remotebsp-stm32g431cbu6 remotebsp-stm32g431-adc-core
        ;;
    weact-stm32g431cbu6-core-studio-identity)
        build_studio_identity
        ;;
    weact-stm32g431cbu6-core-usb|g431-usb)
        build_one g431-weact-core-usb \
            configs/stm32g431_weact_core_usb_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-weact-core-usb
        ;;
    weact-stm32g431cbu6-core-motion)
        build_one g431-weact-core-motion \
            tests/configs/stm32g431_weact_core_motion_1axis_tmc2209_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-weact-core-motion-1axis-tmc2209
        ;;
    bluepill-katapult|weact-bluepill-plus-katapult)
        build_one f103-bluepill-katapult \
            configs/stm32f103_weact_bluepill_plus_katapult_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-katapult
        ;;
    g431-katapult|weact-stm32g431cbu6-core-katapult)
        build_one g431-katapult \
            configs/stm32g431_weact_core_katapult_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-katapult
        ;;
    all)
        build_one f072 configs/stm32f072rbt6_defconfig \
            remotebsp-stm32f072rbt6
        build_one f103 configs/stm32f103cbt6_defconfig \
            remotebsp-stm32f103cbt6
        build_one f103-bluepill \
            configs/stm32f103_weact_bluepill_plus_defconfig \
            remotebsp-stm32f103cbt6 \
            remotebsp-stm32f103-bluepill-pb8-pb9
        build_one f072-fly-d5 \
            configs/stm32f072_mellow_fly_d5_defconfig \
            remotebsp-stm32f072rbt6 \
            remotebsp-stm32f072-fly-d5
        build_one g431 configs/stm32g431cbu6_defconfig \
            remotebsp-stm32g431cbu6
        build_one g431-weact-core \
            configs/stm32g431_weact_core_defconfig \
            remotebsp-stm32g431cbu6 \
            remotebsp-stm32g431-weact-core
        ;;
    ci)
        # Git 默认不会保留 Windows 工作区脚本的可执行位；始终显式交给 bash，
        # 保证本地与 GitHub 干净检出行为一致。
        bash "$0" all
        bash "$0" weact-stm32g431cbu6-core-bus-hal
        bash "$0" stm32f072-bus-hal
        bash "$0" stm32f103-bus-hal
        bash "$0" stm32-adc-core
        bash "$0" mellow-fly-d5-motion
        bash "$0" weact-bluepill-plus-motion
        bash "$0" weact-stm32g431cbu6-core-motion
        bash "$0" mellow-fly-d5-katapult
        bash "$0" weact-bluepill-plus-katapult
        bash "$0" weact-stm32g431cbu6-core-katapult
        bash "$0" weact-stm32g431cbu6-core-usb
        bash "$0" weact-stm32g431cbu6-core-studio-identity
        python3 "${root_dir}/scripts/verify_ci_firmware_matrix.py"
        ;;
    *)
        printf '用法：%s [f072|f103|g431|stm32f072-bus-hal|stm32f103-bus-hal|mellow-fly-d5|mellow-fly-d5-motion|mellow-fly-d5-katapult|weact-bluepill-plus|weact-bluepill-plus-motion|weact-bluepill-plus-katapult|weact-stm32g431cbu6-core|weact-stm32g431cbu6-core-dual-pwm|weact-stm32g431cbu6-core-bus-hal|weact-stm32g431cbu6-core-studio-identity|weact-stm32g431cbu6-core-usb|weact-stm32g431cbu6-core-motion|weact-stm32g431cbu6-core-katapult|all|ci]\n' \
            "$0" >&2
        exit 2
        ;;
esac
