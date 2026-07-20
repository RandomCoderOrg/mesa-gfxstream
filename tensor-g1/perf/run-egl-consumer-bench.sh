#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

usage() {
    echo "usage: $0 INPUT.h264 OUTPUT_DIR [REPETITIONS] [off|on ...]" >&2
    echo "input must contain no more access units than TENSOR_SURFACE_POOL" >&2
}

if (( $# < 2 )); then
    usage
    exit 2
fi

input=$1
output_dir=$2
repetitions=${3:-5}
shift "$(( $# >= 3 ? 3 : 2 ))"
if (( $# )); then
    modes=("$@")
else
    modes=(off on)
fi

service=${TENSOR_SERVICE:-"$HOME/mediacodec-service-perf"}
client=${TENSOR_EGL_CLIENT:-/tmp/surface-lifecycle-egl-bench}
driver_path=${TENSOR_PANFROST_DRIVER_PATH:-/opt/panfork-tensor/lib/aarch64-linux-gnu/dri}
library_path=${TENSOR_PANFROST_LIBRARY_PATH:-/opt/panfork-tensor/lib/aarch64-linux-gnu}
socket_host=${TENSOR_SOCKET_HOST:-"$PREFIX/tmp/tensor-egl-consumer.sock"}
socket_guest=${TENSOR_SOCKET_GUEST:-/tmp/tensor-egl-consumer.sock}
input_guest=${TENSOR_INPUT_GUEST:-/tmp/"$(basename "$input")"}
width=${TENSOR_WIDTH:-1920}
height=${TENSOR_HEIGHT:-1080}
fps=${TENSOR_FPS:-60}
surface_pool=${TENSOR_SURFACE_POOL:-16}

[[ -r "$input" ]] || { echo "input is not readable: $input" >&2; exit 2; }
[[ -x "$service" ]] || { echo "service is not executable: $service" >&2; exit 2; }
[[ "$repetitions" =~ ^[1-9][0-9]*$ ]] || { echo "invalid repetitions" >&2; exit 2; }
mkdir -p "$output_dir"

service_pid=
cleanup() {
    if [[ -n "$service_pid" ]] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    unlink "$socket_host" 2>/dev/null || true
}
trap cleanup EXIT

run_once() {
    local mode=$1 run_id=$2 run_dir=$3
    local service_log="$run_dir/$run_id-service.log"
    local service_metrics="$run_dir/$run_id-service.jsonl"
    local client_log="$run_dir/$run_id-client.log"
    local wait_env=()
    [[ "$mode" == on ]] && wait_env=(PAN_MALI_DMABUF_SYNC_WAIT=1)

    unlink "$socket_host" 2>/dev/null || true
    TENSOR_MEDIACODEC_RELEASE_FENCE=1 \
    TENSOR_PERF_OUTPUT="$service_metrics" \
        "$service" "$socket_host" --once >"$service_log" 2>&1 &
    service_pid=$!

    local ready=0
    for _ in {1..100}; do
        if [[ -S "$socket_host" ]]; then
            ready=1
            break
        fi
        kill -0 "$service_pid" 2>/dev/null || break
        sleep 0.02
    done
    (( ready )) || { wait "$service_pid" || true; service_pid=; return 1; }

    set +e
    udroid login jammy:raw env \
        TENSOR_EGL_CONSUMER=1 \
        TENSOR_VA_DMA_HEAP=system-uncached \
        "${wait_env[@]}" \
        LIBGL_DRIVERS_PATH="$driver_path" \
        LD_LIBRARY_PATH="$library_path" \
        "$client" "$socket_guest" "$input_guest" \
        "$width" "$height" "$fps" "$surface_pool" \
        >"$client_log" 2>&1
    local client_status=$?
    wait "$service_pid"
    local service_status=$?
    service_pid=
    set -e

    sed -n '/^{/p' "$client_log"
    (( service_status == 0 )) || return 1
    if [[ "$mode" == on ]]; then
        (( client_status == 0 ))
    else
        (( client_status == 0 || client_status == 1 ))
    fi
}

for mode in "${modes[@]}"; do
    [[ "$mode" == off || "$mode" == on ]] || { usage; exit 2; }
    mode_dir="$output_dir/$mode"
    mkdir -p "$mode_dir"
    run_once "$mode" warmup "$mode_dir" >/dev/null
    : >"$mode_dir/benchmark.jsonl"
    for ((rep = 1; rep <= repetitions; rep++)); do
        run_once "$mode" "$mode-$rep" "$mode_dir" \
            | tee -a "$mode_dir/benchmark.jsonl"
    done
done
