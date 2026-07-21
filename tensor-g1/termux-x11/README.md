# Termux:X11 Tensor G1 DRI3 patch

This directory preserves the companion X-server changes used by the Tensor G1
Panfrost experiment. They apply to Termux:X11 commit
`caa7553b36c5567e19896ab6521a2cdcb8010014` and are intentionally kept as a
standalone patch because Termux:X11 is not vendored into this Mesa repository.

Apply and build it from a clean Termux:X11 checkout:

```sh
git checkout caa7553b36c5567e19896ab6521a2cdcb8010014
git apply /path/to/tensor-g1/termux-x11/tensor-g1-dri3.patch
JAVA_HOME='/Applications/Android Studio.app/Contents/jbr/Contents/Home' \
  ./gradlew :app:assembleDebug --no-daemon \
  -Pandroid.injected.build.abi=arm64-v8a
adb install -r -t app/build/outputs/apk/debug/app-arm64-v8a-debug.apk
```

The patch is deliberately platform-specific:

- FD-backed X pixmaps use Android's system DMA heap when available and fall
  back to the original shared-memory region otherwise. Their allocated row
  stride is padded to 64 bytes before they can become Panfrost v7+ sampled
  textures; regular-to-FD conversion copies between the real source and
  padded destination strides.
- DRI3 opens `/dev/mali0` for clients and can export an X pixmap as a linear
  DMA-BUF. This gives KWin and other compositors a real Kbase/Panfrost device
  and importable backing storage without `/dev/dri`.
- AHardwareBuffer imports retain their logical X pixmap dimensions even when
  Android allocates a physically padded object.
- DRI3 format and drawable-modifier discovery advertise Termux:X11's private
  `AHARDWAREBUFFER_SOCKET_FD` transport alongside linear DMA-BUF. This lets a
  broker-backed Panfrost image keep its socket modifier through loader
  negotiation instead of being rejected as an ordinary raw fd.
- converting a regular pixmap clears the stale EXA CPU pointer before the new
  backing allocation is locked. This fixed repeated texture-from-pixmap
  updates using the same pixmap.
- CPU access and copies are bracketed with `DMA_BUF_IOCTL_SYNC`.
- an FD buffer receives a final START/END READ|WRITE synchronization before
  `munmap()` and `close()`. This fixed the Kbase fault caused by destroying a
  composited X window while the GPU still referenced its DMA-BUF.
- the xkbcomp include override avoids a host-header collision in the Android
  build used for the test APK.

Enable optional release-fence timing while reproducing lifecycle behavior:

```sh
TERMUX_X11_DEBUG=1 TERMUX_X11_LOG_FD_RELEASE_SYNC=1 termux-x11 :0
```

The clean regression checkpoint completed ten rapid composited-window destroy
cycles and twelve repeated texture-from-pixmap update cycles without a GPU
fault. A later width matrix completed 33/33 allocation/import/Present/destroy
cases from 960 through 1040 pixels without raw fallback or a GPU fault. The
final FD synchronization usually cost about 0.19--0.31 ms, with observed
samples from roughly 0.004 to 0.50 ms. Destruction synchronization and aligned
producer strides address independent failure modes.

The complete AHardwareBuffer path was promoted to full Plasma at 1080x2205.
SurfaceFlinger measured 29.14 FPS for the initial window-motion run versus
18.17 FPS on aligned raw DRI3. The five-run thermal-status-2 soak delivered a
27.11 FPS median with zero dropped frames; evidence and the current graph are
in [`../perf/results/winsys-ahb-20260721/`](../perf/results/winsys-ahb-20260721/).

The reliable compositor route still performs two Android-side stages after
KWin renders: Present GPU-copies the AHB into the X root buffer, then the
renderer samples that root buffer into the Android EGL surface. The renderer
also waits synchronously for its GPU fence before acknowledging the Present
source. This serialization is the leading explanation for the approximately
30--32 FPS cool-state cadence versus the 61.47 FPS uncomposited ceiling.

The base `loriePresentFlip` test contains a separate width/height error:

```c
root.width != pixmap.width || root.width != pixmap.height
```

The second comparison should test `root.height`, otherwise every non-square
full-screen buffer is rejected. This checkpoint deliberately records but does
not change it. Direct flip must first prove explicit source lifetime, correct
Complete/Idle ordering, pixels, resize, and orientation in a bounded AHB probe;
the previously tested strict no-copy route could become invisible or corrupt.

This is a research patch, not an upstream-ready Termux:X11 design. The
hard-coded Kbase node, system heap, format assumptions, and DRI3 behavior need
a negotiated platform interface before general use.
