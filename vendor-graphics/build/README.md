# Reproducible AArch64 build

`source-lock.json` is the complete external source graph for the first glibc
AArch64 runtime. A build must fetch each repository by the exact 40-character
revision and apply only the ordered patches under `../patches/`.

The build environment starts from the digest-pinned Jammy ARM64 image in
`Dockerfile.jammy-aarch64`. Build and publish that image by digest before a
runtime release; the moving local tag is not release evidence. Component jobs
must install into a fresh staging tree, never copy libtool `.libs` trees, and
then pass `../packaging/build-manifest.py` and `verify-runtime.py`.

`environment-lock.json` records the first successfully built image ID and the
tool versions observed inside it. This is local checkpoint evidence, not a
substitute for publishing the build image by registry digest.

The current closed gates are libhybris and the vendor bridge. Patches 0001
through 0003 apply cleanly, `make install` emits no RPATH/RUNPATH,
`libhybris-common.so.1` carries one 64-128 KiB `PT_TLS` reserve, and the
packaged 8-thread lifecycle probe passes. The pinned Vulkan loader, sysvk and
AHardwareBuffer wrapper also build reproducibly. `build-wsi.sh` adds the eight
ordered xMeM WSI patches and installs the AHardwareBuffer layer as an explicit
layer. The Loader is generated with the matching Vulkan 1.3.204 registry,
while xMeM is compiled against the separately pinned newer declarations it
uses for extension dispatch. The layer may not advertise those extensions
unless its downstream driver supports them. Mesa/Zink still needs to be
assembled into the same clean staging tree before an asset exists.

`build-mesa.sh` fetches the separately locked Mesa revision, applies the
ordered Zink compatibility patches, and stages only the X11 GLX/EGL/GLES
frontends with Zink. It deliberately excludes llvmpipe and lavapipe so a
qualification run cannot silently pass through a software renderer.

The packaged Loader retains XCB and Xlib surface entrypoints. The external WSI
layer replaces their implementation, but applications must first resolve the
standard `vkCreateXcbSurfaceKHR` or `vkCreateXlibSurfaceKHR` trampoline from
the Loader. Disabling Loader-side X11 support prevents layer interception and
is rejected by device Present probes.

## Reproduction checkpoint

On 2026-08-16, `build-libhybris.sh` ran twice from separate empty output
directories. All 17 installed files and symlink targets were byte-identical.
Both trees exposed a 69,632-byte `PT_TLS` segment and no RPATH/RUNPATH. The
second tree then replaced common/hardware/q in an isolated Exynos 9611 device
runtime and passed 8 threads x 25 Vulkan lifecycles (200 total), with Android
TLS initialized and host TLS canaries intact.

The first full-bridge assembly attempt also caught and rejected a mixed source
generation: Vulkan Loader 1.3.204 had been paired with headers reporting
1.4.357. The lock now uses the peeled Vulkan-Headers `v1.3.204` commit. Two
corrected bridge builds produced identical hashes for all 13 binaries and
symlink targets. A runtime containing only the freshly built loader, sysvk,
AHardwareBuffer wrapper, libhybris and lifecycle probe then passed the same
8 x 25 Exynos test through automatic Android-property HAL selection.

xMeM itself references extension structures newer than Vulkan 1.3.204. Its
compile-only headers are therefore locked independently as `wsi_headers`; they
must never be used to generate the 1.3.204 Loader registry.
