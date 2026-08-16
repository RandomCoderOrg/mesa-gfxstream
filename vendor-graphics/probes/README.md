# Graphics probes

These programs isolate one graphics boundary at a time. They are deliberately
smaller than a desktop environment or browser so failures can be attributed to
discovery, allocation, import, synchronization, presentation, or API support.

## Vulkan XCB Present

`vulkan-xcb-present.c` creates a Vulkan XCB swapchain, clears one image red,
presents it, and keeps the window alive briefly. It verifies the complete
vendor Vulkan to X11 WSI path without involving Mesa, Zink, or a compositor.
The default six-second hold supports visual inspection. Use `--hold-ms 0` for
repeatable lifecycle soaks that exercise creation, presentation, and teardown
without paying the visual hold cost on every cycle.

Build it in an AArch64 glibc guest with Vulkan and XCB development headers:

```sh
cc -O2 -Wall -Wextra vulkan-xcb-present.c -o vulkan-xcb-present \
  -lvulkan -lxcb

./vulkan-xcb-present --hold-ms 0
```

A passing run must satisfy all of these conditions:

- the log reaches `PASS stage=present` and `PASS stage=clean-exit`;
- an image view using the advertised swapchain format reaches
  `PASS stage=image-view`;
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

## Unix-socket handshake faults

`unix-socket-handshake.c` validates the bounded control-plane behavior used by
the paired xMeM producer and Termux:X11 importer. It covers a silent peer, a
closed peer, a delayed valid acknowledgement, and an invalid acknowledgement
without allocating an AHardwareBuffer or launching X11.

```sh
cc -O2 -Wall -Wextra unix-socket-handshake.c -o unix-socket-handshake -pthread
./unix-socket-handshake
```

`x11-ahb-handshake-fault.c` exercises the actual Termux:X11 DRI3 import path.
It withholds the AHardwareBuffer after the acknowledgement and requires the
server to reject that request near the three-second deadline, then answer a
normal X request in under one second.

```sh
cc -O2 -Wall -Wextra x11-ahb-handshake-fault.c \
  -o x11-ahb-handshake-fault -lxcb -lxcb-dri3
DISPLAY=:0 ./x11-ahb-handshake-fault
```
