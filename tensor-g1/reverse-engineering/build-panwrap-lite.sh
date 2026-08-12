#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

cc=${CC:-clang}
src=${PANWRAP_LITE_SRC:-$(dirname "$0")/panwrap-lite.c}
out=${PANWRAP_LITE_OUT:-./panwrap-lite.so}

"$cc" -std=c11 -O2 -fPIC -shared -Wall -Wextra -Werror \
   "$src" -o "$out" -ldl

printf 'built=%s\n' "$out"
