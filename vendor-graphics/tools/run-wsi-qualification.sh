#!/usr/bin/env bash
set -euo pipefail

usage()
{
    cat >&2 <<'EOF'
Usage: run-wsi-qualification.sh [OPTIONS]

Run the device-neutral Vulkan/X11 WSI qualification suite inside a configured
Linux guest.

Options:
  --present-probe PATH   vulkan-xcb-present binary
  --protocol-probe PATH  x11-buffer-transport-protocol binary
  --profile NAME         smoke or full (default: smoke)
  --output FILE          JSON result (default: stdout)
  --log FILE             raw probe transcript
  -h, --help             show this help

The caller must supply DISPLAY and the selected Vulkan/WSI runtime environment.
EOF
}

present_probe=./vulkan-xcb-present
protocol_probe=./x11-buffer-transport-protocol
profile=smoke
output=-
log=

while (($#)); do
    case "$1" in
        --present-probe)
            (($# >= 2)) || { usage; exit 2; }
            present_probe=$2
            shift 2
            ;;
        --protocol-probe)
            (($# >= 2)) || { usage; exit 2; }
            protocol_probe=$2
            shift 2
            ;;
        --profile)
            (($# >= 2)) || { usage; exit 2; }
            profile=$2
            shift 2
            ;;
        --output)
            (($# >= 2)) || { usage; exit 2; }
            output=$2
            shift 2
            ;;
        --log)
            (($# >= 2)) || { usage; exit 2; }
            log=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

case "$profile" in
    smoke)
        steady_frames=30
        create_destroy_cycles=20
        resize_processes=3
        lost_query_processes=3
        live_loss_processes=5
        connection_loss_processes=5
        ;;
    full)
        steady_frames=60
        create_destroy_cycles=100
        resize_processes=20
        lost_query_processes=25
        live_loss_processes=50
        connection_loss_processes=25
        ;;
    *)
        echo "Unknown profile: $profile" >&2
        exit 2
        ;;
esac

[[ -x $present_probe ]] || {
    echo "Present probe is not executable: $present_probe" >&2
    exit 3
}
[[ -x $protocol_probe ]] || {
    echo "Protocol probe is not executable: $protocol_probe" >&2
    exit 3
}

if [[ -z $log ]]; then
    if [[ $output == - ]]; then
        log="${TMPDIR:-/tmp}/udroid-wsi-qualification-$$.log"
    else
        log=${output%.json}.log
    fi
fi
mkdir -p "$(dirname -- "$log")"
: > "$log"

fail()
{
    local stage=$1
    echo "FAIL stage=$stage log=$log" >&2
    exit 1
}

last_output=
run_probe()
{
    local stage=$1
    shift
    {
        printf 'BEGIN stage=%s command=' "$stage"
        printf '%q ' "$@"
        printf '\n'
    } >> "$log"
    if ! last_output=$("$@" 2>&1); then
        printf '%s\nEND stage=%s result=fail\n' "$last_output" "$stage" >> "$log"
        fail "$stage"
    fi
    printf '%s\nEND stage=%s result=pass\n' "$last_output" "$stage" >> "$log"
    [[ $last_output == *"PASS stage=clean-exit"* ]] || fail "$stage-clean-exit"
}

repeat_probe()
{
    local count=$1
    local stage=$2
    local required=$3
    shift 3
    local iteration
    for ((iteration = 1; iteration <= count; ++iteration)); do
        run_probe "$stage-$iteration" "$present_probe" "$@"
        [[ $last_output == *"$required"* ]] || fail "$stage-$iteration-result"
    done
    printf 'PASS stage=%s processes=%s\n' "$stage" "$count" >&2
}

suite_start=$SECONDS
protocol_json=$($protocol_probe 2>>"$log") || fail protocol
printf 'PROTOCOL %s\n' "$protocol_json" >> "$log"
[[ $protocol_json == *'"compatible":true'* ]] || fail protocol-compatible
printf 'PASS stage=protocol\n' >&2

run_probe steady "$present_probe" --frames "$steady_frames" --hold-ms 0
[[ $last_output == *"PASS stage=present frames=$steady_frames"* ]] || fail steady-present
gpu=$(sed -n 's/^PASS stage=physical-device device=\(.*\) api=.*/\1/p' <<<"$last_output" | head -1)
api=$(sed -n 's/^PASS stage=physical-device .* api=\([^ ]*\).*/\1/p' <<<"$last_output" | head -1)
steady_duration_ms=$(sed -n 's/^PASS stage=present .* duration_ms=\([^ ]*\) .*/\1/p' <<<"$last_output" | head -1)
steady_fps=$(sed -n 's/^PASS stage=present .* fps=\([^ ]*\).*/\1/p' <<<"$last_output" | head -1)
[[ -n $gpu && -n $api && -n $steady_duration_ms && -n $steady_fps ]] || fail steady-parse
printf 'PASS stage=steady frames=%s fps=%s\n' "$steady_frames" "$steady_fps" >&2

run_probe immediate-teardown "$present_probe" \
    --create-destroy-cycles "$create_destroy_cycles" --hold-ms 0
[[ $last_output == *"PASS stage=create-destroy cycles=$create_destroy_cycles"* ]] || \
    fail immediate-teardown-result
printf 'PASS stage=immediate-teardown cycles=%s\n' "$create_destroy_cycles" >&2

repeat_probe "$resize_processes" resize-retirement \
    'PASS stage=resize-recreate' \
    --frames 8 --resize-after-frames 3 --hold-ms 0

repeat_probe "$lost_query_processes" lost-capability-query \
    'PASS stage=lost-surface-capabilities' \
    --destroy-window-before-capabilities --hold-ms 0

repeat_probe "$live_loss_processes" live-surface-loss \
    'PASS stage=live-surface-loss' \
    --frames 4 --destroy-window-after-frames 3 --hold-ms 0

repeat_probe "$connection_loss_processes" x-connection-loss \
    'PASS stage=x-connection-loss' \
    --frames 4 --disconnect-x-after-frames 3 --hold-ms 0

suite_duration_seconds=$((SECONDS - suite_start))
generated_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)

result=$(printf '%s\n' \
    '{' \
    '  "schema": 1,' \
    "  \"generatedAt\": \"$generated_at\"," \
    "  \"profile\": \"$profile\"," \
    "  \"gpu\": \"$gpu\"," \
    "  \"vulkanApi\": \"$api\"," \
    "  \"suiteDurationSeconds\": $suite_duration_seconds," \
    "  \"rawLog\": \"$log\"," \
    "  \"protocol\": $protocol_json," \
    '  "results": {' \
    "    \"steadyPresent\": {\"frames\": $steady_frames, \"durationMs\": $steady_duration_ms, \"fps\": $steady_fps, \"pass\": true}," \
    "    \"immediateTeardown\": {\"cycles\": $create_destroy_cycles, \"pass\": true}," \
    "    \"resizeRetirement\": {\"processes\": $resize_processes, \"passes\": $resize_processes}," \
    "    \"lostCapabilityQuery\": {\"processes\": $lost_query_processes, \"passes\": $lost_query_processes}," \
    "    \"liveSurfaceLoss\": {\"processes\": $live_loss_processes, \"passes\": $live_loss_processes}," \
    "    \"xConnectionLoss\": {\"processes\": $connection_loss_processes, \"passes\": $connection_loss_processes}" \
    '  }' \
    '}')

if [[ $output == - ]]; then
    printf '%s\n' "$result"
else
    mkdir -p "$(dirname -- "$output")"
    printf '%s\n' "$result" > "$output"
    printf 'PASS stage=suite output=%s log=%s\n' "$output" "$log" >&2
fi
