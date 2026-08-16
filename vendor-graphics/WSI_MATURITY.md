# WSI maturity tracker

The runtime keeps Ginkage and xMeM selectable. xMeM is the small upstream WSI
oracle being hardened here; passing it does not remove Ginkage or force an
installed distribution to use one acceleration route. Profile selection and
rollback remain explicit.

```mermaid
flowchart LR
    A["Application"] --> B["Vendor Vulkan"]
    B --> C["AHardwareBuffer swapchain"]
    C --> D["DRI3 private transport"]
    D --> E["Termux:X11 Present"]
    E --> F["Android renderer"]

    B -. "Vulkan fence wait" .-> C
    D -. "Present Complete and Idle" .-> C
```

## Resolved in the Exynos proof

| Contract | State | Evidence |
| --- | --- | --- |
| Image memory requirements | Fixed | Allocation size and memory-type bits now come from the created Vulkan image |
| Cleanup initialization | Fixed | Memory, layout, pixmap, and AHB fields start in a safe empty state |
| Image identity | Fixed | Deferred allocation binds the original application-visible `VkImage`; it no longer creates and leaks a replacement image |
| Surface format contract | Fixed | Surface enumeration advertises only AHB-exportable formats; the selected format remains identical through image-view creation, allocation, and transport |
| Color encoding | Fixed | BGRA sRGB maps to RGBA sRGB instead of silently becoming UNORM |
| Channel semantics | Fixed | Paired modifiers `1257` and `1255` identify negotiated RGBA and BGRA content |
| Teardown wake | Fixed | A Present MSC notification wakes the event thread before join |
| Direct presentation | Pass | Mali-G72 red frame, clean exit, 1/1 Termux:X11 Present copies GPU-offloaded |
| Rapid lifecycle soak | Pass | 25/25 image views and swapchains reached clean exit; 25/25 Present copies were GPU-offloaded in an isolated log run |

## Promotion gates still open

1. Add bounded error handling to the AHardwareBuffer socket handshake so a
   dead peer cannot block the producer or X server indefinitely.
2. Carry acquire/release synchronization to X Present with an explicit fence.
   The current implementation is correct but waits a Vulkan fence on the CPU
   before Present, which can cost compositor latency.
3. Advertise only presentation modes whose semantics are implemented. MAILBOX
   needs real queued-frame replacement; until then FIFO is the release target.
4. Correctly handle resize, surface loss, retired swapchains, Present serial
   wrap, and X connection teardown under repeated stress.
5. Remove unnecessary sync-FD extension gates or use sync-FD end to end. The
   X11 backend currently checks external sync-FD support but uses a normal
   Vulkan presentation fence.
6. Version the paired private transport and reject mismatched WSI/server builds
   with a clear diagnostic rather than corrupted output.

## Qualification sequence

Every device/build pair advances independently:

```mermaid
flowchart TD
    A["Discovery baseline"] --> B["Bionic TLS isolation"]
    B --> C["AHB allocation and transport"]
    C --> D["Vulkan lifecycle"]
    D --> E["Direct Vulkan XCB Present"]
    E --> F["Present offload and teardown"]
    F --> G["Matched Mesa and Zink"]
    G --> H["Compositor micro-workloads"]
    H --> I["Desktop and application soak"]
```

No later pass erases an earlier failure. Performance results are compared only
between runs with the same resolution, presentation mode, thermal state, and
workload definition.
