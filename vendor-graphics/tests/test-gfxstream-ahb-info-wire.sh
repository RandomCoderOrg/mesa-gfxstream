#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
source_dir=$root/vendor-graphics/experiments/gfxstream
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT

${CC:-cc} -std=c17 -Wall -Wextra -Werror -pedantic \
    -I"$source_dir" \
    "$source_dir/ahb-info-wire.c" \
    "$source_dir/test-ahb-info-wire.c" \
    -o "$temporary/test-ahb-info-wire"

"$temporary/test-ahb-info-wire"
