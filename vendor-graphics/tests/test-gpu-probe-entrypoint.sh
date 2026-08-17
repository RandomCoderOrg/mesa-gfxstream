#!/usr/bin/env bash
# Mock executable bodies below are intentionally single-quoted so their
# variables expand when the generated fixtures run, not while they are built.
# shellcheck disable=SC2016
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
entrypoint=$root/vendor-graphics/runtime/bin/udroid-gpu-probe
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT

make_executable()
{
    local path=$1
    shift
    mkdir -p "$(dirname -- "$path")"
    printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' "$@" > "$path"
    chmod +x "$path"
}

make_executable "$temporary/runner" \
    'printf '\''%q '\'' "$@" > "$CAPTURED_RUNNER"' \
    '[[ $1 == --profile && $3 == -- ]]' \
    'shift 3' \
    'exec "$@"'
make_executable "$temporary/qualifier" \
    'printf '\''%q '\'' "$@" > "$CAPTURED_QUALIFIER"' \
    'printf '\''{"schema":2,"profile":"full"}\n'\'''
for probe in present protocol stats thread; do
    make_executable "$temporary/$probe" 'exit 0'
done

export DISPLAY=:7
export CAPTURED_RUNNER=$temporary/runner.args
export CAPTURED_QUALIFIER=$temporary/qualifier.args
export UDROID_GPU_RUN=$temporary/runner
export UDROID_WSI_QUALIFIER=$temporary/qualifier
export UDROID_PRESENT_PROBE=$temporary/present
export UDROID_PROTOCOL_PROBE=$temporary/protocol
export UDROID_PRESENT_STATS_PROBE=$temporary/stats
export UDROID_VULKAN_THREAD_PROBE=$temporary/thread

result=$(
    "$entrypoint" \
        --profile vendor-vulkan:ginkage-ahb \
        --qualification full \
        --output - \
        --log "$temporary/raw log.txt"
)

[[ $result == '{"schema":2,"profile":"full"}' ]]
grep -Fq -- '--profile vendor-vulkan:ginkage-ahb --' "$CAPTURED_RUNNER"
grep -Fq -- "--present-probe $temporary/present" "$CAPTURED_QUALIFIER"
grep -Fq -- "--protocol-probe $temporary/protocol" "$CAPTURED_QUALIFIER"
grep -Fq -- "--stats-probe $temporary/stats" "$CAPTURED_QUALIFIER"
grep -Fq -- "--thread-probe $temporary/thread" "$CAPTURED_QUALIFIER"
grep -Fq -- '--profile full --output -' "$CAPTURED_QUALIFIER"
grep -Fq -- "--log $temporary/raw\\ log.txt" "$CAPTURED_QUALIFIER"

if DISPLAY='' "$entrypoint" >/dev/null 2>"$temporary/no-display.log"; then
    echo 'probe unexpectedly accepted an empty DISPLAY' >&2
    exit 1
fi
grep -Fq 'DISPLAY is not set' "$temporary/no-display.log"

printf 'PASS graphics probe entrypoint preserves the selected runtime and qualification arguments\n'
