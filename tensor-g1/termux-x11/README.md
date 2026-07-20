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
  back to the original shared-memory region otherwise.
- DRI3 opens `/dev/mali0` for clients and can export an X pixmap as a linear
  DMA-BUF. This gives KWin and other compositors a real Kbase/Panfrost device
  and importable backing storage without `/dev/dri`.
- AHardwareBuffer imports retain their logical X pixmap dimensions even when
  Android allocates a physically padded object.
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
fault. The final FD synchronization usually cost about 0.19--0.31 ms, with
observed samples from roughly 0.004 to 0.50 ms. This synchronization fixes the
destroy path; it does not fix Panfrost's separate requirement that imported
regular textures use a 64-byte-aligned row stride.

This is a research patch, not an upstream-ready Termux:X11 design. The
hard-coded Kbase node, system heap, format assumptions, and DRI3 behavior need
a negotiated platform interface before general use.
