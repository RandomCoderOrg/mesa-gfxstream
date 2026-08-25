# Rootless vendor graphics

This directory contains device-neutral probes and qualification records for the
rootless Android vendor Vulkan bridge. Tensor-specific experiments remain under
`tensor-g1/`; only work that is intended to apply across Android GPU vendors
belongs here.

The first supported target is an AArch64 glibc guest on an ARM64 Android device
with Vulkan and AHardwareBuffer support. A device is never approved from its
model name alone: it must pass the same capability and presentation probes as
every other device.

## Baseline collection

Connect one device with ADB and run:

```sh
vendor-graphics/tools/collect-device-baseline.sh \
  --serial DEVICE_SERIAL \
  --output vendor-graphics/device-matrix/DEVICE.json
```

The report records Android identity, kernel/runtime details, graphics device
nodes, Vulkan HAL candidates, and what the uDroid app domain can actually
access. It intentionally does not infer that graphics work merely because a
Mali, Adreno, or PowerVR node exists.

Derive a proposed runtime route from a baseline with:

```sh
jq -f vendor-graphics/tools/derive-runtime-profile.jq \
  vendor-graphics/device-matrix/DEVICE.json
```

This result is a probe plan, not approval. It chooses standard DRM ahead of the
vendor bridge when a writable render node exists and otherwise proposes the
vendor Vulkan AHardwareBuffer route only when its prerequisites are visible.
Every listed runtime probe must still pass on the current Android build.

An independently selectable gfxstream/Kumquat investigation lives under
[`experiments/gfxstream`](experiments/gfxstream/README.md). It now preserves a
complete AHardwareBuffer across Rutabaga's C ABI, but it does not yet implement
the scanout lifecycle required for a desktop and is not a runtime profile.

## Runtime profile

The packaged `vendor-graphics/runtime/bin/udroid-gpu-run` wrapper applies the
matched Mesa/Zink, libhybris, sysvk, and WSI environment to one command. It
does not edit the guest or replace distribution libraries. Android's normal
Vulkan HAL lookup remains first; when that lookup is unavailable in a rootless
guest, the wrapper resolves Android's Vulkan, board, and hardware properties,
then uses a sole readable 64-bit HAL only as the final fallback. An ambiguous
device fails closed until qualification supplies an explicit module. The same
selected module is initialized through `HYBRIS_TLS_PRELOAD` before application
`main()`, while the packaged `libhybris-common.so.1` isolates its Bionic TLS
from the glibc process.

## WSI qualification

After the selected runtime profile has configured `DISPLAY`, Vulkan, and the
WSI layer, run the same lifecycle suite on every device/build pair:

```sh
vendor-graphics/tools/run-wsi-qualification.sh \
  --present-probe /path/to/vulkan-xcb-present \
  --protocol-probe /path/to/x11-buffer-transport-protocol \
  --stats-probe /path/to/x11-present-stats \
  --thread-probe /path/to/vulkan-thread-lifecycle \
  --profile smoke \
  --output wsi-smoke.json
```

Use `smoke` when admitting a new device and `full` before promoting a profile.
The result is machine-readable JSON and points to the complete raw transcript.
The suite snapshots the server's cumulative Present counters before and after
the workload; AHardwareBuffer qualification requires a non-zero attempt delta
with every attempted copy GPU-offloaded.
The suite does not select or modify drivers; that boundary lets one probe
contract compare standard DRM, vendor Vulkan, and future graphics routes.

## Native Termux Zink launcher

`termux/bin/udroid-zink-run` runs one native Termux X11 application with the
installed Android vendor-Vulkan wrapper and Mesa Zink. It is deliberately
separate from `runtime/bin/udroid-gpu-run`, which configures the glibc guest
bridge and WSI layer.

Install and verify it from Termux:

```sh
install -Dm755 vendor-graphics/termux/bin/udroid-zink-run \
  "$PREFIX/bin/udroid-zink-run"
udroid-zink-run --check
udroid-zink-run -- glxinfo -B
udroid-zink-run -- glxgears
```

The launcher checks the selected ICD library, Zink DRI driver, and local X11
socket before executing the application. `UDROID_VULKAN_ICD=/path/to/icd.json`
or `--icd FILE` selects a non-default wrapper; `--display :1` selects another
Termux:X11 display. Application-specific scaling, GL-version overrides, and
emulator settings remain outside this wrapper.
