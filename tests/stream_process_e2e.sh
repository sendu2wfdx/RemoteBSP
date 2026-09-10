#!/usr/bin/env bash
set -euo pipefail

toolbusd_bin="$1"
mock_mcu_bin="$2"
remote_cli_bin="$3"
manifest="$4"
can_only_manifest="$5"
work="$(mktemp -d)"
link="$work/usb.sock"
ipc="$work/toolbusd.sock"
ledger="$work/ledger"
node_log="$work/node.log"
daemon_log="$work/daemon.log"
node_pid=""
daemon_pid=""
cleanup() {
  result=$?
  [[ -z "$daemon_pid" ]] || kill "$daemon_pid" 2>/dev/null || true
  [[ -z "$node_pid" ]] || kill "$node_pid" 2>/dev/null || true
  wait "$daemon_pid" 2>/dev/null || true
  wait "$node_pid" 2>/dev/null || true
  if [[ $result -ne 0 ]]; then
    sed -n '1,120p' "$daemon_log" >&2 || true
    sed -n '1,120p' "$node_log" >&2 || true
  fi
  rm -rf "$work"
  exit "$result"
}
trap cleanup EXIT

"$mock_mcu_bin" "$link" usb-mock --board "$manifest" \
  --stream-source 251658241 >"$node_log" 2>&1 &
node_pid=$!
mkdir -p "$ledger"
chmod 700 "$ledger"
"$toolbusd_bin" "$link" usb-mock "$ipc" \
  --runtime-operation-ledger-dir "$ledger" >"$daemon_log" 2>&1 &
daemon_pid=$!

for _ in $(seq 1 100); do
  [[ -S "$ipc" ]] && "$remote_cli_bin" --socket "$ipc" node-list 2>/dev/null | \
    grep -Eq 'node_id=1 .*online=1|online=1 .*node_id=1' && break
  sleep 0.05
done
"$remote_cli_bin" --socket "$ipc" node-list | \
  grep -Eq 'node_id=1 .*online=1|online=1 .*node_id=1'
lease=""
for _ in $(seq 1 100); do
  lease="$($remote_cli_bin --socket "$ipc" --node 1 \
    resource-acquire 251658241 10000 shared-read 2>/dev/null || true)"
  [[ "$lease" == *lease_id=* ]] && break
  sleep 0.05
done
grep -q 'lease_id=' <<<"$lease"
opened="$($remote_cli_bin --socket "$ipc" --node 1 stream-open 251658241 16 5 64)"
stream_id="$(sed -n 's/.*stream_id=\([0-9]*\).*/\1/p' <<<"$opened")"
[[ -n "$stream_id" ]]
first="$($remote_cli_bin --socket "$ipc" --node 1 stream-read "$stream_id" 0 3000)"
grep -q "stream_id=$stream_id sequence=0 data_hex=6d6f636b2d73747265616d0a" <<<"$first"
second="$($remote_cli_bin --socket "$ipc" --node 1 stream-read "$stream_id" 1 3000)"
grep -q "stream_id=$stream_id sequence=1 data_hex=6d6f636b2d73747265616d0a" <<<"$second"
status="$($remote_cli_bin --socket "$ipc" --node 1 stream-status "$stream_id")"
grep -q 'available_credit_bytes=64' <<<"$status"

# 错误预期序号不得消费事件或归还信用，随后正确序号仍可取得同一块。
if "$remote_cli_bin" --socket "$ipc" --node 1 stream-read "$stream_id" 9 3000 >/dev/null 2>&1; then
  echo "错误序号被接受" >&2
  exit 1
fi
third="$($remote_cli_bin --socket "$ipc" --node 1 stream-read "$stream_id" 2 1000)"
grep -q "stream_id=$stream_id sequence=2" <<<"$third"

# 停止后旧 stream_id 必须立即被明确拒绝，而不是等待数据超时。
"$remote_cli_bin" --socket "$ipc" --node 1 stream-stop "$stream_id"
started_at="$(date +%s)"
if "$remote_cli_bin" --socket "$ipc" --node 1 stream-read "$stream_id" 3 10000 \
    >/dev/null 2>&1; then
  echo "已停止的旧 stream_id 被接受" >&2
  exit 1
fi
(( $(date +%s) - started_at < 5 ))

# 同一条 Mock USB 链路也不能打开仅授权 CAN 的静态合同。
kill "$daemon_pid" "$node_pid"
wait "$daemon_pid" 2>/dev/null || true
wait "$node_pid" 2>/dev/null || true
daemon_pid=""
node_pid=""
rm -f "$link" "$ipc"
"$mock_mcu_bin" "$link" usb-mock --board "$can_only_manifest" \
  >"$node_log" 2>&1 &
node_pid=$!
"$toolbusd_bin" "$link" usb-mock "$ipc" \
  --runtime-operation-ledger-dir "$ledger" >"$daemon_log" 2>&1 &
daemon_pid=$!
for _ in $(seq 1 100); do
  [[ -S "$ipc" ]] && "$remote_cli_bin" --socket "$ipc" node-list 2>/dev/null | \
    grep -q 'online=1' && break
  sleep 0.05
done
if "$remote_cli_bin" --socket "$ipc" --node 1 \
    stream-open 251658241 16 5 64 >/dev/null 2>&1; then
  echo "仅授权 CAN 的 STREAM 合同被 USB 链路错误接受" >&2
  exit 1
fi
