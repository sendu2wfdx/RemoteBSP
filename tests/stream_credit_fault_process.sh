#!/usr/bin/env bash
set -euo pipefail

toolbusd_bin="$1"
mock_mcu_bin="$2"
fault_client_bin="$3"
manifest="$4"
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
    sed -n '1,160p' "$daemon_log" >&2 || true
    sed -n '1,160p' "$node_log" >&2 || true
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
  --runtime-operation-ledger-dir "$ledger" \
  --test-stream-credit-drop-ipc-response 1 >"$daemon_log" 2>&1 &
daemon_pid=$!

for _ in $(seq 1 100); do
  [[ -S "$ipc" ]] && break
  sleep 0.05
done
[[ -S "$ipc" ]]
"$fault_client_bin" "$ipc"
