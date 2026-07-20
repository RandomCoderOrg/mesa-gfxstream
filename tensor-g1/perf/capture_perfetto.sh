#!/bin/sh
set -eu

usage() {
  echo "usage: $0 OUTPUT.perfetto-trace [ADB]" >&2
  exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
output=$1
adb=${2:-adb}
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
remote=/data/misc/perfetto-traces/tensor-g1-codex.perfetto-trace

"$adb" get-state >/dev/null
"$adb" shell perfetto --txt -c - -o "$remote" <"$script_dir/perfetto.cfg"
"$adb" shell cat "$remote" >"$output"
"$adb" shell rm -f "$remote"

bytes=$(wc -c <"$output" | tr -d ' ')
echo "captured $bytes bytes to $output"
