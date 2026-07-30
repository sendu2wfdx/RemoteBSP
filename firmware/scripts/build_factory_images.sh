#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
packer="${root_dir}/scripts/pack_factory_image.py"
output_dir="${root_dir}/out"

pack_one() {
    local bootloader="$1"
    local application="$2"
    local output="$3"

    python3 "${packer}" \
        --bootloader "${output_dir}/${bootloader}" \
        --application "${output_dir}/${application}" \
        --output "${output_dir}/${output}"
}

pack_one katapult-stm32f103_dual.bin \
    remotebsp-stm32f103-bluepill-katapult.bin \
    remotebsp-stm32f103-bluepill-katapult-dual-factory.bin
pack_one katapult-stm32g431_dual.bin \
    remotebsp-stm32g431-katapult.bin \
    remotebsp-stm32g431-katapult-dual-factory.bin
