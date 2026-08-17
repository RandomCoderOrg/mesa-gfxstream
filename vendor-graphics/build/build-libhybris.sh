#!/usr/bin/env bash
set -euo pipefail

usage()
{
    cat >&2 <<'EOF'
Usage: build-libhybris.sh OUTPUT_DIRECTORY

Fetch the locked libhybris and Android-header revisions, apply the ordered
uDroid patches, and install the relocatable AArch64 bridge into:

  OUTPUT_DIRECTORY/stage/opt/udroid/graphics

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
    printf 'build-libhybris: output already exists: %s\n' "$output" >&2
    exit 2
}
mkdir -p "$output/src" "$output/stage"

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
        printf 'build-libhybris: revision mismatch for %s\n' "$name" >&2
        exit 3
    }
}

fetch_locked libhybris
fetch_locked android_headers

for patch in "$repo_root"/vendor-graphics/patches/libhybris/*.patch; do
    git -C "$output/src/libhybris" apply --check "$patch"
    git -C "$output/src/libhybris" apply "$patch"
done
git -C "$output/src/libhybris" diff --check

source_date_epoch=$(git -C "$output/src/libhybris" show -s --format=%ct HEAD)

docker run --rm --platform linux/arm64 \
    --volume "$output:/work" \
    --env LC_ALL=C \
    --env TZ=UTC \
    --env SOURCE_DATE_EPOCH="$source_date_epoch" \
    udroid-graphics-build:jammy-aarch64 \
    bash -euo pipefail -c '
        cd /work/src/libhybris/hybris
        ./autogen.sh
        ./configure \
            --prefix=/opt/udroid/graphics \
            --libdir=/opt/udroid/graphics/lib/bridge \
            --with-android-headers=/work/src/android_headers \
            --with-default-hybris-ld-library-path=/system/lib64:/system_ext/lib64:/product/lib64:/vendor/lib64:/vendor/lib64/egl:/vendor/lib64/hw:/odm/lib64:/apex/com.android.runtime/lib64/bionic \
            --enable-arch=arm64 \
            --enable-mali-quirks \
            --enable-property-cache \
            --enable-experimental \
            --enable-relocatable-runtime \
            --disable-debug \
            CFLAGS="-O2 -g0 -Wa,--noexecstack -fPIC -ffile-prefix-map=/work=." \
            CXXFLAGS="-O2 -g0 -Wa,--noexecstack -fPIC -ffile-prefix-map=/work=." \
            LDFLAGS="-Wl,-z,noexecstack"
        make -C common -j"$(nproc)"
        make -C hardware -j"$(nproc)"
        make -C common DESTDIR=/work/stage install
        make -C hardware DESTDIR=/work/stage install

        prefix=/work/stage/opt/udroid/graphics
        test -f "$prefix/lib/bridge/libhybris-common.so.1.0.0"
        test -f "$prefix/lib/bridge/libhardware.so.2.0.0"
        test -f "$prefix/lib/bridge/libhybris/linker/q.so"
        while IFS= read -r -d "" candidate; do
            if readelf -h "$candidate" >/dev/null 2>&1; then
                if readelf -dW "$candidate" | grep -Eq "\\((RPATH|RUNPATH)\\)"; then
                    printf "absolute or embedded runtime path in %s\n" "$candidate" >&2
                    exit 1
                fi
            fi
        done < <(find "$prefix" -type f -print0)
    '

python3 - "$repo_root" "$output/stage/opt/udroid/graphics" <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, str(Path(sys.argv[1]) / "vendor-graphics" / "packaging"))
from runtime_contract import validate_libhybris_common

root = Path(sys.argv[2])
size = validate_libhybris_common(root / "lib/bridge/libhybris-common.so.1")
print(f"PASS libhybris relocatable install tls-bytes={size}")
PY
