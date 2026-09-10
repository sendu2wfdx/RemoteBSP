#!/usr/bin/env bash
set -euo pipefail

mock="$1"; daemon="$2"; cli="$3"; manifest="$4"
link="/tmp/rbsp-record-link-$$.sock"
ipc="/tmp/rbsp-record-ipc-$$.sock"
ledger="$(mktemp -d /tmp/rbsp-record-ledger-XXXXXX)"
records="$(mktemp -d /tmp/rbsp-record-output-XXXXXX)"
mock_log="/tmp/rbsp-record-mock-$$.log"
daemon_log="/tmp/rbsp-record-daemon-$$.log"
mock_pid=""; daemon_pid=""
cleanup() {
  result=$?
  [[ -n "$mock_pid" ]] && kill "$mock_pid" 2>/dev/null || true
  [[ -n "$daemon_pid" ]] && kill "$daemon_pid" 2>/dev/null || true
  [[ -n "$mock_pid" ]] && wait "$mock_pid" 2>/dev/null || true
  [[ -n "$daemon_pid" ]] && wait "$daemon_pid" 2>/dev/null || true
  if [[ $result -ne 0 ]]; then sed -n '1,120p' "$daemon_log" >&2 || true; fi
  rm -f -- "$link" "$ipc" "$mock_log" "$daemon_log"
  rm -rf -- "$ledger" "$records"
  exit "$result"
}
trap cleanup EXIT INT TERM

"$daemon" "$link" usb-mock "$ipc" --runtime-operation-ledger-dir "$ledger" \
  --logical-recording-dir "$records" >"$daemon_log" 2>&1 & daemon_pid=$!
"$mock" "$link" usb-mock --instance 1 --board "$manifest" >"$mock_log" 2>&1 & mock_pid=$!
for _ in $(seq 1 500); do
  [[ -S "$ipc" ]] && "$cli" --socket "$ipc" logical-recording-status >/dev/null 2>&1 && break
  kill -0 "$daemon_pid"; sleep 0.02
done

status="$($cli --socket "$ipc" logical-recording-status)"
grep -Fq 'configured=1 active=0 evidence_scope=logical-link-boundary-only' <<<"$status"
"$cli" --socket "$ipc" logical-recording-start term.rbsplog >/dev/null
sleep 0.1
grep -Fq 'active=1' <<<"$($cli --socket "$ipc" logical-recording-status)"
kill -TERM "$daemon_pid"
wait "$daemon_pid"; daemon_pid=""
test -s "$records/term.rbsplog"

# no-clobber：重启后同名目标仍必须拒绝。
"$daemon" "$link" usb-mock "$ipc" --runtime-operation-ledger-dir "$ledger" \
  --logical-recording-dir "$records" >"$daemon_log" 2>&1 & daemon_pid=$!
for _ in $(seq 1 300); do
  [[ -S "$ipc" ]] && "$cli" --socket "$ipc" logical-recording-status >/dev/null 2>&1 && break
  kill -0 "$daemon_pid"; sleep 0.02
done
if "$cli" --socket "$ipc" logical-recording-start term.rbsplog >/dev/null 2>&1; then
  echo '已有录制文件被意外覆盖' >&2; exit 1
fi
