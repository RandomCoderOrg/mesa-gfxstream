#!/data/data/com.termux/files/usr/bin/sh
set -eu

if [ -z "${PREFIX:-}" ] || [ ! -d "$PREFIX" ]; then
    echo "Run this script from a current Termux environment." >&2
    exit 1
fi

apt-get install -y \
    bison clang cmake flex git libandroid-shmem libc++ libdrm libwayland \
    libx11 libxrandr libxcb libxshmfence ninja pkg-config python \
    vulkan-headers xorgproto zlib zstd

python -m pip install packaging mako meson pyyaml

# The Android loader exposes VK_KHR_android_surface only. The wrapper is an
# ICD for the generic loader and adds Mesa's X11/XCB WSI implementation.
# Avoid the recommended Lavapipe package because this project selects the
# wrapper ICD explicitly.
apt-get install -y --no-install-recommends \
    vulkan-loader-generic vulkan-tools vulkan-loader-android-

echo "Termux Vulkan wrapper dependencies are installed."
