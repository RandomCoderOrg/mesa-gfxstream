#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail

usage() {
    echo "usage: $0 INPUT.h264 OUTPUT_DIR [FRAMES]" >&2
}

if (( $# < 2 || $# > 3 )); then
    usage
    exit 2
fi

input=$1
output_dir=$2
frames=${3:-0}
service=${TENSOR_SERVICE:-"$HOME/mediacodec-service-perf"}
driver_path=${TENSOR_VA_DRIVER_PATH:-/opt/tensor-va/lib/dri}
socket_host=${TENSOR_SOCKET_HOST:-"$PREFIX/tmp/tensor-mediacodec-va-correctness.sock"}
socket_guest=${TENSOR_SOCKET_GUEST:-/tmp/tensor-mediacodec-va-correctness.sock}
input_guest=${TENSOR_INPUT_GUEST:-/tmp/"$(basename "$input")"}
timeout_seconds=${TENSOR_TIMEOUT_SECONDS:-60}

[[ -r "$input" ]] || { echo "input is not readable: $input" >&2; exit 2; }
[[ -x "$service" ]] || { echo "service is not executable: $service" >&2; exit 2; }
[[ "$frames" =~ ^[0-9]+$ ]] || { echo "invalid frame count: $frames" >&2; exit 2; }
[[ "$timeout_seconds" =~ ^[1-9][0-9]*$ ]] || {
    echo "invalid timeout: $timeout_seconds" >&2
    exit 2
}

mkdir -p "$output_dir"
work_dir=$(mktemp -d "$PREFIX/tmp/tensor-va-correctness.XXXXXX")
work_guest=/tmp/"$(basename "$work_dir")"
service_pid=

cleanup() {
    if [[ -n "$service_pid" ]] && kill -0 "$service_pid" 2>/dev/null; then
        kill "$service_pid" 2>/dev/null || true
        wait "$service_pid" 2>/dev/null || true
    fi
    unlink "$socket_host" 2>/dev/null || true
    rm -rf "$work_dir"
}
trap cleanup EXIT

service_log="$work_dir/service.log"
service_metrics="$work_dir/service.jsonl"
hardware="$work_dir/hardware.framemd5"
software="$work_dir/software.framemd5"

unlink "$socket_host" 2>/dev/null || true
TENSOR_MEDIACODEC_RELEASE_FENCE=1 TENSOR_PERF_OUTPUT="$service_metrics" \
    "$service" "$socket_host" --once >"$service_log" 2>&1 &
service_pid=$!

ready=0
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
    wait "$service_pid" 2>/dev/null || true
    service_pid=
    cp "$service_log" "$output_dir/service.log"
    echo "service did not create its socket" >&2
    exit 1
fi

frame_args=()
if (( frames )); then
    frame_args=(-frames:v "$frames")
fi

started_ns=$(date +%s%N)
set +e
timeout "$timeout_seconds" udroid login jammy:raw env \
    LIBVA_DRIVER_NAME=tensor \
    LIBVA_DRIVERS_PATH="$driver_path" \
    TENSOR_MEDIACODEC_SOCKET="$socket_guest" \
    TENSOR_VA_DMA_HEAP=system-uncached \
    TENSOR_VA_DEFERRED_EXPORT=1 \
    ffmpeg -hide_banner -loglevel error \
        -hwaccel vaapi -hwaccel_device /dev/mali0 \
        -hwaccel_output_format vaapi \
        -i "$input_guest" -vf hwdownload,format=nv12 \
        "${frame_args[@]}" -f framemd5 "$work_guest/hardware.framemd5"
hardware_status=$?

if (( hardware_status == 0 )); then
    wait "$service_pid"
    service_status=$?
else
    kill "$service_pid" 2>/dev/null || true
    wait "$service_pid" 2>/dev/null
    service_status=$?
fi
service_pid=

software_status=1
if (( hardware_status == 0 )); then
    timeout "$timeout_seconds" udroid login jammy:raw ffmpeg \
        -hide_banner -loglevel error -i "$input_guest" -pix_fmt nv12 \
        "${frame_args[@]}" -f framemd5 "$work_guest/software.framemd5"
    software_status=$?
fi
set -e
elapsed_ns=$(( $(date +%s%N) - started_ns ))

hardware_frames=0
software_frames=0
exact_match=false
if [[ -s "$hardware" && -s "$software" ]]; then
    awk '!/^#/ { gsub(/[[:space:]]/, ""); print }' "$hardware" \
        >"$work_dir/hardware.rows"
    awk '!/^#/ { gsub(/[[:space:]]/, ""); print }' "$software" \
        >"$work_dir/software.rows"
    hardware_frames=$(wc -l <"$work_dir/hardware.rows")
    software_frames=$(wc -l <"$work_dir/software.rows")
    if cmp -s "$work_dir/hardware.rows" "$work_dir/software.rows"; then
        exact_match=true
    fi
fi

teardown_clean=true
if grep -q -E 'FORTIFY|destroyed mutex|Fatal signal' "$service_log"; then
    teardown_clean=false
fi

passed=false
if (( hardware_status == 0 && service_status == 0 && software_status == 0 &&
      hardware_frames > 0 && hardware_frames == software_frames )) &&
   [[ "$exact_match" == true && "$teardown_clean" == true ]]; then
    passed=true
fi

cp "$service_log" "$output_dir/service.log"
cp "$service_metrics" "$output_dir/service.jsonl" 2>/dev/null || true
cp "$hardware" "$output_dir/hardware.framemd5" 2>/dev/null || true
cp "$software" "$output_dir/software.framemd5" 2>/dev/null || true

printf '{"schema":"tensor-perf-v1","kind":"correctness","name":"vaapi-framemd5","elapsed_ns":%s,"requested_frames":%s,"hardware_frames":%s,"software_frames":%s,"hardware_status":%s,"service_status":%s,"software_status":%s,"exact_match":%s,"teardown_clean":%s,"passed":%s}\n' \
    "$elapsed_ns" "$frames" "$hardware_frames" "$software_frames" \
    "$hardware_status" "$service_status" "$software_status" \
    "$exact_match" "$teardown_clean" "$passed" \
    | tee "$output_dir/result.jsonl"

[[ "$passed" == true ]]
