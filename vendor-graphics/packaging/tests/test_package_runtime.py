#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import tarfile
import tempfile
import unittest
from pathlib import Path

from runtime_fixture import RuntimeFixture, digest


MODULE_PATH = Path(__file__).resolve().parents[1] / "package-runtime.py"
SPEC = importlib.util.spec_from_file_location("package_runtime", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
PACKAGE_RUNTIME = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE_RUNTIME)


class PackageRuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.workspace = Path(self.temporary.name)
        self.runtime = self.workspace / "runtime"
        self.runtime.mkdir()
        RuntimeFixture(self.runtime)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_packages_one_versioned_root_and_checksum(self) -> None:
        output = self.workspace / "runtime.tar.xz"
        result = PACKAGE_RUNTIME.package(self.runtime, output, epoch=1234)
        self.assertEqual(
            result["archiveRoot"],
            "udroid-graphics-0.1.0-test.1-linux-aarch64-glibc",
        )
        self.assertEqual(result["sha256"], digest(output))
        self.assertEqual(
            output.with_name(f"{output.name}.sha256").read_text(encoding="utf-8"),
            f"{digest(output)}  {output.name}\n",
        )
        with tarfile.open(output, "r:xz") as archive:
            names = archive.getnames()
            self.assertEqual(names, sorted(names))
            self.assertTrue(
                all(
                    name == result["archiveRoot"]
                    or name.startswith(f"{result['archiveRoot']}/")
                    for name in names
                ),
            )
            runner = archive.getmember(f"{result['archiveRoot']}/bin/udroid-gpu-run")
            self.assertEqual(runner.mode, 0o755)
            self.assertEqual(runner.mtime, 1234)
            self.assertEqual(runner.uid, 0)
            self.assertEqual(runner.gid, 0)

    def test_same_runtime_and_epoch_produce_identical_archives(self) -> None:
        first = self.workspace / "first.tar.xz"
        second = self.workspace / "second.tar.xz"
        PACKAGE_RUNTIME.package(self.runtime, first, epoch=1234)
        PACKAGE_RUNTIME.package(self.runtime, second, epoch=1234)
        self.assertEqual(digest(first), digest(second))

    def test_rejects_output_inside_runtime(self) -> None:
        with self.assertRaisesRegex(PACKAGE_RUNTIME.PackageError, "inside the staging root"):
            PACKAGE_RUNTIME.package(self.runtime, self.runtime / "bad.tar.xz", epoch=0)


if __name__ == "__main__":
    unittest.main()
