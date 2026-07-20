#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

usage() {
    echo "usage: $0 INPUT.h264 OUTPUT_DIR [REPETITIONS] [MODE ...]" >&2
    echo "modes: byte-buffer private-ahb cpu-readable-ahb" >&2
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
    modes=(byte-buffer private-ahb cpu-readable-ahb)
fi

decoder=${TENSOR_DECODER:-"$HOME/mediacodec-decode-perf"}
sampler=${TENSOR_SAMPLER:-"$HOME/tensor-perf-src/process_sampler.py"}
codec=${TENSOR_CODEC:-c2.exynos.h264.decoder}
width=${TENSOR_WIDTH:-1920}
height=${TENSOR_HEIGHT:-1080}
fps=${TENSOR_FPS:-60}

[[ -r "$input" ]] || { echo "input is not readable: $input" >&2; exit 2; }
[[ -x "$decoder" ]] || { echo "decoder is not executable: $decoder" >&2; exit 2; }
[[ -r "$sampler" ]] || { echo "sampler is not readable: $sampler" >&2; exit 2; }
[[ "$repetitions" =~ ^[1-9][0-9]*$ ]] || { echo "invalid repetitions: $repetitions" >&2; exit 2; }

mkdir -p "$output_dir"

run_decoder() {
    local mode=$1
    shift
    env TENSOR_MEDIACODEC_QUIET=1 "$@" \
        "$decoder" "$input" "$codec" "$width" "$height" "$fps"
}

for mode in "${modes[@]}"; do
    case "$mode" in
        byte-buffer) mode_env=() ;;
        private-ahb) mode_env=(TENSOR_MEDIACODEC_PRIVATE=1) ;;
        cpu-readable-ahb) mode_env=(TENSOR_MEDIACODEC_SURFACE=1) ;;
        *) echo "unknown mode: $mode" >&2; usage; exit 2 ;;
    esac

    mode_dir="$output_dir/$mode"
    mkdir -p "$mode_dir"
    echo "warmup: $mode" >&2
    run_decoder "$mode" "${mode_env[@]}" >"$mode_dir/warmup.log" 2>&1

    : >"$mode_dir/benchmark.jsonl"
    : >"$mode_dir/process.jsonl"
    for ((rep = 1; rep <= repetitions; rep++)); do
        run_id="$mode-$rep"
        log="$mode_dir/$run_id.log"
        echo "measured: $run_id" >&2
        run_decoder "$mode" "${mode_env[@]}" >"$log" 2>&1 &
        decoder_pid=$!
        python3 "$sampler" \
            --duration 15 --interval-ms 250 --pid "$decoder_pid" \
            --run-id "$run_id" --output "$mode_dir/process.jsonl" &
        sampler_pid=$!

        set +e
        wait "$decoder_pid"
        decoder_status=$?
        kill -TERM "$sampler_pid" 2>/dev/null
        wait "$sampler_pid"
        sampler_status=$?
        set -e

        sed -n '/^{/p' "$log" >>"$mode_dir/benchmark.jsonl"
        if (( decoder_status != 0 || sampler_status != 0 )); then
            echo "$run_id failed: decoder=$decoder_status sampler=$sampler_status" >&2
            exit 1
        fi
    done
done
