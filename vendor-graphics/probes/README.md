# Graphics probes

These programs isolate one graphics boundary at a time. They are deliberately
smaller than a desktop environment or browser so failures can be attributed to
discovery, allocation, import, synchronization, presentation, or API support.

## Vulkan XCB Present

`vulkan-xcb-present.c` creates a Vulkan XCB swapchain, clears one image red,
presents it, and keeps the window alive briefly. It verifies the complete
vendor Vulkan to X11 WSI path without involving Mesa, Zink, or a compositor.

Build it in an AArch64 glibc guest with Vulkan and XCB development headers:

```sh
cc -O2 -Wall -Wextra vulkan-xcb-present.c -o vulkan-xcb-present \
  -lvulkan -lxcb
```

A passing run must satisfy all of these conditions:

- the log reaches `PASS stage=present` and `PASS stage=complete`;
- the window is red rather than blue, black, or intermittently corrupted;
- Termux:X11 reports the Present copy as GPU-offloaded;
- the process exits normally without terminating the X server.

The visual and server-log gates are required. Vulkan returning success alone
does not prove correct channel semantics or zero-copy/offloaded presentation.
