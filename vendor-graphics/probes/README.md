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

## AHardwareBuffer transport

`ahardwarebuffer-transport.c` measures allocation, CPU mapping, explicit unlock
fence completion, Unix-socket handle transfer, receiving, and content integrity
across aligned and deliberately odd widths. Each case emits one JSON line with
stage timings and hashes so regressions can be graphed without launching a
desktop application.

The probe must be linked through the same libhybris AHardwareBuffer provider as
the runtime. Provider discovery is a separate gate: a library existing on disk
does not prove that its Bionic ABI and TLS behavior are safe in the guest.

Run ten iterations across all default widths with:

```sh
./ahardwarebuffer-transport 10 32
```

Use `UDROID_AHB_TRANSPORT_ONLY=1` to isolate allocation and socket transport,
or `UDROID_AHB_SOURCE_ONLY=1` to stop after the producer sends its handle.
