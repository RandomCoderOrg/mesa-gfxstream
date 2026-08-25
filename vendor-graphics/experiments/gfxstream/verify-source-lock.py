#!/usr/bin/env python3

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from urllib.parse import urlparse


REVISION = re.compile(r"^[0-9a-f]{40}$")
EXPECTED_SOURCES = {"rutabaga_gfx", "gfxstream", "mesa_guest"}


def fail(message: str) -> None:
    raise SystemExit(f"gfxstream source lock: {message}")


def main() -> None:
    path = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).with_name("source-lock.json")
    lock = json.loads(path.read_text(encoding="utf-8"))

    if lock.get("schema") != 1:
        fail("unsupported schema")
    if lock.get("target") != "android-arm64-bionic+linux-aarch64-glibc":
        fail("unexpected target")

    sources = lock.get("sources")
    if not isinstance(sources, dict) or set(sources) != EXPECTED_SOURCES:
        fail("source set is incomplete")

    for name, source in sources.items():
        if not isinstance(source, dict):
            fail(f"{name} is not an object")
        url = source.get("url", "")
        parsed = urlparse(url)
        if parsed.scheme != "https" or not parsed.netloc or not url.endswith(".git"):
            fail(f"{name} does not use an HTTPS git URL")
        if not REVISION.fullmatch(source.get("revision", "")):
            fail(f"{name} revision is not immutable")

    rutabaga = sources["rutabaga_gfx"]
    if rutabaga["url"] != "https://github.com/RandomCoderOrg/rutabaga_gfx.git":
        fail("patched Rutabaga must resolve to the project fork")
    if rutabaga.get("upstream") != "https://github.com/magma-gpu/rutabaga_gfx.git":
        fail("Rutabaga upstream provenance is missing")
    if not REVISION.fullmatch(rutabaga.get("upstream_revision", "")):
        fail("Rutabaga upstream revision is not immutable")

    print("PASS gfxstream sources are immutable and patched code uses a project fork")


if __name__ == "__main__":
    main()
