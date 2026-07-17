# Tensor G1 PRoot GPU

Experimental GPU acceleration for Linux applications running through Termux,
Termux:X11, and uDroid/PRoot on Google Tensor G1 devices with Mali-G78 graphics.

This repository carries two deliberately separate acceleration paths:

| Path | API and userspace | Rendering and presentation |
| --- | --- | --- |
| Panfork/Panfrost | OpenGL and OpenGL ES in glibc PRoot distributions | Patched Mesa submits directly to Android Kbase `/dev/mali0`; completed linear frames are copied to Termux:X11. |
| Mali Vulkan wrapper | Vulkan for native Termux/Bionic programs | Mesa wrapper adds X11/XCB WSI around Android's proprietary Mali Vulkan driver. |

Neither path replaces the Android kernel driver. The Vulkan wrapper is not
PanVK, and the OpenGL path does not use ANGLE, Zink, VirGL, or Vulkan.

## Repository layout

```text
src/                              patched Panfork/Mesa source
tensor-g1/README.md               Panfork bring-up and verified status
tensor-g1/panfork/                launchers, probes, and test utilities
tensor-g1/vulkan-wrapper/         pinned wrapper recipe and patch series
tensor-g1/screenshots/            unedited device captures
README.rst                        original Mesa/Panfork project README
```

## Current verified status

The open-source Panfork path reports `Mali-G78 (Panfrost)` and runs EGL, GLX,
glxgears, glmark2, GNOME, and SuperTuxKart with known limitations documented in
[`tensor-g1/README.md`](tensor-g1/README.md).

The proprietary Vulkan path reports the real Mali-G78 r54p3 driver with Vulkan
1.4.305. XCB presentation is verified with `vkcube` both directly in Termux and
when the same Bionic binary is launched from a Jammy PRoot namespace. See
[`tensor-g1/vulkan-wrapper/README.md`](tensor-g1/vulkan-wrapper/README.md).

## Warning

This is research and prototyping code, not an official Google, Arm, Mesa, or
Termux driver. The Panfork changes are intentionally invasive and are not
upstream quality. Keep experimental installs isolated, preserve ADB access,
and expect rendering bugs, GPU faults, CPU-copy presentation overhead, and ABI
limitations.
