#!/usr/bin/env python3

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


PACKAGING_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGING_ROOT))
from runtime_contract import SOURCE_NAMES  # noqa: E402


class SourceLockTests(unittest.TestCase):
    def test_lock_covers_every_manifest_source_with_immutable_revisions(self) -> None:
        path = PACKAGING_ROOT.parent / "build" / "source-lock.json"
        lock = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(lock["schema"], 1)
        self.assertEqual(lock["target"], "linux-aarch64-glibc")
        self.assertEqual(set(lock["sources"]), set(SOURCE_NAMES))
        for name, source in lock["sources"].items():
            self.assertRegex(source["url"], r"^https://")
            self.assertRegex(source["revision"], r"^[0-9a-f]{40}$", name)


if __name__ == "__main__":
    unittest.main()
