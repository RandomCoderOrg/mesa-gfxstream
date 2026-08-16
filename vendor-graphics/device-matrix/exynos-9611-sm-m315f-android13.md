# Samsung SM-M315F / Exynos 9611 / Android 13

This is the first non-Tensor qualification target for the rootless vendor
graphics bridge. The machine-readable discovery record is stored beside this
file.

## Current result

| Boundary | Result | Evidence |
| --- | --- | --- |
| App permissions | Pass | `/dev/mali0` and `/dev/ion` are readable and writable from the uDroid app domain |
| Vendor Vulkan | Pass | Mali-G72, Vulkan 1.1.213 through the rootless sysvk/libhybris delegate |
| AHardwareBuffer lifecycle | Pass | 90/90 allocations and imports across odd and aligned widths |
| Vulkan lifecycle | Pass | 50/50 device create/destroy cycles |
| X11 protocol | Pass | DRI3 1.2 and Present 1.2 |
| AHB Vulkan XCB Present | Pass | Red frame, clean exit, Termux:X11 reports 1/1 copies GPU-offloaded |
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
- Use `libnativewindow.so` for AHardwareBuffer entry points on this build.
- Keep the AHardwareBuffer producer alive until the consumer has completed its
  import and presentation work.
- Allocate an RGBA-capable physical AHardwareBuffer and carry the guest's RGBA
  channel semantic independently. This device rejects the BGRA AHB request.
- Do not approve Zink or a compositor merely because the direct Vulkan Present
  probe passes; those are separate qualification stages.
