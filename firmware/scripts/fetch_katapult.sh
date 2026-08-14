#!/usr/bin/env bash
set -euo pipefail

# Katapult（原 CanBoot）使用 GPLv3。它作为独立 Bootloader 构建，
# 不与 RemoteBSP APP 链接。固定提交保证 Flash 布局和升级协议可复现。
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="${root_dir}/vendor/katapult"
repository="https://github.com/Arksine/katapult.git"
revision="ec59b9bb9ad6c2ec8d4dc6831fbc77f0b308e29e"
dual_patch="${root_dir}/bootloader/patches/katapult-dual-can-usb.patch"
persistent_patch="${root_dir}/bootloader/patches/katapult-persistent-region.patch"

apply_patch_file() {
    local patch_file="$1"
    local label="$2"
    if git -C "${target}" apply --reverse --check "${patch_file}" \
        >/dev/null 2>&1; then
        printf 'Katapult %s补丁已经应用。\n' "${label}"
        return
    fi
    if ! git -C "${target}" apply --check "${patch_file}"; then
        printf 'Katapult %s补丁与当前源码不匹配，请人工检查。\n' \
            "${label}" >&2
        exit 1
    fi
    git -C "${target}" apply "${patch_file}"
    printf '已应用 Katapult %s补丁。\n' "${label}"
}

apply_dual_patch() {
    apply_patch_file "${dual_patch}" "CAN/USB双模式"
}

apply_persistent_patch() {
    apply_patch_file "${persistent_patch}" "持久化区写保护"
}

if [[ -d "${target}/.git" ]]; then
    actual="$(git -C "${target}" rev-parse HEAD)"
    if [[ "${actual}" != "${revision}" ]]; then
        printf 'Katapult 版本不匹配：期望 %s，当前 %s\n' \
            "${revision}" "${actual}" >&2
        printf '为避免覆盖本地修改，请人工确认 vendor/katapult。\n' >&2
        exit 1
    fi
    printf 'Katapult 已存在且版本正确：%s\n' "${revision}"
    apply_dual_patch
    apply_persistent_patch
    exit 0
fi

if [[ -e "${target}" ]]; then
    printf '错误：%s 已存在但不是 Git 仓库，请人工确认。\n' \
        "${target}" >&2
    exit 1
fi

git clone "${repository}" "${target}"
git -C "${target}" checkout --detach "${revision}"
printf 'Katapult 已固定到：%s\n' "${revision}"
apply_dual_patch
apply_persistent_patch
