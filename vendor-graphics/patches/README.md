# Patch series

These are the minimal source changes used by the first passing Exynos vendor
Vulkan and AHardwareBuffer WSI proof. They are kept as reviewable patches so a
release build can fetch pinned upstream sources and fail if a patch no longer
applies cleanly.

| Component | Upstream | Pinned revision | Patch |
| --- | --- | --- | --- |
| libhybris | `https://github.com/libhybris/libhybris.git` | `7079712a42ea2754adf747e70c6cc75764c8596e` | `libhybris/0001-complete-locale-and-ui-compatibility.patch` |
| sysvk | `https://github.com/xMeM/sysvk.git` | `23ecd775ed6fe06bb5ac0063b5f981f70c543c67` | `sysvk/0001-discover-and-validate-explicit-vulkan-hal.patch` |
| xMeM WSI | `https://github.com/xMeM/vulkan-wsi-layer.git` | `d5624d42d8b2debbd910ad25662a05c751eb38b7` | `xmem-wsi/0001-port-ahb-x11-wsi-to-glibc-and-rgba-semantics.patch`, `xmem-wsi/0002-pass-sync-file-fences-to-x-present.patch` |
| Termux:X11 | uDroid's pinned Termux:X11 submodule | recorded by the consuming uDroid revision | `termux-x11/0001-import-rgba-ahardwarebuffer-content-without-swizzle.patch`, `termux-x11/0002-import-linux-sync-file-fences.patch` |

The xMeM WSI and Termux:X11 patches form one private protocol revision: DRI3
modifier `1257` means an AHardwareBuffer transport whose content has RGBA
semantics. Do not deploy one side without the other. This private modifier is
not claimed to be a Linux DRM modifier and must remain inside the paired uDroid
transport boundary.

The second Termux:X11 patch recognizes Linux `sync_file` FDs in the existing
DRI3 fence import backend while preserving ordinary xshmfence behavior. It is
the server half of the optional explicit-acquire synchronization path; the CPU
Vulkan-fence wait remains the safe client fallback.

The second xMeM patch is the paired producer half. With
`UDROID_X11_EXPLICIT_SYNC=1`, it exports each submitted Vulkan fence as a
`sync_file`, imports it through DRI3, passes it to Present, and destroys the X
fence after completion. Without that opt-in it retains the previously validated
CPU Vulkan-fence wait.

The sysvk environment override is a fallback, not the default discovery
mechanism. A packager must discover an existing Vulkan HAL from Android's
standard `vendor`, `odm`, or `system` hardware-module directories and pass only
the validated absolute path.
