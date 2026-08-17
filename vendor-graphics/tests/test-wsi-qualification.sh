#!/usr/bin/env bash
# Mock executable bodies below are intentionally single-quoted so their
# variables expand inside the generated fixtures.
# shellcheck disable=SC2016
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
qualifier=$root/vendor-graphics/tools/run-wsi-qualification.sh
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

make_executable "$temporary/thread" \
    'total=$(($1 * $2))' \
    'printf "PASS threads=%s cycles-per-thread=%s total-lifecycles=%s tls-isolated=true host-canary=true failures=0\n" "$1" "$2" "$total"'

make_executable "$temporary/protocol" \
    'printf '\''%s\n'\'' '\''{"connected":true,"propertyPresent":true,"version":1,"capabilities":15,"ahbSocket":true,"rgba":true,"syncFileAcquire":true,"gpuCopy":true,"bgraModifier":1,"rgbaModifier":2,"compatible":true}'\'''

make_executable "$temporary/stats" \
    'count=0' \
    '[[ -f $STATS_COUNTER ]] && read -r count < "$STATS_COUNTER"' \
    'count=$((count + 1))' \
    'printf "%s\n" "$count" > "$STATS_COUNTER"' \
    'attempts=0; ((count > 1)) && attempts=30' \
    'printf '\''{"version":1,"attempts":%s,"offloads":%s,"fallbacks":{"disabled":0,"rendererUnavailable":0,"bufferUnsupported":0,"regionUnsupported":0,"queueFull":0},"consistent":true}\n'\'' "$attempts" "$attempts"'

make_executable "$temporary/present" \
    'arguments=" $* "' \
    'printf '\''PASS stage=physical-device device=Mock-GPU api=1.1.0\n'\''' \
    'case "$arguments" in' \
    '  *" --create-destroy-cycles "*) printf '\''PASS stage=create-destroy cycles=20\n'\'' ;;' \
    '  *" --resize-after-frames "*) printf '\''PASS stage=resize-recreate\n'\'' ;;' \
    '  *" --destroy-window-before-capabilities "*) printf '\''PASS stage=lost-surface-capabilities\n'\'' ;;' \
    '  *" --destroy-window-after-frames "*) printf '\''PASS stage=live-surface-loss\n'\'' ;;' \
    '  *" --disconnect-x-after-frames "*) printf '\''PASS stage=x-connection-loss\n'\'' ;;' \
    '  *) printf '\''PASS stage=present frames=30 duration_ms=500 fps=60\n'\'' ;;' \
    'esac' \
    'printf '\''PASS stage=clean-exit\n'\'''

export STATS_COUNTER=$temporary/stats-counter
result=$temporary/result.json
"$qualifier" \
    --present-probe "$temporary/present" \
    --protocol-probe "$temporary/protocol" \
    --stats-probe "$temporary/stats" \
    --thread-probe "$temporary/thread" \
    --profile smoke \
    --output "$result" \
    --log "$temporary/result.log"

python3 - "$result" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    result = json.load(handle)
thread = result["results"]["vulkanThreadLifecycle"]
assert thread == {
    "threads": 8,
    "cyclesPerThread": 25,
    "totalLifecycles": 200,
    "tlsIsolated": True,
    "hostCanary": True,
    "pass": True,
}
assert result["results"]["presentOffload"]["attemptsDelta"] == 30
assert result["results"]["presentOffload"]["offloadsDelta"] == 30
PY

printf 'PASS WSI qualification requires TLS lifecycle and GPU-offloaded Present\n'
