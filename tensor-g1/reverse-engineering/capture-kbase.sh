#!/bin/sh
# SPDX-License-Identifier: MIT

set -u

if [ "$#" -eq 0 ]; then
   echo "usage: $0 PROGRAM [ARG ...]" >&2
   exit 2
fi

capture_dir=${TENSOR_CAPTURE_DIR:-tensor-g1/reverse-engineering/results}
stamp=$(date -u +%Y%m%dT%H%M%SZ)
prefix="$capture_dir/kbase-$stamp"
mkdir -p "$capture_dir"

set +e
strace -ff -ttt -T -yy -s 128 \
   -e trace=openat,close,ioctl,mmap,munmap,poll,ppoll \
   -o "$prefix.strace" "$@"
status=$?
set -e

for trace in "$prefix.strace"*; do
   grep -E '/dev/mali0|ioctl\(' "$trace" > "$trace.kbase" || true
done

printf 'exit_status=%s\ntrace_prefix=%s\n' "$status" "$prefix"
exit "$status"
