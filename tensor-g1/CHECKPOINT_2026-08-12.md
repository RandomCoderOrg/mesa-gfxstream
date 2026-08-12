# Graphics and media checkpoint — 2026-08-12

This index records the source-owning repository for the currently preserved
experimental patches. It does not promote any experimental backend to a
default.

| Area | Repository and branch | Commit | Verification |
| --- | --- | --- | --- |
| AHardwareBuffer lifecycle probe | `tensor-g1-proot-gpu`, `tensor-g1` | `c13b9a2a07d` | Host mock: 18/18 cases passed |
| Selectable xMeM AHB oracle | `tensor-g1-proot-gpu`, `tensor-g1` | `0e3b40979f2` | Shell and patch gates passed; Ginkage remains default |
| Termux:X11/KDE AHB investigation | `tensor-g1-proot-gpu`, `tensor-g1` | `c1b67350e0f` | Preserved as a rejected diagnostic patch with visual and latency results |
| Mesa v9 Android Kbase backend | `tensor-g1-mesa-upstream`, `tensor-g1-upstream-v9` | `4faddb444e6` | Prior device checkpoints preserved; no macOS Mesa toolchain available for a fresh build |
| Blender hair differential probes | `tensor-g1-mesa-upstream`, `tensor-g1-upstream-v9` | `771173015b1` | Shell, Python, JSON, and whitespace gates passed |
| Bounded Firefox/FMA probe | `fake-media-accel`, `feat/codec-conformance` | `416e847` | Protocol, H.264, IVF, AV1 parser, and Python gates passed |
| Cross-ABI FMA asset provenance | `udroid-android`, `feat/media-acceleration` | `304af51` | Six ELF assets, tamper tests, unit test, multi-ABI APK build, and unpacked-APK verification passed |

## Current boundaries

- Ginkage DMA-BUF copy remains the vendor-Vulkan fallback and default.
- xMeM AHB is an explicit, fail-closed oracle only.
- The Termux:X11 external-OES/cache patch is preserved but rejected for
  default use: it did not remove visible corruption or KDE panel latency.
- Panfrost/Kbase remains an experimental driver port, not an official or
  conformant driver.
- FMA browser diagnostics do not by themselves prove hardware decoding.
- uDroid media acceleration remains on its experimental feature branch.

Existing WIP stashes were not modified or applied.
