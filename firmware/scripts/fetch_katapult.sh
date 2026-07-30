#!/usr/bin/env bash
set -euo pipefail

# Katapult（原 CanBoot）使用 GPLv3。它作为独立 Bootloader 构建，
# 不与 RemoteBSP APP 链接。固定提交保证 Flash 布局和升级协议可复现。
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="${root_dir}/vendor/katapult"
repository="https://github.com/Arksine/katapult.git"
revision="ec59b9bb9ad6c2ec8d4dc6831fbc77f0b308e29e"
dual_patch="${root_dir}/bootloader/patches/katapult-dual-can-usb.patch"

apply_dual_patch() {
    if git -C "${target}" apply --reverse --check "${dual_patch}" \
        >/dev/null 2>&1; then
        printf 'Katapult 双模式补丁已经应用。\n'
        return
    fi
    if ! git -C "${target}" apply --check "${dual_patch}"; then
        printf 'Katapult 双模式补丁与当前源码不匹配，请人工检查。\n' >&2
        exit 1
    fi
    git -C "${target}" apply "${dual_patch}"
    printf '已应用 Katapult CAN/USB 双模式补丁。\n'
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
