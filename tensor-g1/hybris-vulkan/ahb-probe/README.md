# Rootless AHardwareBuffer lifecycle probe

This micro-probe validates the buffer contract needed by xMeM's Termux:X11
WSI without starting Vulkan, a browser, or a desktop. It deliberately uses the
same libhybris-to-`libandroid.so` boundary as the planned WSI.

Each case:

1. allocates a CPU-readable, CPU-writable, GPU-sampleable, GPU-renderable AHB;
2. records Android's actual row stride;
3. writes a deterministic RGBA pattern and completes any release fence;
4. sends the AHB through an `AF_UNIX` socket and receives a new reference;
5. releases the original reference before reading the receiver;
6. verifies description and visible-pixel hash through the received reference;
7. emits allocation, mapping, transport, and total timings as one JSON object.

The default widths straddle common 64-pixel boundaries and include 1919,
1920, and 1921. Ten iterations produce 90 bounded cases while using only a
32-pixel height for the wide stride checks.

Build inside the existing Jammy rootless bridge tree:

```sh
cd /root/hybris-rootless/ahb-probe
make -j4
```

Run through the existing per-process libhybris environment and capture JSONL:

```sh
tensor-vulkan-run ./ahb-probe 10 32 > ahb-lifecycle.jsonl
```

This probe does not claim X11 GPU Present offload. Passing it is the gate for
the next probe, which registers the received AHB with Termux:X11 and measures
the server's offload counter. The existing Ginkage DMA-BUF WSI remains
untouched and selectable throughout these tests.

The control flow also has a host-only mock test. It verifies padded strides,
reference lifetime, pattern hashes, JSON output, and failure accounting, but
does not represent Android performance or synchronization:

```sh
make host-test
```

After pulling a device result to the host, render the latency distribution and
per-stage median costs with:

```sh
./plot-results.py ahb-lifecycle.jsonl \
  --output ahb-lifecycle.png \
  --summary-json ahb-lifecycle-summary.json
```
