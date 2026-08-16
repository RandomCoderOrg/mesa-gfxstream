#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "build-release-index.py"
SPEC = importlib.util.spec_from_file_location("build_release_index", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
BUILD_INDEX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD_INDEX)


class BuildReleaseIndexTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.metadata = self.root / "package.json"
        self.metadata.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "component": "udroid-graphics",
                    "version": "0.1.0-dev.1",
                    "target": {
                        "os": "linux",
                        "arch": "aarch64",
                        "libc": "glibc",
                        "glibcMin": "2.35",
                        "androidApiMin": 26,
                    },
                    "profiles": ["vendor-vulkan:ginkage-ahb"],
                    "archive": "udroid-graphics-0.1.0-dev.1-linux-aarch64-glibc.tar.xz",
                    "format": "tar.xz",
                    "sha256": "1" * 64,
                    "manifestSha256": "2" * 64,
                    "size": 1234,
                },
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_builds_explicit_recommended_asset(self) -> None:
        result = BUILD_INDEX.build_index(
            [self.metadata],
            "https://github.com/RandomCoderOrg/udroid-gpu-runtime/releases/download/v0.1.0",
            "0.1.0-dev.1",
            1234,
        )
        self.assertEqual(result["recommended"]["linux-aarch64-glibc"], "0.1.0-dev.1")
        asset = result["releases"][0]["asset"]
        self.assertEqual(asset["format"], "tar.xz")
        self.assertTrue(asset["url"].endswith(asset["name"]))
        self.assertEqual(result["generatedAt"], "1970-01-01T00:20:34Z")

    def test_rejects_mutable_http_asset_base(self) -> None:
        with self.assertRaisesRegex(BUILD_INDEX.IndexError, "HTTPS"):
            BUILD_INDEX.build_index(
                [self.metadata],
                "http://example.test/assets",
                "0.1.0-dev.1",
                0,
            )

    def test_rejects_filename_that_does_not_match_identity(self) -> None:
        value = json.loads(self.metadata.read_text(encoding="utf-8"))
        value["archive"] = "something-else.tar.xz"
        self.metadata.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(BUILD_INDEX.IndexError, "unexpected archive name"):
            BUILD_INDEX.build_index(
                [self.metadata],
                "https://example.test/assets",
                "0.1.0-dev.1",
                0,
            )


if __name__ == "__main__":
    unittest.main()
