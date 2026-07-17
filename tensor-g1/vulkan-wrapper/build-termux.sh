#!/data/data/com.termux/files/usr/bin/sh
set -eu

SOURCE_URL=https://github.com/xMeM/mesa.git
SOURCE_COMMIT=e65c7eb6ee2f9903c3256f2677beb1d98464103f

SCRIPT_DIR=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
TERMUX_PREFIX=${PREFIX:-/data/data/com.termux/files/usr}
WORK_ROOT=${VULKAN_WRAPPER_WORKDIR:-"$HOME/.cache/tensor-g1-proot-gpu/vulkan-wrapper"}
SOURCE_DIR="$WORK_ROOT/source"
BUILD_DIR="$WORK_ROOT/build"
INSTALL_PREFIX=${VULKAN_WRAPPER_PREFIX:-"$HOME/.local/opt/tensor-g1-vulkan-wrapper"}
PATCH_STAMP="$WORK_ROOT/patches.sha256"

for command in getprop git meson ninja nproc python sha256sum; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Missing $command; run $SCRIPT_DIR/setup-termux.sh first." >&2
        exit 1
    fi
done

if [ "$(uname -m)" != aarch64 ]; then
    echo "This build recipe currently supports aarch64 Termux only." >&2
    exit 1
fi

mkdir -p "$WORK_ROOT"

if [ ! -d "$SOURCE_DIR/.git" ]; then
    mkdir -p "$SOURCE_DIR"
    git -C "$SOURCE_DIR" init
    git -C "$SOURCE_DIR" remote add origin "$SOURCE_URL"
    git -C "$SOURCE_DIR" fetch --depth 1 origin "$SOURCE_COMMIT"
    git -C "$SOURCE_DIR" checkout --detach FETCH_HEAD
fi

if [ "$(git -C "$SOURCE_DIR" rev-parse HEAD)" != "$SOURCE_COMMIT" ]; then
    echo "$SOURCE_DIR is not at the pinned wrapper commit." >&2
    echo "Choose a fresh VULKAN_WRAPPER_WORKDIR and rerun." >&2
    exit 1
fi

PATCH_HASH=$(
    for patch in "$SCRIPT_DIR"/patches/*.patch; do
        sha256sum <"$patch"
    done | sha256sum | cut -d ' ' -f 1
)

if [ -f "$PATCH_STAMP" ]; then
    if [ "$(cat "$PATCH_STAMP")" != "$PATCH_HASH" ]; then
        echo "The patch series changed after it was applied." >&2
        echo "Choose a fresh VULKAN_WRAPPER_WORKDIR and rerun." >&2
        exit 1
    fi
else
    if ! git -C "$SOURCE_DIR" diff --quiet || \
       ! git -C "$SOURCE_DIR" diff --cached --quiet; then
        echo "$SOURCE_DIR has unexpected modifications." >&2
        exit 1
    fi

    for patch in "$SCRIPT_DIR"/patches/*.patch; do
        echo "Applying $(basename "$patch")"
        sed "s|@TERMUX_PREFIX@|$TERMUX_PREFIX|g" "$patch" |
            git -C "$SOURCE_DIR" apply -
    done
    printf '%s\n' "$PATCH_HASH" >"$PATCH_STAMP"
fi

ANDROID_API=${ANDROID_API:-$(getprop ro.build.version.sdk)}
case "$ANDROID_API" in
    ''|*[!0-9]*)
        echo "Could not determine the Android API level: $ANDROID_API" >&2
        exit 1
        ;;
esac

TARGET=aarch64-linux-android$ANDROID_API
export CFLAGS="--target=$TARGET -D__USE_GNU ${CFLAGS:-}"
export CXXFLAGS="--target=$TARGET -D__USE_GNU ${CXXFLAGS:-}"
export LDFLAGS="--target=$TARGET -landroid-shmem ${LDFLAGS:-}"

set -- \
    --prefix="$INSTALL_PREFIX" \
    --libdir=lib \
    -Dbuildtype=release \
    -Db_ndebug=true \
    -Dcpp_rtti=false \
    -Dgallium-drivers= \
    -Dgbm=disabled \
    -Dllvm=disabled \
    -Dopengl=false \
    -Dplatforms=x11 \
    -Dshared-llvm=disabled \
    -Dvulkan-drivers=wrapper \
    -Dxmlconfig=disabled

if [ -f "$BUILD_DIR/meson-private/coredata.dat" ]; then
    meson setup "$BUILD_DIR" "$SOURCE_DIR" --reconfigure "$@"
else
    meson setup "$BUILD_DIR" "$SOURCE_DIR" "$@"
fi

meson compile -C "$BUILD_DIR" -j "${JOBS:-$(nproc)}"
meson install -C "$BUILD_DIR"

cat <<EOF

Installed Tensor G1 Vulkan wrapper to:
  $INSTALL_PREFIX

Test it with:
  $SCRIPT_DIR/run-vulkan-x11 vulkaninfo --summary
  $SCRIPT_DIR/run-vulkan-x11 vkcube --c 300 --wsi xcb
EOF
