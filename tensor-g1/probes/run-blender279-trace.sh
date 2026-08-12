#!/bin/sh
set -eu

export DISPLAY=:0
export XDG_RUNTIME_DIR=/tmp
export PYTHONHOME=/root/blender279/usr
export LIBGL_ALWAYS_SOFTWARE=1
export PAN_KBASE_VERBOSE=1
export LD_LIBRARY_PATH=/root/blender279/compat:/root/blender279/usr/lib:/root/blender279/usr/lib/aarch64-linux-gnu:/root/mesa-v9-build/src/gallium/targets/dri:/root/mesa-v9-build/src/gallium/targets/dril:/opt/panfrost-upstream-v9/lib/aarch64-linux-gnu:/opt/spirv-tools-2024.1/lib
export LIBGL_DRIVERS_PATH=/root/mesa-v9-build/src/gallium/targets/dril
export MESA_LOADER_DRIVER_OVERRIDE=panfrost
export GALLIUM_DRIVER=panfrost
export BLENDER_SYSTEM_SCRIPTS=/root/blender279/usr/share/blender/scripts
export BLENDER_SYSTEM_DATAFILES=/root/blender279/usr/share/blender/datafiles

exec timeout "${TENSOR_TRACE_SECONDS:-20}s" \
  /root/blender279/usr/bin/blender --factory-startup \
  /root/blender-scenes/blender_splash_fishy_cat/fishy_cat.blend
