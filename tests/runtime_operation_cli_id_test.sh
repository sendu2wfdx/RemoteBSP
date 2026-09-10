#!/usr/bin/env bash
set -eu

remote_cli="$1"
uppercase_id="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
if output="$("${remote_cli}" --json --socket /tmp/no-operation-ledger.sock \
    runtime-operation-status 01010101010101010101010101010101 owner \
    "${uppercase_id}" 2>&1)"; then
    echo "remote-cli 错误接受了非规范大写 operation_id" >&2
    exit 1
fi

case "${output}" in
    *"operation ID 必须是64位规范小写十六进制"*) ;;
    *)
        echo "remote-cli 未在连接前按规范拒绝大写 operation_id: ${output}" >&2
        exit 1
        ;;
esac
