#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

probe=${PROBE:-$HOME/buffer-texture-xfb-android}
wrapper=${PANWRAP_LITE_SO:-$HOME/panwrap-lite.so}
android_egl=${ANDROID_EGL_DIR:-$HOME/android-egl-proprietary}
log=${PANWRAP_LOG:-$HOME/hair-proprietary-ioctl.jsonl}

rm -f "$log"

export TENSOR_PROBE_PLATFORM=android
export LD_LIBRARY_PATH="$android_egl"
export LD_PRELOAD="$wrapper"
export PANWRAP_LOG="$log"

"$probe" "$@"
printf 'panwrap_log=%s\n' "$log"
