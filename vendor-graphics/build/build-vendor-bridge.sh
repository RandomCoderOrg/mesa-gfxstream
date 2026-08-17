#!/usr/bin/env bash
set -euo pipefail

usage()
{
    cat >&2 <<'EOF'
Usage: build-vendor-bridge.sh OUTPUT_DIRECTORY

Build the pinned libhybris, Vulkan loader, sysvk, AHardwareBuffer wrapper and
thread-lifecycle probe into OUTPUT_DIRECTORY/stage/opt/udroid/graphics.
OUTPUT_DIRECTORY must not already exist.
EOF
}

(($# == 1)) || { usage; exit 2; }

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH='' cd -- "$script_dir/../.." && pwd)
lock=$script_dir/source-lock.json
output=$1
[[ $output == /* ]] || output=$PWD/$output
[[ ! -e $output ]] || {
    printf 'build-vendor-bridge: output already exists: %s\n' "$output" >&2
    exit 2
}
mkdir -p "$output"

"$script_dir/build-libhybris.sh" "$output/libhybris-build"
mkdir -p "$output/src" "$output/stage/opt/udroid/graphics/lib/bridge/libhybris/linker"

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
        printf 'build-vendor-bridge: revision mismatch for %s\n' "$name" >&2
        exit 3
    }
}

fetch_locked sysvk
fetch_locked vulkan_headers
fetch_locked vulkan_loader

git -C "$output/src/sysvk" apply --check \
    "$repo_root/vendor-graphics/patches/sysvk/0001-discover-and-validate-explicit-vulkan-hal.patch"
git -C "$output/src/sysvk" apply \
    "$repo_root/vendor-graphics/patches/sysvk/0001-discover-and-validate-explicit-vulkan-hal.patch"
git -C "$output/src/sysvk" diff --check

libhybris_prefix=$output/libhybris-build/stage/opt/udroid/graphics
bridge=$output/stage/opt/udroid/graphics/lib/bridge
install -Dm755 "$libhybris_prefix/lib/bridge/libhybris-common.so.1.0.0" \
    "$bridge/libhybris-common.so.1.0.0"
install -Dm755 "$libhybris_prefix/lib/bridge/libhardware.so.2.0.0" \
    "$bridge/libhardware.so.2.0.0"
install -Dm755 "$libhybris_prefix/lib/bridge/libhybris/linker/q.so" \
    "$bridge/libhybris/linker/q.so"
ln -s libhybris-common.so.1.0.0 "$bridge/libhybris-common.so.1"
ln -s libhybris-common.so.1.0.0 "$bridge/libhybris-common.so"
ln -s libhardware.so.2.0.0 "$bridge/libhardware.so.2"
ln -s libhardware.so.2.0.0 "$bridge/libhardware.so"

docker run --rm --platform linux/arm64 \
    --volume "$output:/work" \
    --volume "$repo_root:/repo:ro" \
    --env LC_ALL=C \
    --env TZ=UTC \
    udroid-graphics-build:jammy-aarch64 \
    bash -euo pipefail -c '
        cmake -S /work/src/vulkan_headers -B /work/build/vulkan-headers -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX=/work/vulkan-headers-install
        cmake --build /work/build/vulkan-headers --parallel "$(nproc)"
        cmake --install /work/build/vulkan-headers

        cmake -S /work/src/vulkan_loader -B /work/build/vulkan-loader -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX=/opt/udroid/graphics \
            -DCMAKE_INSTALL_LIBDIR=lib/bridge \
            -DVULKAN_HEADERS_INSTALL_DIR=/work/vulkan-headers-install \
            -DBUILD_TESTS=OFF \
            -DBUILD_WSI_XCB_SUPPORT=ON \
            -DBUILD_WSI_XLIB_SUPPORT=ON \
            -DBUILD_WSI_WAYLAND_SUPPORT=OFF
        cmake --build /work/build/vulkan-loader --parallel "$(nproc)"
        DESTDIR=/work/loader-stage cmake --install /work/build/vulkan-loader

        bridge=/work/stage/opt/udroid/graphics/lib/bridge
        install -Dm755 \
            /work/loader-stage/opt/udroid/graphics/lib/bridge/libvulkan.so.1.3.204 \
            "$bridge/libvulkan.so.1.3.204"
        ln -s libvulkan.so.1.3.204 "$bridge/libvulkan.so.1"
        ln -s libvulkan.so.1.3.204 "$bridge/libvulkan.so"

        # The explicit WSI layer intercepts these entry points, but it still
        # reaches them through the standard Loader dispatch table. A Loader
        # built without XCB/Xlib support can pass headless Vulkan probes and
        # then fail only when the first desktop surface is created.
        loader_symbols=$(nm -D --defined-only "$bridge/libvulkan.so.1" |
            awk "{ print \$3 }" | sed "s/@.*//")
        for symbol in vkCreateXcbSurfaceKHR vkCreateXlibSurfaceKHR; do
            # Do not use grep -q directly in the nm pipeline: with pipefail,
            # An early grep exit turns the expected nm SIGPIPE into a false
            # build failure.
            if ! grep -Fxq "$symbol" <<<"$loader_symbols"; then
                printf "build-vendor-bridge: Loader is missing %s\n" \
                    "$symbol" >&2
                exit 1
            fi
        done

        gcc -std=c11 -O2 -g0 -fPIC -shared \
            -ffile-prefix-map=/work=. \
            -I/work/src/sysvk/include \
            -I/work/src/vulkan_headers/include \
            -I/work/libhybris-build/src/android_headers \
            -I/work/libhybris-build/src/libhybris/hybris/include \
            /work/src/sysvk/sysvk.c \
            -L"$bridge" -Wl,--no-as-needed -lhardware -lhybris-common \
            -Wl,-rpath,'"'"'$ORIGIN'"'"' -Wl,-z,noexecstack -pthread -ldl \
            -o "$bridge/libsysvk.so"

        gcc -std=c11 -O2 -g0 -fPIC -shared \
            -ffile-prefix-map=/work=. \
            -I/work/libhybris-build/src/android_headers \
            -I/work/libhybris-build/src/libhybris/hybris/include \
            /repo/vendor-graphics/bridge/ahb-wrapper.c \
            -L"$bridge" -Wl,--no-as-needed -lhybris-common \
            -Wl,-rpath,'"'"'$ORIGIN'"'"' -Wl,-z,noexecstack -pthread -ldl \
            -o "$bridge/libahb-wrapper.so"

        libexec=/work/stage/opt/udroid/graphics/libexec
        mkdir -p "$libexec"
        gcc -std=c11 -O2 -g0 -ffile-prefix-map=/work=. \
            -I/work/src/vulkan_headers/include \
            /repo/vendor-graphics/probes/vulkan-thread-lifecycle.c \
            -L"$bridge" -lvulkan -ldl -pthread \
            -Wl,-rpath,'"'"'$ORIGIN/../lib/bridge'"'"' -Wl,-z,noexecstack \
            -o "$libexec/vulkan-thread-lifecycle"

        while IFS= read -r -d "" candidate; do
            if ! readelf -h "$candidate" >/dev/null 2>&1; then
                continue
            fi
            dynamic=$(readelf -dW "$candidate")
            while IFS= read -r line; do
                [[ $line =~ \((RPATH|RUNPATH)\).*\[(.*)\] ]] || continue
                path=${BASH_REMATCH[2]}
                case "$path" in
                    ""|'"'"'$ORIGIN'"'"'|'"'"'$ORIGIN/../lib/bridge'"'"') ;;
                    *) printf "non-relocatable runtime path in %s: %s\n" \
                           "$candidate" "$path" >&2; exit 1 ;;
                esac
            done <<<"$dynamic"
        done < <(find /work/stage/opt/udroid/graphics -type f -print0)
    '

mkdir -p "$output/stage/opt/udroid/graphics/share/vulkan/icd.d"
install -m 0644 "$repo_root/vendor-graphics/bridge/sysvk.json" \
    "$output/stage/opt/udroid/graphics/share/vulkan/icd.d/sysvk.json"

python3 - "$repo_root" "$output/stage/opt/udroid/graphics" <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv[1]) / "vendor-graphics" / "packaging"))
from runtime_contract import validate_libhybris_common

root = Path(sys.argv[2])
size = validate_libhybris_common(root / "lib/bridge/libhybris-common.so.1")
required = (
    "lib/bridge/libhardware.so.2",
    "lib/bridge/libsysvk.so",
    "lib/bridge/libahb-wrapper.so",
    "lib/bridge/libvulkan.so.1",
    "lib/bridge/libhybris/linker/q.so",
    "libexec/vulkan-thread-lifecycle",
    "share/vulkan/icd.d/sysvk.json",
)
for relative in required:
    if not (root / relative).exists():
        raise SystemExit(f"missing bridge output: {relative}")
print(f"PASS reproducible vendor bridge tls-bytes={size}")
PY
