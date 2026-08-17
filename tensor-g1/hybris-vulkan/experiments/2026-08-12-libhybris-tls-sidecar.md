# libhybris TLS sidecar and accelerated Plasma checkpoint

Date: 2026-08-12

Device: Pixel 6a, Tensor G1, Mali-G78

Status: validated proof; installed on the test device; not promoted to the
default desktop launcher

## Why this checkpoint matters

The older `plasma-vendor-session` launcher runs KWin and ordinary applications
through Zink and the Android vendor Vulkan driver, but deliberately forces
`plasmashell` to llvmpipe. That file is a compatibility fallback, not the
latest technical result. Do not use it as evidence that accelerated Plasma
Shell remains impossible.

The later debugger run found the actual Plasma crash. The Android-Q libhybris
linker resolved a vendor static TLS symbol to `TPIDR_EL0 + 136`, inside the
host glibc thread-control block. A store from `libGLES_mali.so` corrupted
glibc's current-locale pointer; the next `tolower()` call crashed Plasma.

This ruled out KDE, Ginkage, Termux:X11, Zink, and locale hooks as the primary
cause of that crash.

## Preserved proof patch

[`libhybris-0002-isolate-android-static-tls.patch`](../patches/libhybris-0002-isolate-android-static-tls.patch)
changes only the Android static TLSDESC path:

1. Allocate one Android static-TLS block per host thread.
2. Initialize it from libhybris's registered TLS modules.
3. Resolve Android static TLS addresses inside that sidecar.
4. Return `sidecar address - TPIDR_EL0`, preserving the AArch64 TLSDESC ABI.
5. Cache the address in host-native `thread_local` storage and release the
   allocation through a pthread key.

The patch is a proof, not an upstream-ready implementation.

## Validation passed

- Full Vulkan instance, device, queue, wait-idle, and destroy lifecycle.
- Host TLS diff stayed unchanged except Android's expected OpenGL slot.
- Eight host threads completed four Vulkan device lifecycles each with zero
  failures and unchanged glibc ctype pointers.
- `glxinfo` reported `zink Vulkan 1.4 (Mali-G78 (ARM_PROPRIETARY))`.
- KWin and Plasma both ran through Zink, vendor Vulkan, and the selected WSI.
- Plasma used `QT_QUICK_BACKEND=opengl` and `QSG_RENDER_LOOP=threaded`.
- Two independent 100-cycle launcher/hover stress runs survived.
- The cached-sidecar 100-cycle run completed in about 42.3 seconds.

The test-device `q.so` currently exports `__hybris_static_tls_address`, proving
that the installed libhybris build contains this patch.

## Active, fallback, and rejected paths

| Path | State | Meaning |
| --- | --- | --- |
| Ginkage AHardwareBuffer + Kopper + TLS sidecar | Validated manual checkpoint | Current smooth vendor-Vulkan desktop baseline |
| Ginkage DMA-BUF copy + TLS sidecar | Vulkan compatibility path | Not the validated smooth Plasma configuration |
| `plasma-vendor-session` with software Plasma Shell | Compatibility fallback | Stale launcher; useful only if threaded Qt Quick regresses |
| xMeM AHardwareBuffer WSI | Isolated oracle | Keep selectable; do not replace Ginkage |
| Termux:X11 external-texture import | Rejected experiment | Did not pass the artifact or responsiveness gate |
| Panfrost/Panfork | Separate renderer | Not part of this vendor-Vulkan checkpoint |

For a manual accelerated Plasma run, use the versioned
[`start-plasma-vendor-zink-ahb`](../start-plasma-vendor-zink-ahb) launcher. The
important process-local settings are:

```sh
export MESA_LOADER_DRIVER_OVERRIDE=zink
export LIBGL_DRIVERS_PATH=/root/mesa-kopper-build2/src/gallium/targets/dri
export LIBGL_KOPPER_DRI2=1
export VK_INSTANCE_LAYERS=VK_LAYER_window_system_integration
export VK_LAYER_PATH=/root/hybris-rootless/ginkage-ahb-prefix/share/vulkan/implicit_layer.d
export WSI_X11_AHB=1
export WSI_X11_PRIVATE_CONNECTION=1
export QT_QUICK_BACKEND=opengl
export PLASMA_QUICK_BACKEND=opengl
export QSG_RENDER_LOOP=threaded
```

The validated session used Termux:X11 display `:2`, left KWin compositing
enabled, and launched KWin, KDE services, and Plasma in one D-Bus session. Do
not set `KWIN_COMPOSE=N`, unset the Vulkan/Zink variables around
`plasmashell`, or substitute the generic DMA-BUF copy wrapper; each change
selects a different checkpoint.

## Promotion gate

Before replacing the compatibility launcher:

- encode the validated process tree in a versioned launcher;
- add an explicit TLS generation contract for modules loaded after sidecar
  allocation;
- route dynamic TLSDESC through the same isolated storage contract;
- replace duplicated register-save assembly with a shared trampoline;
- rerun independent per-thread initialization/destructor probes;
- rerun the launcher/hover stress and measure panel response latency;
- retain an explicit software fallback instead of silently selecting it.

Until those checks pass, describe this as a validated accelerated checkpoint,
not a stable default.
