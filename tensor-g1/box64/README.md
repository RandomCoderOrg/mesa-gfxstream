# Box64 / Steam compatibility experiments

This directory preserves the bounded helpers used while investigating x86
Linux applications from the Jammy PRoot. It is not a working Steam or Proton
distribution, and it does not modify the Panfrost driver.

The experiment has two distinct graphics choices:

- Native ARM64 Linux applications use this repository's open Panfrost OpenGL
  path on `/dev/mali0`.
- An x86 application under Box64 may instead load the rootless libhybris
  Vulkan bridge documented in [`../hybris-vulkan/README.md`](../hybris-vulkan/README.md).
  Passing a Vulkan-loader smoke does not prove that Steam, its Chromium-based
  web helpers, Wine, or a game will run.

## Files

- `abi-smoke.c` is a dependency-free guest ABI sanity check. Build it for the
  guest architecture being tested and verify that the emulator prints
  `ABI_BITS`, a PID, and the expected deterministic value before returning 0.
- `vulkan-smoke.c` opens `libvulkan.so.1`, creates an instance, enumerates
  physical devices, and prints `VULKAN_SMOKE_OK`. It deliberately stops before
  swapchain/window-system testing.
- `proot-box64` is an experimental replacement for the `proot` command used by
  uDroid. It moves `--rootfs` before `--qemu` because PRoot resolves the
  emulator path while parsing its arguments. Its Termux and Box64 paths are
  intentionally device-local and must be adjusted for another installation.
- `patches/0001-box32-rootless-shell-dispatch.patch` makes Box32 route `system`,
  `posix_spawn`, and `posix_spawnp` children back through Box64 when a rootless
  PRoot cannot use `binfmt_misc`. Apply it to the matching Box64 checkout with
  `git apply`; it is a dirty compatibility patch, not an upstream-ready fix.

## Current result

The helpers were enough to explore nested x86/x86-64 process dispatch and the
Vulkan-loader boundary. Steam itself did not reach a usable client window.
Steam web-helper error windows appeared briefly and exited. Some broad
launch/cleanup attempts coincided with the Termux process dying, but that cause
was not isolated. No Steam Big Picture session, Proton game, or
Winlator-style environment has been validated.

Keep future tests bounded: run the ABI and Vulkan probes first, use exact PIDs
when stopping processes, and monitor the Android low-memory killer before
starting Steam. A successful probe should be treated only as evidence for that
single boundary.
