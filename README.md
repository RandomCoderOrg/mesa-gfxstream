# Tensor G1 PRoot GPU

Experimental GPU acceleration for Linux applications running through Termux,
Termux:X11, and uDroid/PRoot on Google Tensor G1 devices with Mali-G78 graphics.

This repository carries three deliberately separate acceleration paths:

| Path | API and userspace | Rendering and presentation |
| --- | --- | --- |
| Panfork/Panfrost | OpenGL and OpenGL ES in glibc PRoot distributions | Patched Mesa submits directly to Android Kbase `/dev/mali0`; GLX DMA-heap display targets use DRI3, while EGL retains the CPU presenter. |
| Mali Vulkan wrapper | Vulkan for native Termux/Bionic programs | Mesa wrapper adds X11/XCB WSI around Android's proprietary Mali Vulkan driver. |
| libhybris Vulkan | Vulkan for glibc PRoot programs | sysvk and libhybris call the Android Mali Vulkan HAL; a patched WSI layer presents DMA-BUFs through Termux:X11 DRI3. |

None of these paths replaces the Android kernel driver. The Vulkan routes are
not PanVK, and the OpenGL path does not use ANGLE, Zink, VirGL, or Vulkan.

## Repository layout

```text
src/                              patched Panfork/Mesa source
tensor-g1/README.md               Panfork bring-up and verified status
tensor-g1/panfork/                launchers, probes, and test utilities
tensor-g1/vulkan-wrapper/         pinned wrapper recipe and patch series
tensor-g1/hybris-vulkan/          rootless glibc Vulkan launcher and DRI3 fix
tensor-g1/screenshots/            unedited device captures
README.rst                        original Mesa/Panfork project README
```

## Current verified status

The open-source Panfork path reports `Mali-G78 (Panfrost)` and runs EGL, GLX,
glxgears, glmark2, GNOME, and SuperTuxKart. Its rootless DRI3 path removes the
old per-frame client readback/upload; a three-slot Present queue recycles
DMA-heap buffers after the X server releases them. Termux:X11 still performs
the final server-side copy. Known limitations are documented in
[`tensor-g1/README.md`](tensor-g1/README.md).

The proprietary Vulkan path reports the real Mali-G78 r54p3 driver with Vulkan
1.4.305. XCB presentation is verified with `vkcube` both directly in Termux and
when the same Bionic binary is launched from a Jammy PRoot namespace. See
[`tensor-g1/vulkan-wrapper/README.md`](tensor-g1/vulkan-wrapper/README.md).

The rootless glibc Vulkan milestone adds a third route through sysvk, patched
libhybris, and an X11 WSI layer. DMA-BUF allocation and DRI3 presentation are
verified from Jammy without root, including a 1,800-frame soak. See
[`tensor-g1/hybris-vulkan/README.md`](tensor-g1/hybris-vulkan/README.md).

## Warning

This is research and prototyping code, not an official Google, Arm, Mesa, or
Termux driver. The Panfork changes are intentionally invasive and are not
upstream quality. Keep experimental installs isolated, preserve ADB access,
and expect rendering bugs, GPU faults, presentation fallbacks, and ABI
limitations.
