#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

run_id=${1:-manual}
deqp_dir=${DEQP_DIR:-/root/deqp-build-3.2.8/modules/gles31}
mesa_prefix=${MESA_PREFIX:-/opt/panfrost-upstream-v9}
spirv_prefix=${SPIRV_PREFIX:-/opt/spirv-tools-2024.1}
log=${DEQP_LOG:-/root/deqp-gles31-hair-${run_id}.qpa}
case_pattern=${DEQP_CASE:-dEQP-GLES31.functional.texture.texture_buffer.render.as_vertex_texture.*}

cd "$deqp_dir"

export DISPLAY=${DISPLAY:-:0}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp}
export LD_LIBRARY_PATH="$mesa_prefix/lib/aarch64-linux-gnu:$spirv_prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBGL_DRIVERS_PATH="$mesa_prefix/lib/aarch64-linux-gnu/dri"
export MESA_LOADER_DRIVER_OVERRIDE=panfrost
export GALLIUM_DRIVER=panfrost
if [ "${DEQP_PAN_MESA_DEBUG+x}" = x ]; then
   export PAN_MESA_DEBUG=$DEQP_PAN_MESA_DEBUG
else
   unset PAN_MESA_DEBUG
fi

exec ./deqp-gles31 \
   --deqp-case="$case_pattern" \
   --deqp-log-filename="$log" \
   --deqp-surface-width=256 \
   --deqp-surface-height=256 \
   --deqp-visibility=hidden
