#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
packer="${root_dir}/scripts/pack_factory_image.py"
output_dir="${root_dir}/out"
target="${1:-all}"

pack_one() {
    local bootloader="$1"
    local application="$2"
    local output="$3"
    local persistent_size="$4"

    python3 "${packer}" \
        --bootloader "${output_dir}/${bootloader}" \
        --application "${output_dir}/${application}" \
        --persistent-size "${persistent_size}" \
        --output "${output_dir}/${output}"
}

case "${target}" in
    fly-d5|mellow-fly-d5)
        pack_one katapult-stm32f072_mellow_fly_d5_dual.bin \
            remotebsp-stm32f072-fly-d5-katapult.bin \
            remotebsp-stm32f072-fly-d5-katapult-dual-factory.bin \
            0x1000
        ;;
    bluepill|weact-bluepill-plus)
        pack_one katapult-stm32f103_weact_bluepill_plus_dual.bin \
            remotebsp-stm32f103-bluepill-katapult.bin \
            remotebsp-stm32f103-bluepill-katapult-dual-factory.bin \
            0x800
        ;;
    g431|weact-stm32g431cbu6-core)
        pack_one katapult-stm32g431_weact_core_dual.bin \
            remotebsp-stm32g431-katapult.bin \
            remotebsp-stm32g431-katapult-dual-factory.bin \
            0x1000
        ;;
    all)
        pack_one katapult-stm32f072_mellow_fly_d5_dual.bin \
            remotebsp-stm32f072-fly-d5-katapult.bin \
            remotebsp-stm32f072-fly-d5-katapult-dual-factory.bin \
            0x1000
        pack_one katapult-stm32f103_weact_bluepill_plus_dual.bin \
            remotebsp-stm32f103-bluepill-katapult.bin \
            remotebsp-stm32f103-bluepill-katapult-dual-factory.bin \
            0x800
        pack_one katapult-stm32g431_weact_core_dual.bin \
            remotebsp-stm32g431-katapult.bin \
            remotebsp-stm32g431-katapult-dual-factory.bin \
            0x1000
        ;;
    *)
        printf '用法：%s [mellow-fly-d5|weact-bluepill-plus|weact-stm32g431cbu6-core|all]\n' "$0" >&2
        exit 2
        ;;
esac
