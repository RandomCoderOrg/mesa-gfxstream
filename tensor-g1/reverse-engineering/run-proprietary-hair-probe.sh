#!/bin/sh
set -eu

HYBRIS=${HYBRIS:-/root/hybris-rootless/src/libhybris-upstream/hybris}
PROBE=${PROBE:-/root/buffer-texture-xfb-hybris}

export TENSOR_PROBE_PLATFORM=android
export HYBRIS_EGLPLATFORM=null
export HYBRIS_EGLPLATFORM_DIR="$HYBRIS/egl/platforms/null/.libs"
export HYBRIS_LINKER_DIR="$HYBRIS/common/q/.libs"
export LD_LIBRARY_PATH="$HYBRIS/egl/.libs:$HYBRIS/glesv2/.libs:$HYBRIS/common/.libs:$HYBRIS/egl/platforms/common/.libs:$HYBRIS/egl/platforms/null/.libs:$HYBRIS/platforms/common/.libs:$HYBRIS/hardware/.libs:$HYBRIS/gralloc/.libs:$HYBRIS/ui/.libs:$HYBRIS/libsync/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ "$#" -gt 0 ]; then
   exec "$@"
fi

exec "$PROBE"
