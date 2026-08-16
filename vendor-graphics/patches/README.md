# Patch series

These are the minimal source changes used by the first passing Exynos vendor
Vulkan and AHardwareBuffer WSI proof. They are kept as reviewable patches so a
release build can fetch pinned upstream sources and fail if a patch no longer
applies cleanly.

| Component | Upstream | Pinned revision | Patch |
| --- | --- | --- | --- |
| libhybris | `https://github.com/libhybris/libhybris.git` | `7079712a42ea2754adf747e70c6cc75764c8596e` | `libhybris/0001-complete-locale-and-ui-compatibility.patch` |
| sysvk | `https://github.com/xMeM/sysvk.git` | `23ecd775ed6fe06bb5ac0063b5f981f70c543c67` | `sysvk/0001-discover-and-validate-explicit-vulkan-hal.patch` |
| xMeM WSI | `https://github.com/xMeM/vulkan-wsi-layer.git` | `d5624d42d8b2debbd910ad25662a05c751eb38b7` | `xmem-wsi/0001-port-ahb-x11-wsi-to-glibc-and-rgba-semantics.patch`, `xmem-wsi/0002-pass-sync-file-fences-to-x-present.patch`, `xmem-wsi/0003-negotiate-private-x11-buffer-transport.patch`, `xmem-wsi/0004-own-x11-present-thread-lifecycle.patch`, `xmem-wsi/0005-report-lost-x11-surfaces.patch` |
| Termux:X11 | uDroid's pinned Termux:X11 submodule | recorded by the consuming uDroid revision | `termux-x11/0001-import-rgba-ahardwarebuffer-content-without-swizzle.patch`, `termux-x11/0002-import-linux-sync-file-fences.patch`, `termux-x11/0003-use-realtime-for-timed-mutex-deadline.patch`, `termux-x11/0004-gate-gpu-copy-hot-path-logging.patch`, `termux-x11/0005-advertise-buffer-transport-protocol.patch` |

The xMeM WSI and Termux:X11 patches form one private protocol revision. The X
server publishes `_UDROID_X11_BUFFER_TRANSPORT` on the root window as six
32-bit cardinals: protocol version, capability bits, BGRA modifier low/high,
and RGBA modifier low/high. Version 1 publishes modifiers `1255` and `1257`.
They are transport identifiers, not Linux DRM modifiers. xMeM refuses the
private transport when the property is absent, malformed, unsupported, or
missing a required capability.

The second Termux:X11 patch recognizes Linux `sync_file` FDs in the existing
DRI3 fence import backend while preserving ordinary xshmfence behavior. It is
the server half of the optional explicit-acquire synchronization path; the CPU
Vulkan-fence wait remains the safe client fallback.

The second xMeM patch is the paired producer half for sync-file acquire fences.
The third patch negotiates those fences automatically from the server property.
`UDROID_X11_EXPLICIT_SYNC=0` forces the validated CPU Vulkan-fence wait;
`UDROID_X11_EXPLICIT_SYNC=1` is strict and fails if the advertised peer lacks
the capability. `UDROID_X11_PROTOCOL=legacy` is a lab-only escape hatch for an
older paired server and must not be selected automatically by packaging.

The fourth xMeM patch makes the Present event worker an explicitly owned
resource. Swapchain teardown joins a constructed thread even if it has not run
yet, special-event registration is initialized and checked, and partial setup
unwinds without leaving a worker or XCB registration behind.

The fifth xMeM patch converts a failed X geometry query into
`VK_ERROR_SURFACE_LOST_KHR` for both surface-capability entry points. Without
it, a destroyed window returned success with an undefined current extent.

The third Termux:X11 patch fixes a separate POSIX contract at the renderer/X
server lock boundary. `pthread_mutex_timedlock()` consumes an absolute
`CLOCK_REALTIME` deadline; passing a monotonic timestamp made every contended
attempt expire immediately and spin. The patch retains the same 33 ms recovery
interval while allowing Bionic to sleep between retries.

The fourth Termux:X11 patch removes unconditional Android logging from the
per-rectangle GPU-copy hot path. The detailed coordinate and texture trace is
still available with `TERMUX_X11_DEBUG=1`, but normal desktop rendering no
longer performs a log write for every copied damage rectangle.

The sysvk environment override is a fallback, not the default discovery
mechanism. A packager must discover an existing Vulkan HAL from Android's
standard `vendor`, `odm`, or `system` hardware-module directories and pass only
the validated absolute path.
