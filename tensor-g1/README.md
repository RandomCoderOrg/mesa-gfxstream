# Tensor G1 / Termux Panfrost bring-up

This directory contains an experimental userspace Panfrost port for the
Mali-G78 MP20 in Google Tensor G1. It submits jobs directly to Android's Kbase
`/dev/mali0` node; it does not replace or modify the Android kernel driver.

This document covers the open-source OpenGL path. The rootless Android
MediaCodec bridge is documented in
[`media-codec/README.md`](media-codec/README.md), and the separate proprietary
Vulkan integration is documented in
[`vulkan-wrapper/README.md`](vulkan-wrapper/README.md).

> [!WARNING]
> This is a device bring-up branch made from deliberately dirty, invasive
> patches. It is not an upstream-quality driver, it is not conformant, and it
> is not safe to install as the system Mesa. Keep it isolated under
> `/opt/panfork-tensor`. Basic GL works; complex games still expose rendering
> bugs.

Tested on a Pixel 6a from a uDroid Ubuntu 22.04 (Jammy) proot. Rendering is
performed by Panfrost. The current GLX window path allocates linear display
targets from Android's DMA heap, imports them into Kbase, and presents the same
DMA-BUF to Termux:X11 through a three-slot DRI3 Present queue. Released buffers
are rotated back into Mesa instead of being allocated and imported again. EGL
keeps the older CPU presenter until its loader gains the same private callback.
VirGL and ANGLE are not used.

## Proof of hardware rendering

`glxinfo -B` reports the real Tensor GPU and Panfrost renderer:

![glxinfo reporting Mali-G78 Panfrost](screenshots/glxinfo-renderer.png)

Stock Jammy `glxgears` renders through the same GLX/X11 bridge:

| Windowed | Fullscreen |
| --- | --- |
| ![Windowed glxgears](screenshots/glxgears-windowed.png) | ![Fullscreen glxgears](screenshots/glxgears-fullscreen.png) |

The Android status/navigation bars and Termux:X11 extra-keys bar are visible in
the captures because these are unedited `adb screencap` images from the Pixel
6a.

## How this works

```mermaid
flowchart LR
    APP["Linux GLX application"] --> GLX["Mesa GLX and patched drisw"]
    HEAP["Android system DMA heap"] -->|"allocate linear display target"| BUF["DMA-BUF"]
    GLX --> PAN["Panfork / Panfrost"]
    PAN -->|"Kbase UMM import and GPU render"| BUF
    BUF -->|"export fd at swap"| QUEUE["Three-slot DRI3 Present queue"]
    QUEUE -->|"PixmapFromBuffers + Present"| X11["Termux:X11 Present"]
    X11 -->|"IdleNotify releases slot"| QUEUE
    QUEUE -->|"rotate released buffer"| GLX
    X11 -->|"reliable server-side copy"| SURFACE["Android display surface"]
```

There are two separate paths:

1. **Rendering:** Mesa opens Android's existing Kbase character device
   (`/dev/mali0`) and submits Mali Job Manager atoms directly. Panfrost compiles
   shaders and allocates all GPU buffers. This is hardware rendering, not
   llvmpipe.
2. **GLX presentation:** Native Kbase allocations cannot be exported as DMA-BUFs,
   so display targets are instead allocated from `/dev/dma_heap/system` and
   imported into Kbase. At swap, the patched DRI software-presentation frontend
   exports that imported buffer and hands it to Termux:X11 with DRI3 1.2
   `PixmapFromBuffers` and Present. The reliable mode requests a server-side
   copy, allows up to three pixmaps to remain in flight, and rotates a backing
   resource into the next frame only after Present reports it idle.
   `PAN_MALI_X11_DRI3=0` restores the older CPU readback/upload fallback.

`LIBGL_ALWAYS_SOFTWARE=1` therefore has a misleading name here. It selects the
DRI software *presentation frontend*, which this branch then hijacks to create
a Panfrost screen on `/dev/mali0`. `GL_RENDERER` is the source of truth and must
report `Mali-G78 (Panfrost)`.

### Dirty patch inventory

These changes intentionally cut across Mesa layers instead of adding a clean
Android/Kbase winsys:

- `platform_surfaceless.c` opens the device named by `PAN_MALI_DEV`, records a
  non-DRM Kbase hardware device for EGL bookkeeping, and loads
  `panfrost_dri.so` through this fork's swrast-loader ABI. The ABI supplies
  CPU-addressable surfaces; the selected renderer remains Panfrost.
- `platform_x11.c` uses the same non-DRM Kbase EGL device when its repurposed
  software-presentation frontend is backed by Panfrost. This prevents clients
  such as Firefox from classifying the renderer as
  `EGL_MESA_device_software`.
- `platform_x11.c`, `drisw.c`, `drisw_glx.c`, and the DRI target route the
  swrast loader into a real Panfrost `pipe_screen` when
  `PAN_MALI_X11_SWRAST=1`.
- The swrast loader ABI has a private v7 callback that exports a Gallium display
  target as a DMA-BUF and presents it with DRI3/Present. It also reports the
  assigned Present slot and released-slot mask so Gallium can maintain a small
  reusable resource pool. This deliberately crosses the Gallium/GLX loader
  boundary.
- Panfrost display targets use `/dev/dma_heap/system` when
  `PAN_MALI_X11_DRI3=1`; Kbase imports those allocations through its existing
  UMM path. The ordinary Kbase allocator remains the fallback for other
  resources.
- DMA-heap allocation is activated only when the loader advertises the private
  callback. The EGL X11 loader remains on the CPU presenter instead of creating
  an imported buffer it cannot present.
- The normal DRI image extension gate assumes a DRM fd and disables DMA-BUF
  import on `/dev/mali0`. `PAN_MALI_DMABUF_IMPORT=1` overrides that gate for
  the standalone EGL/DRI3 verification probe.
- Panfrost display targets are forced linear because the CPU presenter cannot
  interpret tiled layouts.
- The Mali-G78 product ID (`0x9202`, `TBOx`) is added locally. AFBC is disabled
  for it because this old fork's G78 AFBC path triggers Kbase
  `DATA_INVALID_FAULT` (`0x58`).
- The Kbase Job Manager shim serialises submissions, fixes its completion
  watermark, and drains outstanding atoms before its 8-bit atom IDs wrap.
- Valhall command-stream fixes fill the varying shader's complete resource,
  FAU, and TLS environment; separate fixed-varying packet bytes from generic
  attribute stride; zero rounded FAU tails; and correct the primary shader
  program bits.
- The shared tiler heap starts at its real base. The fork-only `base + 64`
  bottom pointer corrupted later geometry after fully culled draws.
- A few DRI damage paths gain null-resource guards required by the repurposed
  software frontend.

The result is useful as a research/prototyping driver, but the environment
variable switches, non-DRM EGL bookkeeping device, private loader ABI,
fixed-size Present queue, device-specific quirks, and hard-coded aarch64
install layout all need proper design work before anything could be proposed
upstream.

## Build in Jammy

Install Mesa's normal build dependencies. GLX additionally requires
`libxcb-glx0-dev`. Configure with:

```sh
meson setup build-tensor \
  -Dgallium-drivers=panfrost \
  -Dvulkan-drivers= \
  -Dplatforms=x11 \
  -Dglx=dri \
  -Dshared-glapi=enabled \
  -Degl=enabled \
  -Dllvm=disabled \
  -Dbuildtype=debugoptimized \
  -Dprefix=/opt/panfork-tensor
ninja -C build-tensor -j1 install
```

The single-job build is deliberate on Android devices with limited available
memory.

Build the bounded verification programs:

```sh
cc -O2 -Wall -Wextra \
  -I/opt/panfork-tensor/include tensor-g1/panfork/egl-smoke.c \
  -L/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -Wl,-rpath,/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -lEGL -lGLESv2 -o egl-smoke

cc -O2 -Wall -Wextra \
  -I/opt/panfork-tensor/include tensor-g1/desktop/surfaceless-probe.c \
  -L/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -Wl,-rpath,/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -lEGL -lGLESv2 -o surfaceless-probe

cc -O2 -Wall -Wextra \
  -I/opt/panfork-tensor/include tensor-g1/panfork/egl-x11-smoke.c \
  -L/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -Wl,-rpath,/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -lEGL -lGLESv2 -lX11 -o egl-x11-smoke

cc -O2 -Wall -Wextra \
  -I/opt/panfork-tensor/include tensor-g1/panfork/glx-x11-smoke.c \
  -L/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -Wl,-rpath,/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -lGL -lX11 -o glx-x11-smoke

cc -O2 -Wall -Wextra \
  -I/opt/panfork-tensor/include tensor-g1/panfork/glx-x11-bc3.c \
  -L/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -Wl,-rpath,/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -lGL -lX11 -o glx-x11-bc3

cc -O2 -Wall -Wextra \
  -I/opt/panfork-tensor/include tensor-g1/panfork/egl-x11-triangle.c \
  -L/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -Wl,-rpath,/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -lEGL -lGLESv2 -lX11 -o egl-x11-triangle

cc -O2 -Wall -Wextra \
  -I/opt/panfork-tensor/include tensor-g1/panfork/glx-x11-triangle.c \
  -L/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -Wl,-rpath,/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -lGL -lX11 -o glx-x11-triangle

cc -O2 -Wall -Wextra \
  -I/opt/panfork-tensor/include tensor-g1/panfork/egl-dmabuf-dri3.c \
  -L/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -Wl,-rpath,/opt/panfork-tensor/lib/aarch64-linux-gnu \
  -lEGL -lGLESv2 $(pkg-config --cflags --libs xcb xcb-dri3 xcb-present) \
  -o egl-dmabuf-dri3
```

## Run

The surfaceless test does not require a display:

```sh
tensor-g1/panfork/run-panfrost ./egl-smoke
tensor-g1/panfork/run-panfrost ./surfaceless-probe
```

Start Termux:X11 on display `:0` before running windowed applications, then
use the X11 wrapper:

```sh
tensor-g1/panfork/run-panfrost-x11 ./egl-x11-smoke
tensor-g1/panfork/run-panfrost-x11 ./glx-x11-smoke
tensor-g1/panfork/run-panfrost-x11 ./egl-x11-triangle
tensor-g1/panfork/run-panfrost-x11 ./glx-x11-triangle
PAN_MESA_DEBUG=sync tensor-g1/panfork/run-panfrost-x11 ./glx-x11-bc3
tensor-g1/panfork/run-panfrost-x11 your-application
```

The standalone transport probe can be run with the same environment:

```sh
tensor-g1/panfork/run-panfrost-x11 ./egl-dmabuf-dri3
```

It renders into a DMA-heap buffer through Panfork, imports that buffer into
Termux:X11 with DRI3, and prints `PASS`. `--readback` intentionally exercises a
known-unsafe imported-buffer CPU cache-sync path and is not part of the normal
test.

The BC3 probe uploads one known opaque-red DXT5 block and reads its centre
pixel back. The expected result is `pixel: 255 0 0 255`.

For the current desktop-GL smoke targets:

```sh
apt-get install glmark2 mesa-utils
tensor-g1/panfork/run-panfrost-x11 glxgears
tensor-g1/panfork/run-panfrost-x11 glmark2 -s 300x300 \
  -b build:duration=5.0:use-vbo=true
tensor-g1/panfork/run-panfrost-x11 glmark2 --validate
```

The X11 wrapper sets `LIBGL_ALWAYS_SOFTWARE=1` only to select Mesa's software
presentation frontend. The patched frontend opens `/dev/mali0`, creates a
Panfrost renderer, and uses the private DMA-BUF callback for presentation;
`GL_RENDERER` must still report `Mali-G78 (Panfrost)`.

For the currently safer diagnostic mode, serialise GPU work:

```sh
PAN_MESA_DEBUG=sync tensor-g1/panfork/run-panfrost-x11 glxgears
```

This costs performance. An unsynchronised windowed glxgears run rendered
correctly and reached roughly 169--181 FPS, but also printed intermittent Kbase
atom event `0x58`. The synchronised fullscreen capture ran roughly 69--85 FPS
without that event. The asynchronous path is therefore not yet considered
stable.

The important environment variables are:

```sh
DISPLAY=:0
PAN_MALI_DEV=/dev/mali0
PAN_MALI_X11_SWRAST=1
PAN_MALI_X11_DRI3=1
PAN_MALI_DMABUF_IMPORT=1
MESA_LOADER_DRIVER_OVERRIDE=panfrost
LIBGL_ALWAYS_SOFTWARE=1
LD_LIBRARY_PATH=/opt/panfork-tensor/lib/aarch64-linux-gnu
LIBGL_DRIVERS_PATH=/opt/panfork-tensor/lib/aarch64-linux-gnu/dri
```

## GNOME desktop and native ARM64 Firefox

The `desktop/` wrappers run GNOME Shell and Mozilla's native aarch64 Firefox
through the same isolated Panfrost install:

```sh
install -Dm644 tensor-g1/desktop/firefox-user.js \
  /root/.mozilla/firefox/panfrost-profile/user.js
mkdir -p /opt/firefox-glxtest-stub
cc -shared -fPIC -Wl,-soname,libpci.so.3 \
  tensor-g1/desktop/glxtest-no-pci.c \
  -o /opt/firefox-glxtest-stub/libpci.so.3
ln -sf libpci.so.3 /opt/firefox-glxtest-stub/libpci.so
tensor-g1/desktop/start-gnome-x11
tensor-g1/desktop/run-firefox-panfrost about:support
tensor-g1/desktop/run-epiphany-panfrost --private-instance \
  file:///tmp/video-loop.html
```

They deliberately default `PAN_MALI_X11_DRI3=0` and
`PAN_MALI_DMABUF_IMPORT=0`. GNOME Shell uses EGL, whose X11 loader does not yet
implement the private reusable DMA-BUF presentation callback used by the GLX
path. Final desktop windows are therefore CPU-presented while rendering stays
on Mali-G78. The GNOME wrapper also removes an empty `/run/systemd/seats`
marker when no systemd process exists. Without that PRoot guard, GNOME Shell 42
mistakes the directory installed by the systemd package for a working logind
service and aborts during background initialization.

Android exposes `/sys/bus/pci` but not `/proc/bus/pci/devices` inside PRoot.
Firefox's short-lived graphics test consequently exits inside libpci before it
can test the GPU. Build `glxtest-no-pci.c` as `libpci.so.3` and install it only
under `/opt/firefox-glxtest-stub`; the Firefox wrapper prepends that directory
to its own `LD_LIBRARY_PATH`. The deliberately empty shim makes Firefox treat
PCI discovery as unavailable and continue to the real EGL test. Do not install
it system-wide.

With that shim, Firefox's probe exits successfully with `TEST_TYPE EGL`,
`DRI_DRIVER panfrost`, and `Mali-G78 (Panfrost)`. `about:support` reports
WebRender, `mesa/panfrost`, and Panfrost for both WebGL versions. The wrapper
retains Firefox-only OpenGL 3.2/GLSL 1.50 overrides for compositor paths that
request a 3.2 core context. Do not export them globally.

Firefox now reaches the experimental Tensor VA-API driver without a custom
browser build. Mesa reports accessible Kbase `/dev/mali0` as the compatibility
device carrier, Firefox's stock probe reports H.264 hardware support, and its
RDD process decoded a local H.264 frame through Android MediaCodec. Presentation
of that decoded DMA-heap NV12 surface was blank in the tested window and the
RDD sandbox currently must be disabled per launch. The implementation and
remaining browser-integration boundary are documented in
[`media-codec/README.md`](media-codec/README.md).

GNOME Web can use that bridge. WebKitGTK's DMA-BUF renderer must be disabled
because it assumes a DRM/GBM device, while this port exposes Kbase
`/dev/mali0`. `run-epiphany-panfrost` scopes that workaround and WebKit's
PRoot sandbox exception to GNOME Web. WebKit accelerated composition remains
enabled: the tested browser visibly painted the 1080p60 test pattern through
Panfrost while GStreamer selected `tensorh264dec` and Android selected
`c2.exynos.h264.decoder`.

## Verified status

- Surfaceless EGL/GLES renders and reads back the expected pixel.
- EGL/GLES creates a visible Termux:X11 window and reports OpenGL ES 3.1.
- GLX creates a desktop OpenGL context and reports OpenGL 3.0.
- GLX reports `Mali-G78 (Panfrost)` while rendering directly into a DMA-heap
  buffer imported by Kbase. The same DMA-BUF is accepted by Termux:X11 through
  DRI3 1.2 and visibly presented by stock Jammy `glxgears`.
- This DRI3 route removes Panfork's client-side framebuffer map/readback and
  XImage/MIT-SHM upload. Termux:X11 currently performs the final Present copy;
  its log reported that these copies were not GPU-offloaded on the tested
  build. GLX now pipelines three Present pixmaps and recycles their DMA-heap
  resources only after `IdleNotify`, avoiding per-frame DMA-heap allocation and
  Kbase import.
- A GLES2 triangle with a vertex-to-fragment color varying completes and reads
  back the expected pixel.
- A GLX fixed-function triangle with depth, lighting, normals, and indexed
  drawing completes and reads back the expected pixel.
- Stock Jammy `glxgears` renders visibly without the intermittent white gear
  sectors. Pre-swap GPU readback found zero near-white pixels in 40 consecutive
  instrumented frames, including the formerly failing fully culled draw path.
  A 20-second 640x480 run with the pipelined presenter measured about 236--274
  FPS on the test Pixel 6a.
- `glmark2` reports OpenGL 3.0 and `Mali-G78 (Panfrost)`. Its VBO build scene
  measured 168 FPS at 300x300 on the older presenter. The DMA-BUF/DRI3 path
  completed the same five-second scene at 640x480 with clean-exit results of
  225--257 FPS using the original wait-per-frame presenter. The pooled
  three-slot queue reached 269 FPS in a standalone run; a build, texture,
  Gouraud-shading, and high-poly-bump transition group completed at 293, 277,
  273, and 228 FPS respectively, scoring 267. Every scene for which glmark2
  2021.02 ships a validation reference passed; the advanced `ideas`,
  `jellyfish`, `terrain`, `shadow`, and `refract` scenes also completed without
  a GPU fault.
- A complete default fullscreen glmark2 run at the Termux:X11 display size
  (1080x2008) completed all 33 scenes with a score of 59. The 300x300 advanced
  smoke group scored 105.
- SuperTuxKart 1.3 renders the tutorial track correctly with its normal HD and
  texture-compression options enabled. Tensor G1's raw GPU properties claim
  BCn support, but a known-good BC3 block samples as opaque black when passed
  to the G78 texture unit. The driver therefore rejects native S3TC for this
  GPU ID; Mesa's state tracker decompresses BC textures once during upload and
  Panfrost samples the resulting RGBA textures in hardware. The dedicated BC3
  probe reads back the expected opaque red pixel after this fallback.
- Khronos VK-GL-CTS 3.2.8.0 targeted smoke groups passed 95/95 cases: 44
  GLES3, 41 GLES2, and 10 EGL. This is deliberately small coverage, not a
  conformance claim.
- Surfaceless EGL now uses the explicit Kbase device even though the wrapper
  sets `LIBGL_ALWAYS_SOFTWARE=1`; `surfaceless-probe` reports EGL 1.4 and
  `Mali-G78 (Panfrost)` through the swrast-loader ABI.
- Firefox's EGL probe completes and `about:support` reports WebRender and
  `mesa/panfrost` rather than a software device.
- Android MediaCodec selected `c2.exynos.h264.decoder`; the native decoder,
  Unix-socket bridge, and Jammy GStreamer `tensorh264dec` element each decoded
  all 120 frames of the 1920x1080 60 FPS smoke stream. The GStreamer element
  delivered all 120 tightly packed NV12 frames to `fakesink`.
- G78 AFBC is disabled in this branch. The older AFBC path produced Kbase
  `DATA_INVALID_FAULT` (`0x58`) even for a simple clear.
- Valhall varying shaders now receive the complete shader environment
  (resources, FAU/uniforms, and thread storage). Previously the v9 malloc-IDVS
  descriptor set only the secondary shader pointer, causing every draw with a
  vertex-to-fragment varying to fail with Kbase `DATA_INVALID_FAULT` (`0x58`).
- The Valhall malloc-IDVS allocation now separates fixed varyings in the
  vertex packet from generic varying attribute stride. This avoids fixed-color
  data being interpreted with a generic attribute stride.
- The shared tiler heap starts at its actual base, matching upstream Mesa.
  This reverts the fork-only `base + 64` bottom pointer that corrupted earlier
  lit geometry when a later IDVS draw was completely removed by face culling.

## Known failures and limitations

- S3TC/BCn is not sampled natively on this Tensor G1 path. Mesa preserves the
  GL extensions through its software upload fallback, but decompressed RGBA
  textures use more memory and bandwidth than BCn. This fixes correctness for
  SuperTuxKart without claiming native compressed-texture acceleration.
- The asynchronous Job Manager path can still report atom event `0x58` during
  longer GLX workloads. Use `PAN_MESA_DEBUG=sync` when correctness matters.
- Reliable presentation still requests a full-frame copy inside Termux:X11.
  The DRI3 path eliminates Panfork's client-side readback/upload, but the
  tested X server did not GPU-offload its final copy. Strict no-copy Present
  (`PAN_MALI_X11_DRI3_ZERO_COPY=1`) negotiates successfully but produces an
  invisible window under the tested GNOME/Mutter session.
- G78 AFBC is disabled; swap control is deferred; resizing, suspend/resume,
  multisampling, audio integration, input devices, long stability runs, and a
  complete desktop session remain incomplete.
- `MESA-LOADER: failed to retrieve device information` is expected because
  `/dev/mali0` is Kbase, not a DRM render node.
- The MediaCodec bridge is H.264-only and CPU-copies decoded NV12 frames over a
  Unix socket into exportable DMA-heap VA surfaces. FFmpeg readback is verified
  pixel-exact, but stock Firefox presentation remains blank; it is not yet a
  working or zero-copy browser path.
- GNOME Web 42.4 segfaults in GTK style-provider setup if WebKit's DMA-BUF
  renderer is enabled. Use `desktop/run-epiphany-panfrost`, which disables
  only that renderer; Panfrost composition and MediaCodec video decoding stay
active. Epiphany session restore may reopen several Web processes and create
  avoidable memory pressure on the 6 GB test device; use `--private-instance`
  for isolated video smokes.

This branch demonstrates that a Termux/proot process can render on Tensor G1's
Mali-G78 with an open Panfrost userspace stack. It does not yet demonstrate a
general-purpose gaming driver.
