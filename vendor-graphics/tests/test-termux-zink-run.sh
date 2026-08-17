#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
runner=$root/vendor-graphics/termux/bin/udroid-zink-run
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT

home=$temporary/home
prefix=$temporary/prefix
icd_dir=$home/.local/opt/udroid-vulkan-wrapper/share/vulkan/icd.d
library=$home/.local/opt/udroid-vulkan-wrapper/lib/libvulkan_wrapper.so
mkdir -p "$icd_dir" "$(dirname -- "$library")" "$prefix/lib/dri"
touch "$library" "$prefix/lib/dri/zink_dri.so"
printf '%s\n' \
    '{' \
    '  "file_format_version": "1.0.0",' \
    '  "ICD": {' \
    '    "library_path": "'"$library"'",' \
    '    "api_version": "1.3.0"' \
    '  }' \
    '}' > "$icd_dir/wrapper_icd.aarch64.json"

HOME=$home \
PREFIX=$prefix \
DISPLAY=remote.example:0 \
UDROID_ZINK_TRACE=1 \
    sh "$runner" -- /usr/bin/env > "$temporary/environment"

grep -Fqx 'DISPLAY=remote.example:0' "$temporary/environment"
grep -Fqx "VK_DRIVER_FILES=$icd_dir/wrapper_icd.aarch64.json" \
    "$temporary/environment"
grep -Fqx 'MESA_LOADER_DRIVER_OVERRIDE=zink' "$temporary/environment"
grep -Fqx 'GALLIUM_DRIVER=zink' "$temporary/environment"
grep -Fqx 'LIBGL_KOPPER_DRI2=1' "$temporary/environment"
grep -Fqx 'QT_QPA_PLATFORM=xcb' "$temporary/environment"
grep -Fqx 'SDL_VIDEODRIVER=x11' "$temporary/environment"

HOME=$home PREFIX=$prefix DISPLAY=remote.example:0 \
    sh "$runner" --check > "$temporary/check"
grep -Fqx "VULKAN_ICD_LIBRARY=$library" "$temporary/check"
grep -Fqx "ZINK_DRI=$prefix/lib/dri/zink_dri.so" "$temporary/check"

if HOME=$temporary/missing PREFIX=$prefix DISPLAY=remote.example:0 \
    sh "$runner" --check > /dev/null 2> "$temporary/missing.log"; then
    echo 'runner unexpectedly accepted a missing ICD' >&2
    exit 1
fi
grep -Fq 'no readable uDroid vendor-Vulkan ICD' "$temporary/missing.log"

printf 'PASS native Termux wrapper selects the vendor ICD and Zink deterministically\n'
