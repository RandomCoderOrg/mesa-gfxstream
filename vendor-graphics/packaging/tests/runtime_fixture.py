from __future__ import annotations

import hashlib
import json
import os
import stat
import struct
import sys
from pathlib import Path

PACKAGING_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGING_ROOT))
from runtime_contract import PROFILE_NAMES, REQUIRED_EXECUTABLES, REQUIRED_PATHS  # noqa: E402


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def minimal_aarch64_shared_object(
    tls_bytes: int = 0x11000,
    dynamic_tags: tuple[int, ...] = (),
) -> bytes:
    """Build the smallest ELF needed to exercise the offline contract parser."""

    program_count = 1 + bool(dynamic_tags)
    program_offset = 64
    dynamic_offset = program_offset + program_count * 56
    dynamic_size = (len(dynamic_tags) + 1) * 16 if dynamic_tags else 0
    data = bytearray(dynamic_offset + dynamic_size)
    identifier = b"\x7fELF" + bytes((2, 1, 1, 0, 0)) + bytes(7)
    struct.pack_into(
        "<16sHHIQQQIHHHHHH",
        data,
        0,
        identifier,
        3,
        183,
        1,
        0,
        program_offset,
        0,
        0,
        64,
        56,
        program_count,
        0,
        0,
        0,
    )
    struct.pack_into("<IIQQQQQQ", data, program_offset, 7, 4, 0, 0, 0, 0, tls_bytes, 64)
    if dynamic_tags:
        struct.pack_into(
            "<IIQQQQQQ",
            data,
            program_offset + 56,
            2,
            4,
            dynamic_offset,
            dynamic_offset,
            0,
            dynamic_size,
            dynamic_size,
            8,
        )
        for index, tag in enumerate((*dynamic_tags, 0)):
            struct.pack_into("<qQ", data, dynamic_offset + index * 16, tag, 0)
    return bytes(data)


class RuntimeFixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        for relative in REQUIRED_PATHS:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if relative == "share/vulkan/icd.d/sysvk.json":
                content = (
                    '{"file_format_version":"1.0.0","ICD":'
                    '{"library_path":"../../../lib/bridge/libsysvk.so",'
                    '"api_version":"1.1.0"}}\n'
                )
            elif relative == "share/vulkan/explicit_layer.d/VkLayer_window_system_integration.json":
                content = (
                    '{"file_format_version":"1.1.2","layer":'
                    '{"name":"VK_LAYER_window_system_integration",'
                    '"type":"GLOBAL",'
                    '"library_path":"./libVkLayer_window_system_integration.so",'
                    '"api_version":"1.1.0","implementation_version":"1",'
                    '"description":"test"}}\n'
                )
            elif relative in REQUIRED_EXECUTABLES:
                content = "#!/bin/sh\nexit 0\n"
            elif relative.endswith(".json"):
                content = "{}\n"
            else:
                content = f"fixture:{relative}\n"
            if relative == "lib/bridge/libhybris-common.so.1":
                path.write_bytes(minimal_aarch64_shared_object())
            else:
                path.write_text(content, encoding="utf-8")
            path.chmod(0o755 if relative in REQUIRED_EXECUTABLES else 0o644)

        self.manifest = {
            "schema": 1,
            "component": "udroid-graphics",
            "version": "0.1.0-test.1",
            "target": {
                "os": "linux",
                "arch": "aarch64",
                "libc": "glibc",
                "glibcMin": "2.35",
                "androidApiMin": 26,
            },
            "x11": {"ahardwareBufferProtocol": 1},
            "profiles": list(PROFILE_NAMES),
            "sources": {
                "mesa": "1" * 40,
                "libhybris": "2" * 40,
                "android_headers": "3" * 40,
                "sysvk": "4" * 40,
                "ginkage": "5" * 40,
                "vulkan_headers": "6" * 40,
                "wsi_headers": "7" * 40,
                "vulkan_loader": "8" * 40,
            },
            "files": [],
        }
        self.write_metadata()

    def write_metadata(self) -> None:
        entries = []
        for relative in REQUIRED_PATHS:
            path = self.root / relative
            if path.is_symlink():
                entries.append(
                    {
                        "path": relative,
                        "type": "symlink",
                        "mode": f"0{stat.S_IMODE(path.lstat().st_mode):03o}",
                        "target": os.readlink(path),
                    },
                )
            else:
                entries.append(
                    {
                        "path": relative,
                        "type": "file",
                        "mode": f"0{stat.S_IMODE(path.stat().st_mode):03o}",
                        "sha256": digest(path),
                    },
                )
        self.manifest["files"] = entries
        manifest_path = self.root / "manifest.json"
        manifest_path.write_text(
            json.dumps(self.manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        sums = {
            relative: digest(self.root / relative)
            for relative in REQUIRED_PATHS
            if not (self.root / relative).is_symlink()
        }
        sums["manifest.json"] = digest(manifest_path)
        (self.root / "SHA256SUMS").write_text(
            "".join(f"{value}  {name}\n" for name, value in sorted(sums.items())),
            encoding="utf-8",
        )
