"""Shared release-time contract for the rootless vendor graphics component."""

from __future__ import annotations

import struct
from pathlib import Path


class ElfContractError(RuntimeError):
    """An ELF does not satisfy the runtime's load-time safety contract."""


LIBHYBRIS_COMMON_PATH = "lib/bridge/libhybris-common.so.1"
LIBHYBRIS_TLS_MIN_BYTES = 64 * 1024
LIBHYBRIS_TLS_MAX_BYTES = 128 * 1024
PT_DYNAMIC = 2
PT_TLS = 7
DT_NULL = 0
DT_RPATH = 15
DT_RUNPATH = 29

SOURCE_NAMES = (
    "mesa",
    "libhybris",
    "android_headers",
    "sysvk",
    "ginkage",
    "vulkan_headers",
    "wsi_headers",
    "vulkan_loader",
)

PROFILE_NAMES = (
    "vendor-vulkan:ginkage-ahb",
    "vendor-vulkan:ginkage-dmabuf-copy",
    "vendor-vulkan:ginkage-dmabuf-zero",
    "vendor-vulkan:shm",
)

REQUIRED_PATHS = (
    "sources.json",
    "THIRD_PARTY_NOTICES.md",
    "bin/udroid-gpu-run",
    "bin/udroid-gpu-probe",
    "lib/mesa/libEGL.so.1",
    "lib/mesa/libGL.so.1",
    "lib/mesa/libGLESv1_CM.so.1",
    "lib/mesa/libGLESv2.so.2",
    "lib/mesa/libglapi.so.0",
    "lib/mesa/dri/zink_dri.so",
    "lib/bridge/libahb-wrapper.so",
    "lib/bridge/libhardware.so.2",
    "lib/bridge/libhybris-common.so.1",
    "lib/bridge/libsysvk.so",
    "lib/bridge/libvulkan.so.1",
    "lib/bridge/libhybris/linker/q.so",
    "share/vulkan/icd.d/sysvk.json",
    "share/vulkan/explicit_layer.d/VkLayer_window_system_integration.json",
    "share/vulkan/explicit_layer.d/libVkLayer_window_system_integration.so",
    "libexec/egl-gles3-instancing",
    "libexec/vulkan-core-features",
    "libexec/vulkan-thread-lifecycle",
    "libexec/run-wsi-qualification.sh",
    "libexec/x11-ahb-present-probe",
    "libexec/x11-buffer-transport-protocol",
    "libexec/x11-present-stats",
)

REQUIRED_EXECUTABLES = (
    "bin/udroid-gpu-run",
    "bin/udroid-gpu-probe",
    "libexec/egl-gles3-instancing",
    "libexec/vulkan-core-features",
    "libexec/vulkan-thread-lifecycle",
    "libexec/run-wsi-qualification.sh",
    "libexec/x11-ahb-present-probe",
    "libexec/x11-buffer-transport-protocol",
    "libexec/x11-present-stats",
)


def inspect_aarch64_elf(path: Path) -> dict[str, object]:
    """Read the ELF properties needed by the offline package verifier."""

    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ElfContractError(f"cannot read ELF {path}: {exc}") from exc
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise ElfContractError(f"not an ELF file: {path}")
    if data[4] != 2 or data[5] != 1 or data[6] != 1:
        raise ElfContractError(f"ELF must be 64-bit little-endian: {path}")

    elf_type, machine = struct.unpack_from("<HH", data, 16)
    if elf_type != 3 or machine != 183:
        raise ElfContractError(f"ELF must be an AArch64 shared object: {path}")
    program_offset = struct.unpack_from("<Q", data, 32)[0]
    program_size, program_count = struct.unpack_from("<HH", data, 54)
    if program_size < 56 or program_count == 0 or program_count > 4096:
        raise ElfContractError(f"invalid ELF program-header table: {path}")
    if program_offset > len(data) or program_count * program_size > len(data) - program_offset:
        raise ElfContractError(f"truncated ELF program-header table: {path}")

    programs: list[tuple[int, int, int, int, int]] = []
    tls_sizes: list[int] = []
    for index in range(program_count):
        offset = program_offset + index * program_size
        header = struct.unpack_from("<IIQQQQQQ", data, offset)
        kind, _flags, file_offset, virtual_address = header[:4]
        _physical_address, file_size, memory_size, _align = header[4:]
        if file_offset > len(data) or file_size > len(data) - file_offset:
            raise ElfContractError(f"ELF segment lies outside the file: {path}")
        programs.append((kind, file_offset, virtual_address, file_size, memory_size))
        if kind == PT_TLS:
            tls_sizes.append(memory_size)

    dynamic_tags: set[int] = set()
    for kind, file_offset, _virtual_address, file_size, _memory_size in programs:
        if kind != PT_DYNAMIC:
            continue
        if file_size % 16:
            raise ElfContractError(f"malformed ELF dynamic segment: {path}")
        for offset in range(file_offset, file_offset + file_size, 16):
            tag, _value = struct.unpack_from("<qQ", data, offset)
            if tag == DT_NULL:
                break
            dynamic_tags.add(tag)

    return {"tlsSizes": tls_sizes, "dynamicTags": dynamic_tags}


def validate_libhybris_common(path: Path) -> int:
    """Require the isolated TLS reserve and prohibit embedded search paths."""

    elf = inspect_aarch64_elf(path)
    tls_sizes = elf["tlsSizes"]
    assert isinstance(tls_sizes, list)
    if len(tls_sizes) != 1:
        raise ElfContractError(f"libhybris-common must contain one PT_TLS segment: {path}")
    tls_size = tls_sizes[0]
    if not isinstance(tls_size, int) or not (
        LIBHYBRIS_TLS_MIN_BYTES <= tls_size <= LIBHYBRIS_TLS_MAX_BYTES
    ):
        raise ElfContractError(
            "libhybris-common PT_TLS reserve is outside the supported "
            f"{LIBHYBRIS_TLS_MIN_BYTES}..{LIBHYBRIS_TLS_MAX_BYTES} byte range: {tls_size}"
        )
    dynamic_tags = elf["dynamicTags"]
    assert isinstance(dynamic_tags, set)
    if DT_RPATH in dynamic_tags or DT_RUNPATH in dynamic_tags:
        raise ElfContractError("libhybris-common must not contain RPATH or RUNPATH")
    return tls_size
