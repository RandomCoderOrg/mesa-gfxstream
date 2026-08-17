#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "verify-runtime.py"
SPEC = importlib.util.spec_from_file_location("verify_runtime", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFY_RUNTIME = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY_RUNTIME)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class RuntimeFixture:
    def __init__(self, root: Path) -> None:
        self.root = root
        executable = root / "bin/udroid-gpu-run"
        executable.parent.mkdir(parents=True)
        executable.write_text("#!/bin/sh\nexec \"$@\"\n", encoding="utf-8")
        executable.chmod(0o755)
        link = root / "bin/udroid-gpu-launch"
        link.symlink_to("udroid-gpu-run")
        self.manifest = {
            "schema": 1,
            "component": "udroid-graphics",
            "version": "0.1.0-test.1",
            "target": {
                "os": "linux",
                "arch": "aarch64",
                "libc": "glibc",
                "androidApiMin": 26,
            },
            "profiles": ["vendor-vulkan:ginkage-ahb"],
            "sources": {
                "mesa": "1" * 40,
                "libhybris": "2" * 40,
                "sysvk": "3" * 40,
                "ginkage": "4" * 40,
            },
            "files": [
                {
                    "path": "bin/udroid-gpu-run",
                    "type": "file",
                    "mode": "0755",
                    "sha256": digest(executable),
                },
                {
                    "path": "bin/udroid-gpu-launch",
                    "type": "symlink",
                    "mode": "0777",
                    "target": "udroid-gpu-run",
                },
            ],
        }
        self.write_metadata()

    def write_metadata(self) -> None:
        manifest_path = self.root / "manifest.json"
        manifest_path.write_text(
            json.dumps(self.manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        sums = {
            "bin/udroid-gpu-run": digest(self.root / "bin/udroid-gpu-run"),
            "manifest.json": digest(manifest_path),
        }
        (self.root / "SHA256SUMS").write_text(
            "".join(f"{value}  {name}\n" for name, value in sorted(sums.items())),
            encoding="utf-8",
        )


class VerifyRuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.fixture = RuntimeFixture(self.root)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_accepts_complete_runtime(self) -> None:
        result = VERIFY_RUNTIME.verify(self.root)
        self.assertTrue(result["ok"])
        self.assertEqual(result["filesVerified"], 3)

    def test_rejects_changed_payload(self) -> None:
        (self.root / "bin/udroid-gpu-run").write_text("changed\n", encoding="utf-8")
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "hash mismatch"):
            VERIFY_RUNTIME.verify(self.root)

    def test_rejects_unlisted_payload(self) -> None:
        (self.root / "surprise.so").write_bytes(b"unexpected")
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "payload inventory mismatch"):
            VERIFY_RUNTIME.verify(self.root)

    def test_rejects_escaping_symlink(self) -> None:
        link = self.root / "bin/udroid-gpu-launch"
        link.unlink()
        link.symlink_to("../../outside")
        self.fixture.manifest["files"][1]["target"] = "../../outside"
        self.fixture.write_metadata()
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "symlink escapes"):
            VERIFY_RUNTIME.verify(self.root)

    def test_rejects_unsafe_manifest_path(self) -> None:
        self.fixture.manifest["files"][0]["path"] = "../escape"
        self.fixture.write_metadata()
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "unsafe component path"):
            VERIFY_RUNTIME.verify(self.root)


if __name__ == "__main__":
    unittest.main()
