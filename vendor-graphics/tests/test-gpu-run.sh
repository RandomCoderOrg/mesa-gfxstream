#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
runner=$root/vendor-graphics/runtime/bin/udroid-gpu-run
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT

mkdir -p \
    "$temporary/runtime/lib/mesa/dri" \
    "$temporary/runtime/lib/bridge/libhybris/linker" \
    "$temporary/runtime/share/vulkan/icd.d" \
    "$temporary/runtime/share/vulkan/explicit_layer.d" \
    "$temporary/xdg"
touch \
    "$temporary/runtime/lib/mesa/dri/zink_dri.so" \
    "$temporary/runtime/lib/bridge/libhybris/linker/q.so" \
    "$temporary/runtime/share/vulkan/icd.d/sysvk.json" \
    "$temporary/runtime/share/vulkan/explicit_layer.d/VkLayer_window_system_integration.json"

UDROID_GRAPHICS_ROOT=$temporary/runtime \
XDG_RUNTIME_DIR=$temporary/xdg \
UDROID_VULKAN_HAL=/vendor/lib64/hw/vulkan.test.so \
    "$runner" --profile vendor-vulkan:ginkage-ahb -- \
    /usr/bin/env > "$temporary/environment"

grep -Fqx 'UDROID_RESOLVED_RENDERER=vendor-vulkan' "$temporary/environment"
grep -Fqx 'UDROID_RESOLVED_WSI=ginkage-ahb' "$temporary/environment"
grep -Fqx 'WSI_X11_AHB=1' "$temporary/environment"
grep -Fqx 'WSI_X11_PRIVATE_CONNECTION=1' "$temporary/environment"
grep -Fqx 'MESA_LOADER_DRIVER_OVERRIDE=zink' "$temporary/environment"
grep -Fqx 'UDROID_VULKAN_HAL=/vendor/lib64/hw/vulkan.test.so' "$temporary/environment"
grep -Fqx "HYBRIS_LINKER_DIR=$temporary/runtime/lib/bridge/libhybris/linker" \
    "$temporary/environment"
grep -Fqx "VK_DRIVER_FILES=$temporary/runtime/share/vulkan/icd.d/sysvk.json" \
    "$temporary/environment"

system_result=$(
    UDROID_GRAPHICS_ROOT=$temporary/missing \
        "$runner" --profile system -- /usr/bin/printf '%s' system-pass
)
[[ $system_result == system-pass ]]

if UDROID_GRAPHICS_ROOT=$temporary/runtime \
    "$runner" --profile invalid -- /usr/bin/true 2>"$temporary/invalid.log"; then
    echo 'runner unexpectedly accepted an unknown profile' >&2
    exit 1
fi
grep -Fq 'unknown profile' "$temporary/invalid.log"

printf 'PASS runtime wrapper selects one explicit profile without modifying system mode\n'
