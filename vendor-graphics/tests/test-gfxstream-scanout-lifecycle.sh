#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source_dir="$repo_root/vendor-graphics/experiments/gfxstream"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

"${CC:-cc}" -std=c17 -Wall -Wextra -Werror -pedantic \
  "$source_dir/scanout-lifecycle.c" \
  "$source_dir/test-scanout-lifecycle.c" \
  -o "$build_dir/test-scanout-lifecycle"

"$build_dir/test-scanout-lifecycle"
