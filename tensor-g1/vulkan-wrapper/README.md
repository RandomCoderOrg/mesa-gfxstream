# Tensor G1 Termux Vulkan wrapper

This component exposes Google Tensor G1's proprietary Android Mali-G78 Vulkan
driver to native Termux Vulkan programs with X11/XCB window-system integration.
It does not implement PanVK, and it does not route OpenGL through ANGLE or Zink.

The working stack is:

```text
Termux/Bionic Vulkan application
  -> generic Vulkan loader
  -> libvulkan_wrapper.so
  -> Mesa X11/XCB WSI
  -> Android libvulkan.so
  -> vendor vulkan.mali.so
  -> Mali-G78
```

## Source and patches

The build script fetches xMeM's Mesa wrapper branch at the pinned commit
`e65c7eb6ee2f9903c3256f2677beb1d98464103f`. The `patches/` directory keeps the
complete local patch series. The first four changes come from xMeM's Termux
packaging work; the final patch preserves Mesa's Android `memfd_create` syscall
fallback after the wrapper deliberately disables `DETECT_OS_ANDROID` to reach
the Linux X11 WSI code.

Generated sources, build objects, and the installed prefix are intentionally
not committed. A clean checkout plus the pinned commit and these patches is the
reproducible source of truth.

## Build in Termux

This changes Termux from `vulkan-loader-android` to
`vulkan-loader-generic`. The two loader packages conflict. The generic loader
is required because the wrapper is a Vulkan ICD that supplies X11/XCB WSI.

```sh
cd tensor-g1/vulkan-wrapper
./setup-termux.sh
./build-termux.sh
```

The build detects the device's real Android API level with `getprop` and uses
an API-qualified Clang target such as `aarch64-linux-android37`. This prevents
native Meson checks from detecting newer Bionic symbols while compiling with
Termux's default API-24 availability rules.

Defaults can be overridden without editing the scripts:

```sh
VULKAN_WRAPPER_WORKDIR=$HOME/vulkan-work \
VULKAN_WRAPPER_PREFIX=$HOME/vulkan-wrapper-prefix \
JOBS=6 ./build-termux.sh
```

## Verify

Start Termux:X11 on display `:0`, then run:

```sh
tensor-g1/vulkan-wrapper/run-vulkan-x11 vulkaninfo --summary
tensor-g1/vulkan-wrapper/run-vulkan-x11 \
  vkcube --c 300 --wsi xcb --width 640 --height 480
```

The tested Pixel 6a reported:

```text
deviceName         = Mali-G78
apiVersion         = 1.4.305
driverVersion      = 54.3.0
driverID           = DRIVER_ID_ARM_PROPRIETARY
driverInfo         = v1.r54p3-01eac0.8acd90adf601ed5e7c6df1066c9b17c1
```

Both a 300-frame native Termux `vkcube` run and a 180-frame run launched from
the uDroid Jammy namespace completed with exit status zero.

## PRoot ABI boundary

The wrapper, generic loader, Vulkan tools, and Android vendor driver are Bionic
binaries. They remain reachable by absolute path from a uDroid/PRoot namespace,
which is why the Termux `vulkaninfo` and `vkcube` binaries work when launched
from `jammy:raw`.

That does not make the wrapper a glibc ICD. An ordinary Jammy ARM64 Vulkan
binary cannot directly load these Bionic libraries. A Termux-native Box64
build can potentially wrap x86-64 Vulkan calls into this native stack; native
glibc Vulkan applications need an additional ABI bridge.

## Status

This is experimental integration code around a proprietary Android driver.
It is separate from the repository's open-source Panfork/Panfrost OpenGL path.
The wrapper's XCB swapchain path is verified, but broad Vulkan CTS, DXVK, Wine,
and Proton coverage has not yet been established.
