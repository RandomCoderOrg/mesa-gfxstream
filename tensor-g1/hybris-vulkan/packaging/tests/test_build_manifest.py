#!/usr/bin/env python3

from __future__ import annotations

import argparse
import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).resolve().parents[1] / "build-manifest.py"
SPEC = importlib.util.spec_from_file_location("build_manifest", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
BUILD_MANIFEST = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD_MANIFEST)

VERIFY_PATH = Path(__file__).resolve().parents[1] / "verify-runtime.py"
VERIFY_SPEC = importlib.util.spec_from_file_location("verify_built_runtime", VERIFY_PATH)
assert VERIFY_SPEC is not None and VERIFY_SPEC.loader is not None
VERIFY_RUNTIME = importlib.util.module_from_spec(VERIFY_SPEC)
VERIFY_SPEC.loader.exec_module(VERIFY_RUNTIME)


class BuildManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        required_files = {
            "bin/udroid-gpu-run": "#!/bin/sh\n",
            "lib/mesa/dri/zink_dri.so": "not-an-elf\n",
            "share/vulkan/icd.d/sysvk.json": (
                '{"file_format_version":"1.0.0","ICD":'
                '{"library_path":"../../../lib/bridge/libsysvk.so",'
                '"api_version":"1.1.0"}}\n'
            ),
            "share/vulkan/explicit_layer.d/VkLayer_window_system_integration.json": (
                '{"file_format_version":"1.1.2","layer":'
                '{"name":"VK_LAYER_window_system_integration",'
                '"type":"GLOBAL",'
                '"library_path":"./libVkLayer_window_system_integration.so",'
                '"api_version":"1.1.0","implementation_version":"1",'
                '"description":"test"}}\n'
            ),
            "share/vulkan/explicit_layer.d/libVkLayer_window_system_integration.so": (
                "not-an-elf\n"
            ),
        }
        for relative, content in required_files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            path.chmod(0o755 if relative.startswith("bin/") else 0o644)
        (self.root / "lib/bridge/linker").mkdir(parents=True)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def args(self) -> argparse.Namespace:
        return argparse.Namespace(
            root=self.root,
            version="0.1.0-test.1",
            glibc_min="2.35",
            android_api_min=26,
            x11_ahb_protocol=1,
            source=[
                f"mesa={'1' * 40}",
                f"libhybris={'2' * 40}",
                f"sysvk={'3' * 40}",
                f"ginkage={'4' * 40}",
            ],
            patch=["test-patch"],
            readelf="readelf",
        )

    def test_builds_runtime_that_verifier_accepts(self) -> None:
        manifest = BUILD_MANIFEST.build(self.args())
        self.assertEqual(manifest["version"], "0.1.0-test.1")
        result = VERIFY_RUNTIME.verify(self.root)
        self.assertTrue(result["ok"])

    def test_rejects_absolute_vulkan_library(self) -> None:
        path = self.root / "share/vulkan/icd.d/sysvk.json"
        path.write_text(
            '{"file_format_version":"1.0.0","ICD":'
            '{"library_path":"/tmp/libsysvk.so","api_version":"1.1.0"}}\n',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(BUILD_MANIFEST.BuildError, "absolute Vulkan"):
            BUILD_MANIFEST.build(self.args())


if __name__ == "__main__":
    unittest.main()
