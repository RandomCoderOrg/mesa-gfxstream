#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

from runtime_fixture import RuntimeFixture


MODULE_PATH = Path(__file__).resolve().parents[1] / "verify-runtime.py"
SPEC = importlib.util.spec_from_file_location("verify_runtime", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
VERIFY_RUNTIME = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY_RUNTIME)


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
        self.assertEqual(result["filesVerified"], len(self.fixture.manifest["files"]) + 1)

    def test_rejects_changed_payload(self) -> None:
        (self.root / "bin/udroid-gpu-run").write_text("changed\n", encoding="utf-8")
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "hash mismatch"):
            VERIFY_RUNTIME.verify(self.root)

    def test_rejects_unlisted_payload(self) -> None:
        (self.root / "surprise.so").write_bytes(b"unexpected")
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "payload inventory mismatch"):
            VERIFY_RUNTIME.verify(self.root)

    def test_rejects_non_executable_probe(self) -> None:
        (self.root / "bin/udroid-gpu-probe").chmod(0o644)
        self.fixture.write_metadata()
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "not executable"):
            VERIFY_RUNTIME.verify(self.root)

    def test_rejects_runtime_without_ahardwarebuffer_profile(self) -> None:
        self.fixture.manifest["profiles"] = ["vendor-vulkan:shm"]
        self.fixture.write_metadata()
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "AHardwareBuffer"):
            VERIFY_RUNTIME.verify(self.root)

    def test_rejects_escaping_symlink(self) -> None:
        relative = "lib/bridge/libahb-wrapper.so"
        link = self.root / relative
        link.unlink()
        link.symlink_to("../../../outside")
        self.fixture.write_metadata()
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "symlink escapes"):
            VERIFY_RUNTIME.verify(self.root)

    def test_rejects_unsafe_manifest_path(self) -> None:
        self.fixture.manifest["files"][0]["path"] = "../escape"
        (self.root / "manifest.json").write_text(
            json.dumps(self.fixture.manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(VERIFY_RUNTIME.VerificationError, "unsafe component path"):
            VERIFY_RUNTIME.verify(self.root)


if __name__ == "__main__":
    unittest.main()
