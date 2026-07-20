#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

usage() {
    echo "usage: $0 INPUT.h264 OUTPUT_DIR [REPETITIONS] [MODE ...]" >&2
    echo "modes: strict release-fence remap-latest" >&2
}

if (( $# < 2 )); then
    usage
    exit 2
fi

input=$1
output_dir=$2
repetitions=${3:-3}
shift "$(( $# >= 3 ? 3 : 2 ))"

if (( $# )); then
    modes=("$@")
else
    modes=(strict remap-latest)
fi

service=${TENSOR_SERVICE:-"$HOME/mediacodec-service-perf"}
client=${TENSOR_LIFECYCLE_CLIENT:-/tmp/surface-lifecycle-bench}
socket_host=${TENSOR_SOCKET_HOST:-"$PREFIX/tmp/tensor-mediacodec-bench.sock"}
socket_guest=${TENSOR_SOCKET_GUEST:-/tmp/tensor-mediacodec-bench.sock}
width=${TENSOR_WIDTH:-1920}
height=${TENSOR_HEIGHT:-1080}
fps=${TENSOR_FPS:-60}
surface_pool=${TENSOR_SURFACE_POOL:-8}

[[ -r "$input" ]] || { echo "input is not readable: $input" >&2; exit 2; }
[[ -x "$service" ]] || { echo "service is not executable: $service" >&2; exit 2; }
[[ "$repetitions" =~ ^[1-9][0-9]*$ ]] || { echo "invalid repetitions: $repetitions" >&2; exit 2; }

mkdir -p "$output_dir"

run_once() {
    local mode=$1
    local run_id=$2
    local run_dir=$3
    shift 3

    local service_log="$run_dir/$run_id-service.log"
    local service_metrics="$run_dir/$run_id-service.jsonl"
    local client_log="$run_dir/$run_id-client.log"

    env TENSOR_PERF_OUTPUT="$service_metrics" "$@" \
        "$service" "$socket_host" --once >"$service_log" 2>&1 &
    local service_pid=$!

    local ready=0
    for _ in {1..100}; do
        if [[ -S "$socket_host" ]]; then
            ready=1
            break
        fi
        if ! kill -0 "$service_pid" 2>/dev/null; then
            break
        fi
        sleep 0.02
    done
    if (( ! ready )); then
        wait "$service_pid" || true
        echo "$run_id service did not create its socket" >&2
        return 1
    fi

    set +e
    local client_env=()
    if [[ "$mode" == release-fence ]]; then
        client_env=(env TENSOR_VA_DMA_HEAP=system-uncached)
    fi
    udroid login jammy:raw "${client_env[@]}" "$client" "$socket_guest" \
        /tmp/"$(basename "$input")" "$width" "$height" "$fps" \
        "$surface_pool" >"$client_log" 2>&1
    local client_status=$?
    wait "$service_pid"
    local service_status=$?
    set -e

    if (( client_status != 0 || service_status != 0 )); then
        echo "$run_id failed: client=$client_status service=$service_status" >&2
        return 1
    fi
}

for mode in "${modes[@]}"; do
    case "$mode" in
        strict) mode_env=() ;;
        release-fence) mode_env=(TENSOR_MEDIACODEC_RELEASE_FENCE=1) ;;
        remap-latest) mode_env=(TENSOR_MEDIACODEC_REMAP_LATEST=1) ;;
        *) echo "unknown mode: $mode" >&2; usage; exit 2 ;;
    esac

    mode_dir="$output_dir/$mode"
    mkdir -p "$mode_dir"
    echo "warmup: $mode" >&2
    run_once "$mode" warmup "$mode_dir" "${mode_env[@]}"

    : >"$mode_dir/benchmark.jsonl"
    : >"$mode_dir/service.jsonl"
    for ((rep = 1; rep <= repetitions; rep++)); do
        run_id="$mode-$rep"
        echo "measured: $run_id" >&2
        run_once "$mode" "$run_id" "$mode_dir" "${mode_env[@]}"
        sed -n '/^{/p' "$mode_dir/$run_id-client.log" \
            >>"$mode_dir/benchmark.jsonl"
        sed -n '/^{/p' "$mode_dir/$run_id-service.jsonl" \
            >>"$mode_dir/service.jsonl"
    done
done
