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

## X11 fence semantics

`x11-sync-fence-semantics.c` distinguishes the standard DRI3 shared-memory
fence from the Android/Linux `sync_file` exported by vendor Vulkan. The first
case is a control: a triggered xshmfence-compatible memfd must import through
DRI3. The second exports a real `VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT`
fence and attempts the same import. This probe establishes whether the X
server can consume an acquire fence without a CPU wait; it does not infer that
capability merely because DRI3 1.2 is advertised.

```sh
cc -O2 -Wall -Wextra x11-sync-fence-semantics.c \
  -o x11-sync-fence-semantics -lvulkan -lxcb -lxcb-dri3 -lxcb-sync
DISPLAY=:0 ./x11-sync-fence-semantics
```

Use `--xshm-only` or `--sync-file-only` to isolate a blocking server or driver
path. The probe has an eight-second internal deadline and exits 124 rather than
leaving a wedged diagnostic process behind.

An unmodified Xorg DRI3 fence backend normally accepts the xshmfence control
and rejects the Vulkan sync file because their FD semantics differ. A direct
explicit-sync path is available only when both imports succeed and a later
delayed-fence Present test proves that the server actually waits.

`x11-present-sync-file-wait.c` performs that ordering test. It submits a tiny
pixmap behind a vendor Vulkan fence whose command buffer is blocked on a Vulkan
event. No Present Complete event may arrive during the first 250 ms. The probe
then releases the Vulkan event and requires completion within three seconds.

```sh
cc -O2 -Wall -Wextra x11-present-sync-file-wait.c \
  -o x11-present-sync-file-wait \
  -lvulkan -lxcb -lxcb-dri3 -lxcb-present -lxcb-sync
DISPLAY=:0 ./x11-present-sync-file-wait
```

Both `completed_early=false` and `completed_after_release=true` are mandatory.
This catches a server that accepts a sync-file FD but triggers or ignores it
before the producer has completed rendering.

## Termux:X11 mutex clock contention

`lorie-mutex-clock.c` reproduces the process-shared renderer lock's timed wait
without starting a desktop. It holds one recursive mutex for 50 ms, then
compares the current `CLOCK_MONOTONIC` deadline with the `CLOCK_REALTIME`
deadline required by `pthread_mutex_timedlock()`.

Build this probe with the Android NDK and run it directly in the Android app or
shell domain:

```sh
aarch64-linux-android24-clang -std=c11 -O2 -Wall -Wextra -Werror -pthread \
  lorie-mutex-clock.c -o lorie-mutex-clock
./lorie-mutex-clock
```

A passing result requires both cases to acquire the lock at approximately the
same wall time while the realtime case performs fewer timeout retries and
consumes less waiting-thread CPU. On the Exynos 9611 qualification device, 25
of 25 runs passed: the old deadline produced 6,405-9,585 immediate timeouts and
used 49,760 us of CPU on average; the corrected deadline produced one real
timeout and used 190 us on average, a 99.6% reduction.
