# Samsung SM-M315F / Exynos 9611 / Android 13

This is the first non-Tensor qualification target for the rootless vendor
graphics bridge. The machine-readable discovery record is stored beside this
file.

## Current result

| Boundary | Result | Evidence |
| --- | --- | --- |
| App permissions | Pass | `/dev/mali0` and `/dev/ion` are readable and writable from the uDroid app domain |
| Packaged HAL selection | Pass | With `UDROID_VULKAN_HAL` unset, `udroid-gpu-run` resolved `/vendor/lib64/hw/vulkan.universal9611.so` from Android's `ro.board.platform` property and reported Mali-G72, Vulkan 1.1.213 |
| Vendor Vulkan | Pass | Mali-G72, Vulkan 1.1.213 through the rootless sysvk/libhybris delegate |
| AHardwareBuffer lifecycle | Pass | 90/90 allocations and imports across odd and aligned widths |
| Narrow AHB provider transport | Pass | `libnativewindow.so` completed 9/9 allocate/send/receive/release cases; `libandroid.so` aborted while loading unrelated framework dependencies |
| Vulkan lifecycle | Pass | Exact clean libhybris build passed 8 concurrent threads x 100 instance/device lifecycles (800 total) |
| Packaged TLS launch contract | Pass | The relocatable launcher selected the same HAL for sysvk and `HYBRIS_TLS_PRELOAD`; 8 x 25 (200 total) passed with every Android TLS generation initialized and every host TLS canary intact |
| Automatic launch contract | Pass | With both HAL variables unset, Android-property selection succeeded and a further 8 x 5 (40 total) lifecycle run passed |
| Reproduced install | Pass | Two empty-tree builds produced byte-identical 17-entry installs; the second install replaced common/hardware/q in an isolated runtime and passed 8 x 25 lifecycles |
| Reproduced full bridge | Pass | Matched Loader 1.3.204 + sysvk + AHB wrapper + libhybris builds were byte-identical; an isolated all-new bridge passed 8 x 25 lifecycles with automatic HAL selection |
| X11 protocol | Pass | DRI3 1.2 and Present 1.2 |
| AHB Vulkan XCB Present | Pass | Red frame, clean exit, Termux:X11 reports 1/1 copies GPU-offloaded |
| Clean-runtime WSI smoke | Pass | TLS lifecycle, protocol, steady Present, teardown, resize and connection/surface-loss cases all passed; 40/40 new Present attempts were GPU-offloaded |
| Vulkan sync-file import | Pass | Both the xshmfence control and a vendor Vulkan `sync_file` import through DRI3 |
| Explicit acquire ordering | Pass | 25/25 Presents stayed blocked for 250–252 ms, then completed 16–39 ms after producer release |
| Present source release ordering | Pass | Termux:X11 completes its GLES source read before IdleNotify makes the pixmap reusable |
| Renderer lock deadline | Pass | 25/25; matching ~50 ms wall wait, average waiting CPU reduced from 49,760 us to 190 us (99.6%) |
| Serialized steady Present | Pass | 5 x 60 frames, five clean exits, 60.10-63.14 FPS with explicit acquire enabled |
| Normal-mode renderer logging | Pass | 0 per-rectangle hot-path entries, down from 58 writes in the earlier one-second run |
| Current provider CPU readback | Open regression | Transport succeeds, but the full map/readback probe reached a page boundary outside its returned mapping; WSI does not CPU-map these display targets |
| Mesa Zink | Blocked | Jammy Mesa rejects missing `logicOp`, `fillModeNonSolid`, and `shaderClipDistance` features |
| Desktop compositor | Not qualified | Requires a matched Zink build after the feature-policy issue is resolved |

## Device-specific discovery

The Vulkan HAL is exposed as
`/vendor/lib64/hw/vulkan.universal9611.so`, linked to the vendor Mali GLES
library. The device has no standard DRM render node and no DMA-heap node, so
the supported allocation path is AHardwareBuffer backed by Android's graphics
stack rather than a Linux DRM/GBM path.

The HAL path is discovery input, not a hard-coded profile. Normal Android HAL
discovery is attempted first; an explicit HAL override remains a diagnostic
and compatibility fallback.

## Proven compatibility rules

- Load the vendor driver through the isolated Bionic TLS sidecar. Loading it
  directly into a glibc process corrupts TLS state.
- Preload the packaged `libhybris-common.so.1` and set `HYBRIS_TLS_PRELOAD` to
  the same qualified HAL consumed by sysvk. The verified build carries one
  69,632-byte `PT_TLS` segment and no RPATH/RUNPATH.
- Use `libnativewindow.so` for AHardwareBuffer entry points on this build.
- Keep explicit acquire synchronization paired: enable it only with the
  matching Termux:X11 sync-file backend, otherwise retain the CPU fence wait.
- Use `CLOCK_REALTIME` deadlines with `pthread_mutex_timedlock`; a monotonic
  absolute timestamp causes immediate timeout spinning on Bionic.
- Keep the AHardwareBuffer producer alive until the consumer has completed its
  import and presentation work.
- Allocate an RGBA-capable physical AHardwareBuffer and carry the guest's RGBA
  channel semantic independently. This device rejects the BGRA AHB request.
- Do not approve Zink or a compositor merely because the direct Vulkan Present
  probe passes; those are separate qualification stages.

The spec-correct allocation and teardown rerun is recorded in
`exynos-9611-sm-m315f-android13-wsi-2026-08-16.json`.
The latest complete smoke qualification, including the isolated TLS lifecycle
and observable offload deltas, is recorded in
`exynos-9611-sm-m315f-android13-wsi-smoke-v3-2026-08-16.json`.
