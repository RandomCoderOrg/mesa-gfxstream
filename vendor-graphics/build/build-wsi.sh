#!/usr/bin/env bash
set -euo pipefail

usage()
{
    cat >&2 <<'EOF'
Usage: build-wsi.sh OUTPUT_DIRECTORY

Build the pinned vendor bridge and patched xMeM AHardwareBuffer X11 WSI into
OUTPUT_DIRECTORY/stage/opt/udroid/graphics. OUTPUT_DIRECTORY must not exist.
EOF
}

(($# == 1)) || { usage; exit 2; }

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/../.." && pwd)
lock=$script_dir/source-lock.json
output=$1
[[ $output == /* ]] || output=$PWD/$output
[[ ! -e $output ]] || {
    printf 'build-wsi: output already exists: %s\n' "$output" >&2
    exit 2
}
mkdir -p "$output"

"$script_dir/build-vendor-bridge.sh" "$output/vendor-bridge"
mkdir -p "$output/src" "$output/stage/opt/udroid/graphics"
cp -a "$output/vendor-bridge/stage/opt/udroid/graphics/." \
    "$output/stage/opt/udroid/graphics/"

source_field()
{
    python3 - "$lock" "$1" "$2" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    lock = json.load(handle)
print(lock["sources"][sys.argv[2]][sys.argv[3]])
PY
}

fetch_locked()
{
    local name=$1
    local url revision destination
    url=$(source_field "$name" url)
    revision=$(source_field "$name" revision)
    destination=$output/src/$name
    git init -q "$destination"
    git -C "$destination" remote add origin "$url"
    git -C "$destination" fetch -q --depth=1 origin "$revision"
    git -C "$destination" checkout -q --detach FETCH_HEAD
    [[ $(git -C "$destination" rev-parse HEAD) == "$revision" ]] || {
        printf 'build-wsi: revision mismatch for %s\n' "$name" >&2
        exit 3
    }
}

fetch_locked ginkage
fetch_locked wsi_headers

for patch in "$repo_root"/vendor-graphics/patches/xmem-wsi/*.patch; do
    git -C "$output/src/ginkage" apply --check "$patch"
    git -C "$output/src/ginkage" apply "$patch"
done
git -C "$output/src/ginkage" diff --check

docker run --rm --platform linux/arm64 \
    --volume "$output:/work" \
    --env LC_ALL=C \
    --env TZ=UTC \
    udroid-graphics-build:jammy-aarch64 \
    bash -euo pipefail -c '
        headers=/work/src/wsi_headers/include
        android_headers=/work/vendor-bridge/libhybris-build/src/android_headers
        test -f "$headers/vulkan/vulkan.h"
        test -f "$android_headers/android/hardware_buffer.h"

        cmake -S /work/src/ginkage -B /work/build/wsi -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX=/opt/udroid/graphics \
            -DVULKAN_CXX_INCLUDE="$headers" \
            -DANDROID_HEADERS_INCLUDE_DIR="$android_headers" \
            -DBUILD_WSI_HEADLESS=ON \
            -DBUILD_WSI_X11=ON \
            -DBUILD_WSI_WAYLAND=OFF \
            -DBUILD_WSI_DISPLAY=OFF \
            -DBUILD_WSI_DISPLAY_SUPPORT_FORMAT_MODIFIERS=ON \
            -DBUILD_WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN=OFF \
            -DVULKAN_WSI_LAYER_EXPERIMENTAL=OFF \
            -DENABLE_INSTRUMENTATION=OFF
        cmake --build /work/build/wsi --parallel "$(nproc)"
        DESTDIR=/work/wsi-stage cmake --install /work/build/wsi

        source_dir=/work/wsi-stage/opt/udroid/graphics/share/vulkan/implicit_layer.d
        target_dir=/work/stage/opt/udroid/graphics/share/vulkan/explicit_layer.d
        mkdir -p "$target_dir"
        install -m 0755 \
            "$source_dir/libVkLayer_window_system_integration.so" \
            "$target_dir/libVkLayer_window_system_integration.so"
        install -m 0644 \
            "$source_dir/VkLayer_window_system_integration.json" \
            "$target_dir/VkLayer_window_system_integration.json"

        layer="$target_dir/libVkLayer_window_system_integration.so"
        manifest="$target_dir/VkLayer_window_system_integration.json"
        readelf -h "$layer" | grep -q "Machine:.*AArch64"
        dynamic=$(readelf -dW "$layer")
        if grep -Eq "\((RPATH|RUNPATH)\)" <<<"$dynamic"; then
            printf "build-wsi: WSI layer contains an unexpected RPATH/RUNPATH\n" >&2
            exit 1
        fi
        python3 - "$manifest" <<'"'"'PY'"'"'
import json
import sys
from pathlib import PurePosixPath

with open(sys.argv[1], encoding="utf-8") as handle:
    manifest = json.load(handle)
layer = manifest.get("layer", {})
if layer.get("name") != "VK_LAYER_window_system_integration":
    raise SystemExit("unexpected WSI layer name")
library = PurePosixPath(layer.get("library_path", ""))
if library.is_absolute() or library.name != "libVkLayer_window_system_integration.so" or ".." in library.parts:
    raise SystemExit("WSI manifest must use a sibling-relative library_path")
for extension_group in ("instance_extensions", "device_extensions"):
    for extension in layer.get(extension_group, []):
        if not extension.get("name"):
            raise SystemExit(f"nameless entry in {extension_group}")
PY
    '

for required in \
    share/vulkan/explicit_layer.d/libVkLayer_window_system_integration.so \
    share/vulkan/explicit_layer.d/VkLayer_window_system_integration.json; do
    [[ -e $output/stage/opt/udroid/graphics/$required ]] || {
        printf 'build-wsi: missing output: %s\n' "$required" >&2
        exit 3
    }
done

printf 'PASS reproducible patched AHardwareBuffer WSI\n'
