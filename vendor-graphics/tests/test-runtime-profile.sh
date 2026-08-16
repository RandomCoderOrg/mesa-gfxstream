#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
fixture=$root/vendor-graphics/device-matrix/exynos-9611-sm-m315f-android13.json
profile=$root/vendor-graphics/tools/derive-runtime-profile.jq

derive()
{
    jq "$1" "$fixture" | jq -f "$profile"
}

assert_value()
{
    local name=$1 transform=$2 query=$3 expected=$4 actual
    actual=$(derive "$transform" | jq -r "$query")
    if [[ $actual != "$expected" ]]; then
        printf 'FAIL %s: expected %s, got %s\n' "$name" "$expected" "$actual" >&2
        return 1
    fi
    printf 'PASS %s=%s\n' "$name" "$actual"
}

assert_value exynos-route '.' '.selectedRoute' vendor-vulkan-ahb
assert_value aarch64-hal-only '.' \
    '[.vulkanHal.fallbackCandidates[] | contains("/lib64/")] | all' true
assert_value drm-priority \
    '.appDomain.deviceAccess += [{path:"/dev/dri/renderD128",exists:true,readable:true,writable:true}]' \
    '.selectedRoute' standard-drm
assert_value reject-32bit-hal \
    '.discovery.vulkanHalCandidates |= map(select(.path | contains("/lib64/") | not))' \
    '.selectedRoute' probe-required
assert_value reject-pre-ahb \
    '.android.sdk = 25' '.selectedRoute' probe-required
assert_value reject-non-aarch64 \
    '.android.abiList = ["armeabi-v7a"]' '.selectedRoute' probe-required
