#!/usr/bin/env bash
set -euo pipefail

usage()
{
    cat >&2 <<'EOF'
Usage: collect-device-baseline.sh --serial SERIAL [--package PACKAGE] [--output FILE]

Collects a machine-readable Android graphics discovery baseline over ADB.
The default package is org.randomcoder.udroid and the default output is stdout.
EOF
}

serial=
package=org.randomcoder.udroid
output=-

while (($#)); do
    case "$1" in
        --serial)
            (($# >= 2)) || { usage; exit 2; }
            serial=$2
            shift 2
            ;;
        --package)
            (($# >= 2)) || { usage; exit 2; }
            package=$2
            shift 2
            ;;
        --output)
            (($# >= 2)) || { usage; exit 2; }
            output=$2
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

[[ -n $serial ]] || { usage; exit 2; }
command -v jq >/dev/null || { echo "jq is required" >&2; exit 3; }

adb=${ADB:-adb}
[[ -x $adb ]] || command -v "$adb" >/dev/null || {
    echo "ADB executable not found: $adb" >&2
    exit 3
}

adb_cmd=("$adb" -s "$serial")
state=$("${adb_cmd[@]}" get-state 2>/dev/null || true)
[[ $state == device ]] || {
    echo "ADB device is not ready: $serial ($state)" >&2
    exit 4
}

prop()
{
    "${adb_cmd[@]}" shell getprop "$1" 2>/dev/null | tr -d '\r'
}

shell_line()
{
    "${adb_cmd[@]}" shell "$@" 2>/dev/null | tr -d '\r'
}

manufacturer=$(prop ro.product.manufacturer)
model=$(prop ro.product.model)
device=$(prop ro.product.device)
board=$(prop ro.product.board)
platform=$(prop ro.board.platform)
hardware=$(prop ro.hardware)
soc_manufacturer=$(prop ro.soc.manufacturer)
soc_model=$(prop ro.soc.model)
android_release=$(prop ro.build.version.release)
android_sdk=$(prop ro.build.version.sdk)
build_fingerprint=$(prop ro.build.fingerprint)
vendor_fingerprint=$(prop ro.vendor.build.fingerprint)
abi_list=$(prop ro.product.cpu.abilist)
vndk_version=$(prop ro.vndk.version)
treble=$(prop ro.treble.enabled)
vulkan_property=$(prop ro.hardware.vulkan)
kernel=$(shell_line uname -r)
selinux=$(shell_line getenforce)

device_id=$(printf '%s\n%s\n%s\n%s\n' \
    "$manufacturer" "$model" "$device" "$build_fingerprint" |
    shasum -a 256 | awk '{print substr($1, 1, 16)}')

# The remote scripts are intentionally single quoted so expansion happens on
# Android rather than on the host running this collector.
# shellcheck disable=SC2016
node_report=$(shell_line sh -c '
for path in /dev/mali0 /dev/kgsl-3d0 /dev/dri/renderD128 /dev/dri/card0 \
            /dev/dma_heap/system /dev/dma_heap/system-uncached /dev/ion; do
    if [ -e "$path" ]; then
        listing=$(ls -lZ "$path" 2>/dev/null || ls -l "$path" 2>/dev/null || true)
        printf "%s|%s\n" "$path" "$listing"
    fi
done')

# shellcheck disable=SC2016
hal_report=$(shell_line sh -c '
for directory in /vendor/lib64/hw /odm/lib64/hw /system/lib64/hw; do
    [ -d "$directory" ] || continue
    for path in "$directory"/vulkan*.so; do
        [ -e "$path" ] || continue
        listing=$(ls -l "$path" 2>/dev/null || true)
        printf "%s|%s\n" "$path" "$listing"
    done
done')

run_as_identity=$("${adb_cmd[@]}" shell run-as "$package" id 2>/dev/null | tr -d '\r' || true)
run_as_access=
if [[ -n $run_as_identity ]]; then
    # shellcheck disable=SC2016
    run_as_access=$("${adb_cmd[@]}" shell run-as "$package" sh -c '
for path in /dev/mali0 /dev/kgsl-3d0 /dev/dri/renderD128 /dev/dri/card0 \
            /dev/dma_heap/system /dev/dma_heap/system-uncached /dev/ion; do
    exists=false; readable=false; writable=false
    [ -e "$path" ] && exists=true
    [ -r "$path" ] && readable=true
    [ -w "$path" ] && writable=true
    printf "%s|%s|%s|%s\n" "$path" "$exists" "$readable" "$writable"
done' 2>/dev/null | tr -d '\r' || true)
fi

report=$(jq -n \
    --argjson schema 1 \
    --arg deviceId "$device_id" \
    --arg manufacturer "$manufacturer" \
    --arg model "$model" \
    --arg device "$device" \
    --arg board "$board" \
    --arg platform "$platform" \
    --arg hardware "$hardware" \
    --arg socManufacturer "$soc_manufacturer" \
    --arg socModel "$soc_model" \
    --arg androidRelease "$android_release" \
    --arg androidSdk "$android_sdk" \
    --arg buildFingerprint "$build_fingerprint" \
    --arg vendorFingerprint "$vendor_fingerprint" \
    --arg abiList "$abi_list" \
    --arg vndkVersion "$vndk_version" \
    --arg treble "$treble" \
    --arg vulkanProperty "$vulkan_property" \
    --arg kernel "$kernel" \
    --arg selinux "$selinux" \
    --arg package "$package" \
    --arg runAsIdentity "$run_as_identity" \
    --arg nodes "$node_report" \
    --arg hals "$hal_report" \
    --arg access "$run_as_access" '
    def lines($value): $value | split("\n") | map(select(length > 0));
    {
      schema: $schema,
      deviceId: $deviceId,
      android: {
        manufacturer: $manufacturer,
        model: $model,
        device: $device,
        board: $board,
        platform: $platform,
        hardware: $hardware,
        socManufacturer: $socManufacturer,
        socModel: $socModel,
        release: $androidRelease,
        sdk: ($androidSdk | tonumber),
        buildFingerprint: $buildFingerprint,
        vendorFingerprint: $vendorFingerprint,
        abiList: ($abiList | split(",") | map(select(length > 0))),
        vndkVersion: $vndkVersion,
        trebleEnabled: ($treble == "true"),
        vulkanProperty: $vulkanProperty,
        kernel: $kernel,
        selinux: $selinux
      },
      discovery: {
        deviceNodes: (lines($nodes) | map(split("|") | {path: .[0], listing: .[1]})),
        vulkanHalCandidates: (lines($hals) | map(split("|") | {path: .[0], listing: .[1]}))
      },
      appDomain: {
        package: $package,
        probeAvailable: ($runAsIdentity != ""),
        identity: $runAsIdentity,
        deviceAccess: (lines($access) | map(split("|") | {
          path: .[0],
          exists: (.[1] == "true"),
          readable: (.[2] == "true"),
          writable: (.[3] == "true")
        }))
      }
    }')

if [[ $output == - ]]; then
    printf '%s\n' "$report"
else
    mkdir -p "$(dirname -- "$output")"
    printf '%s\n' "$report" > "$output"
    printf 'Wrote %s\n' "$output" >&2
fi
