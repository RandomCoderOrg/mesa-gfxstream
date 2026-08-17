#!/usr/bin/env bash
set -euo pipefail

usage()
{
    cat >&2 <<'EOF'
Usage: build-mesa.sh OUTPUT_DIRECTORY

Build the pinned Mesa/Zink source and its ordered compatibility patches into
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
    printf 'build-mesa: output already exists: %s\n' "$output" >&2
    exit 2
}
mkdir -p "$output/src"

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

url=$(source_field mesa url)
revision=$(source_field mesa revision)
git init -q "$output/src/mesa"
git -C "$output/src/mesa" remote add origin "$url"
git -C "$output/src/mesa" fetch -q --depth=1 origin "$revision"
git -C "$output/src/mesa" checkout -q --detach FETCH_HEAD
[[ $(git -C "$output/src/mesa" rev-parse HEAD) == "$revision" ]] || {
    printf 'build-mesa: revision mismatch\n' >&2
    exit 3
}

for patch in "$repo_root"/vendor-graphics/patches/mesa/*.patch; do
    git -C "$output/src/mesa" apply --check "$patch"
    git -C "$output/src/mesa" apply "$patch"
done
git -C "$output/src/mesa" diff --check

docker run --rm --platform linux/arm64 \
    --volume "$output:/work" \
    --env LC_ALL=C \
    --env TZ=UTC \
    udroid-graphics-build:jammy-aarch64 \
    bash -euo pipefail -c '
        meson setup /work/build/mesa /work/src/mesa \
            --buildtype=release \
            --prefix=/opt/udroid/graphics \
            --libdir=lib/mesa \
            -Dplatforms=x11 \
            -Dgallium-drivers=zink \
            -Dvulkan-drivers= \
            -Ddri-drivers= \
            -Dglx=dri \
            -Degl=enabled \
            -Dgles1=enabled \
            -Dgles2=enabled \
            -Dshared-glapi=enabled \
            -Dgbm=disabled \
            -Dllvm=disabled \
            -Dlibunwind=disabled \
            -Dvalgrind=disabled \
            -Dgallium-vdpau=disabled \
            -Dgallium-va=disabled \
            -Dgallium-xa=disabled \
            -Dgallium-omx=disabled \
            -Dshader-cache=enabled \
            -Dzstd=enabled \
            -Db_ndebug=true
        meson compile -C /work/build/mesa -j "$(nproc)"
        DESTDIR=/work/stage meson install -C /work/build/mesa

        mesa=/work/stage/opt/udroid/graphics/lib/mesa
        for required in \
            libEGL.so.1 \
            libGL.so.1 \
            libGLESv1_CM.so.1 \
            libGLESv2.so.2 \
            libglapi.so.0 \
            dri/zink_dri.so; do
            test -e "$mesa/$required" || {
                printf "build-mesa: missing output: lib/mesa/%s\n" \
                    "$required" >&2
                exit 1
            }
        done

        while IFS= read -r -d "" candidate; do
            if ! readelf -h "$candidate" >/dev/null 2>&1; then
                continue
            fi
            machine=$(readelf -h "$candidate" | sed -n "s/^ *Machine: *//p")
            [[ $machine == AArch64 ]] || {
                printf "build-mesa: non-AArch64 ELF in runtime: %s\n" \
                    "$candidate" >&2
                exit 1
            }
            dynamic=$(readelf -dW "$candidate")
            if grep -Eq "\\((RPATH|RUNPATH)\\)" <<<"$dynamic"; then
                printf "build-mesa: unexpected RPATH/RUNPATH in %s\n" \
                    "$candidate" >&2
                exit 1
            fi
        done < <(find "$mesa" -type f -print0)
    '

printf 'PASS reproducible matched Mesa/Zink\n'
