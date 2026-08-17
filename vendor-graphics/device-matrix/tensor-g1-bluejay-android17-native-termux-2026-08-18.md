# Google Pixel 6a / Tensor G1 / Android 17 native Termux checkpoint

This checkpoint records the native Bionic Termux graphics route. It is useful
application evidence for the same Android vendor driver, but it does **not**
replace qualification of the glibc libhybris/sysvk/AHardwareBuffer bridge.
The sanitized discovery baseline and derived route are stored beside this file
as `tensor-g1-bluejay-android17.json` and
`tensor-g1-bluejay-android17-profile.json`.

## Discovery result

| Boundary | Result |
| --- | --- |
| Android build | Pixel 6a (`bluejay`), GS101, Android 17 / SDK 37 |
| Standard DRM | `/dev/dri/renderD128` exists, but is neither readable nor writable from the uDroid app domain |
| Vendor GPU | `/dev/mali0` is readable and writable from the uDroid app domain |
| Vendor Vulkan HAL | One AArch64 candidate: `/vendor/lib64/hw/vulkan.mali.so` |
| AHardwareBuffer API | Eligible; Android SDK is newer than 26 |
| Direct allocator | No writable DMA heap is exposed to the app domain; AHardwareBuffer remains the selected allocation route |
| Derived route | `vendor-vulkan-ahb`, pending the full runtime probe ladder |

The presence of a global render node is not sufficient for selecting standard
DRM. Route selection uses the app-domain access result, so this build correctly
falls through to the vendor bridge.

## Native Termux route

The installed native wrapper was selected by
`vendor-graphics/termux/bin/udroid-zink-run`:

```text
OpenGL vendor:   Mesa
OpenGL renderer: zink Vulkan 1.4(Mali-G78 (ARM_PROPRIETARY))
OpenGL version:  3.2 (Core and Compatibility Profile), Mesa 26.0.6
Direct render:   yes
```

The route is:

```text
native Termux application
  -> Mesa Zink
  -> native Bionic vendor-Vulkan wrapper
  -> /vendor/lib64/hw/vulkan.mali.so
  -> Termux:X11
```

Use the wrapper without app-specific overrides:

```sh
udroid-zink-run --check
udroid-zink-run -- glxinfo -B
udroid-zink-run -- glxgears
```

The device still emits Zink's warning that the vendor driver does not expose
`shaderClipDistance`. Applications therefore remain correctness probes until
their rendered output is checked; detecting Mali-G78 alone is not a pass.

## melonDS application checkpoint

melonDS 1.1 ran natively with OpenGL Renderer 1 through Zink. The tested
graphics profiles were:

| Profile | Scale | Improved polygons | Screen filter | Result |
| --- | ---: | --- | --- | --- |
| Native | 1x | off | off | Correct visible output |
| Enhanced | 4x | on | on | Correct captured output, but performance was CPU-limited before JIT was enabled |
| Enhanced | 8x | on | on | Correct captured output, visibly slow |
| Maximum | 16x | on | on | Correct captured 3D frame, visibly slow; process RSS reached about 775 MiB |

The initial native-resolution run produced only 14-17 of 60 target FPS. This
was not a Zink scaling bottleneck: melonDS's compiled AArch64 JIT was disabled.
Enabling the existing JIT raised the same run to 50-61 FPS while Android still
reported thermal status 3. The performance configuration was:

```toml
AudioSync = false
TargetFPS = 60.0
LimitFPS = true

[3D]
Renderer = 1

[3D.GL]
ScaleFactor = 1
BetterPolygons = false
HiresCoordinates = false

[JIT]
Enable = true
FastMemory = true
BranchOptimisations = true
LiteralOptimisations = true
MaxBlockSize = 32

[Screen]
UseGL = true
VSync = true
VSyncInterval = 1
```

melonDS logs `ftruncate(...): Invalid argument` for an Android shared-memory
region, but the JIT remained functional in this run. That warning needs a
separate emulator portability fix; the graphics launcher must not add
application-specific JIT or scaling options.

Two local 30-second screen recordings were retained outside Git:

| Profile | Duration | Bytes | SHA-256 |
| --- | ---: | ---: | --- |
| 8x | 30.027211 s | 73,711,452 | `fba577a0bdf029303d2599c3c67cdb7cb3a2a31f4874be386d5cb3b1761c3304` |
| 4x | 30.038322 s | 74,080,280 | `f6190def420340a340f13d40b614bb2f179493e5d8d5ef1d4340b654b3758335` |

The recordings are diagnostic evidence, not redistributable release assets.
Thermal state, emulator JIT state, and scale must be held constant before using
future recordings for performance comparisons.

## Remaining qualification

- Run the packaged glibc libhybris/sysvk TLS lifecycle on this exact build.
- Validate AHardwareBuffer allocation, handle transport, and explicit fences.
- Require non-zero Termux:X11 GPU Present-offload deltas.
- Qualify matched packaged Zink with deterministic GL and compositor probes.
- Repeat under a cooled, recorded thermal state before publishing performance
  numbers.
