#!/bin/bash
set -euo pipefail

unset CDPATH
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
root=${TENSOR_VK_ROOT:-/root/hybris-rootless}
source_dir=${TENSOR_VK_XMEM_SOURCE:-$root/src/xmem-vulkan-wsi-layer}
build_dir=${TENSOR_VK_XMEM_BUILD:-$root/build-xmem-ahb}
prefix=${TENSOR_VK_XMEM_PREFIX:-$root/xmem-ahb-prefix}
probe_prefix=${TENSOR_VK_AHB_PROBE_PREFIX:-$root/ahb-probe-prefix}
android_include=${TENSOR_VK_ANDROID_INCLUDE:-$root/prefix/include}
vulkan_include=${TENSOR_VK_VULKAN_INCLUDE:-$root/src/Vulkan-Headers/include}
expected_revision=d5624d42d8b2debbd910ad25662a05c751eb38b7
patch_file="$script_dir/patches/xmem-wsi-0001-use-preloaded-ahb-wrapper.patch"

if [ ! -d "$source_dir/.git" ]; then
  echo "xMeM source is unavailable at $source_dir" >&2
  exit 3
fi
revision=$(git -C "$source_dir" rev-parse HEAD)
if [ "$revision" != "$expected_revision" ]; then
  echo "xMeM revision mismatch: expected $expected_revision, found $revision" >&2
  exit 4
fi

if git -C "$source_dir" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
  : # Patch already present.
elif git -C "$source_dir" apply --check "$patch_file" >/dev/null 2>&1; then
  git -C "$source_dir" apply "$patch_file"
else
  echo "xMeM source does not match the pinned patch or contains overlapping changes" >&2
  exit 5
fi

make -C "$script_dir/ahb-probe" \
  PREFIX="$probe_prefix" \
  HYBRIS_BUILD="$root/src/libhybris-upstream/hybris" \
  ANDROID_INCLUDE="$android_include" \
  install

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DVULKAN_CXX_INCLUDE="$vulkan_include" \
  -DANDROID_HEADERS_INCLUDE_DIR="$android_include" \
  -DBUILD_WSI_HEADLESS=1 \
  -DBUILD_WSI_WAYLAND=0 \
  -DBUILD_WSI_DISPLAY=0 \
  -DBUILD_WSI_X11=1 \
  -DSELECT_EXTERNAL_ALLOCATOR=dma_buf_heaps \
  -DWSIALLOC_MEMORY_HEAP_NAME=system-uncached
cmake --build "$build_dir" -j"$(nproc)"
cmake --install "$build_dir"

layer_dir="$prefix/share/vulkan/implicit_layer.d"
install -m 0644 "$script_dir/VkLayer_udroid_xmem_ahb.jammy.json" \
  "$layer_dir/VkLayer_udroid_xmem_ahb.json"

test -s "$probe_prefix/lib/libahb-wrapper.so"
test -s "$layer_dir/libVkLayer_window_system_integration.so"
test -s "$layer_dir/VkLayer_udroid_xmem_ahb.json"
printf 'xMeM AHB oracle ready\nrevision=%s\nprefix=%s\nprobe_prefix=%s\n' \
  "$revision" "$prefix" "$probe_prefix"
