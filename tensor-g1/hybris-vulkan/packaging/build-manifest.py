#!/usr/bin/env python3
"""Finalize a staged uDroid graphics runtime and generate its inventories."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List


SOURCE_NAMES = ("mesa", "libhybris", "sysvk", "ginkage")
PROFILE_NAMES = (
    "vendor-vulkan:ginkage-ahb",
    "vendor-vulkan:ginkage-dmabuf-copy",
    "vendor-vulkan:ginkage-dmabuf-zero",
    "vendor-vulkan:shm",
)
REQUIRED_PATHS = (
    "bin/udroid-gpu-run",
    "lib/mesa/dri/zink_dri.so",
    "lib/bridge/linker",
    "share/vulkan/icd.d/sysvk.json",
    "share/vulkan/explicit_layer.d/VkLayer_window_system_integration.json",
    "share/vulkan/explicit_layer.d/libVkLayer_window_system_integration.so",
)
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")


class BuildError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def is_elf(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    try:
        with path.open("rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError:
        return False


def dynamic_section(path: Path, readelf: str) -> str:
    try:
        result = subprocess.run(
            [readelf, "-dW", str(path)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except FileNotFoundError as exc:
        raise BuildError(f"readelf is not installed: {readelf}") from exc
    except subprocess.CalledProcessError as exc:
        raise BuildError(f"readelf failed for {path}: {exc.stderr.strip()}") from exc
    return result.stdout


def check_elf(path: Path, root: Path, readelf: str) -> List[str]:
    dynamic = dynamic_section(path, readelf)
    for kind, value in re.findall(r"\((RPATH|RUNPATH)\).*?\[(.*?)\]", dynamic):
        for component in value.split(":"):
            if component.startswith("/"):
                relative = path.relative_to(root)
                raise BuildError(f"absolute {kind} in {relative}: {component}")
            if component and "$ORIGIN" not in component:
                relative = path.relative_to(root)
                raise BuildError(f"non-relocatable {kind} in {relative}: {component}")
    return re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)


def check_vulkan_json(path: Path, root: Path) -> None:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BuildError(f"cannot parse Vulkan manifest {path.relative_to(root)}: {exc}") from exc
    record = document.get("ICD") or document.get("layer")
    if not isinstance(record, dict):
        raise BuildError(f"missing ICD/layer object in {path.relative_to(root)}")
    library_path = record.get("library_path")
    if not isinstance(library_path, str) or not library_path:
        raise BuildError(f"missing library_path in {path.relative_to(root)}")
    if os.path.isabs(library_path):
        raise BuildError(f"absolute Vulkan library_path in {path.relative_to(root)}")


def payload_paths(root: Path) -> Iterable[Path]:
    for directory, dir_names, file_names in os.walk(root, followlinks=False):
        base = Path(directory)
        for name in list(dir_names):
            candidate = base / name
            if candidate.is_symlink():
                dir_names.remove(name)
                yield candidate
        for name in file_names:
            candidate = base / name
            if candidate.name not in {"manifest.json", "SHA256SUMS"}:
                yield candidate


def safe_symlink_target(path: Path, root: Path) -> str:
    target = os.readlink(path)
    if os.path.isabs(target):
        raise BuildError(f"absolute symlink target: {path.relative_to(root)} -> {target}")
    resolved = (path.parent / target).resolve(strict=False)
    try:
        resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise BuildError(f"escaping symlink: {path.relative_to(root)} -> {target}") from exc
    return target


def make_entry(path: Path, root: Path) -> Dict[str, Any]:
    relative = path.relative_to(root).as_posix()
    mode = f"0{stat.S_IMODE(path.lstat().st_mode):03o}"
    if path.is_symlink():
        return {
            "path": relative,
            "type": "symlink",
            "mode": mode,
            "target": safe_symlink_target(path, root),
        }
    if not path.is_file():
        raise BuildError(f"unsupported payload type: {relative}")
    return {
        "path": relative,
        "type": "file",
        "mode": mode,
        "sha256": sha256_file(path),
    }


def parse_source(values: List[str]) -> Dict[str, str]:
    sources: Dict[str, str] = {}
    for value in values:
        name, separator, revision = value.partition("=")
        if not separator or name not in SOURCE_NAMES or not REVISION_RE.fullmatch(revision):
            raise BuildError(f"invalid --source value: {value}")
        sources[name] = revision
    missing = sorted(set(SOURCE_NAMES) - set(sources))
    if missing:
        raise BuildError(f"missing source revisions: {', '.join(missing)}")
    return sources


def build(args: argparse.Namespace) -> Dict[str, Any]:
    root = args.root.resolve()
    if not root.is_dir():
        raise BuildError(f"staging root is not a directory: {root}")
    for relative in REQUIRED_PATHS:
        if not (root / relative).exists():
            raise BuildError(f"required runtime path is missing: {relative}")
    for stale in (root / "manifest.json", root / "SHA256SUMS"):
        if stale.exists() or stale.is_symlink():
            stale.unlink()

    sources = parse_source(args.source)
    entries: List[Dict[str, Any]] = []
    needed: Dict[str, List[str]] = {}
    for path in sorted(payload_paths(root), key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix()
        if "/.git/" in f"/{relative}/" or relative.startswith(("build/", "src/")):
            raise BuildError(f"source/build artifact in runtime: {relative}")
        if is_elf(path):
            needed[relative] = check_elf(path, root, args.readelf)
        if relative.startswith("share/vulkan/") and relative.endswith(".json"):
            check_vulkan_json(path, root)
        entries.append(make_entry(path, root))

    manifest: Dict[str, Any] = {
        "schema": 1,
        "component": "udroid-graphics",
        "version": args.version,
        "target": {
            "os": "linux",
            "arch": "aarch64",
            "libc": "glibc",
            "glibcMin": args.glibc_min,
            "androidApiMin": args.android_api_min,
        },
        "x11": {"ahardwareBufferProtocol": args.x11_ahb_protocol},
        "profiles": list(PROFILE_NAMES),
        "sources": sources,
        "patches": args.patch,
        "knownMissingVulkanFeatures": ["fillModeNonSolid", "shaderClipDistance"],
        "needed": needed,
        "files": entries,
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    checksums = [("manifest.json", sha256_file(manifest_path))]
    checksums.extend(
        (entry["path"], entry["sha256"])
        for entry in entries
        if entry["type"] == "file"
    )
    (root / "SHA256SUMS").write_text(
        "".join(f"{digest}  {relative}\n" for relative, digest in sorted(checksums)),
        encoding="utf-8",
    )
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="staged runtime root")
    parser.add_argument("--version", required=True)
    parser.add_argument("--glibc-min", default="2.35")
    parser.add_argument("--android-api-min", type=int, default=26)
    parser.add_argument("--x11-ahb-protocol", type=int, default=1)
    parser.add_argument(
        "--source",
        action="append",
        default=[],
        metavar="NAME=40_HEX_REVISION",
        help="repeat for mesa, libhybris, sysvk and ginkage",
    )
    parser.add_argument("--patch", action="append", default=[])
    parser.add_argument("--readelf", default="readelf")
    args = parser.parse_args()
    try:
        manifest = build(args)
    except BuildError as exc:
        print(f"component build failed: {exc}", file=sys.stderr)
        return 1
    print(
        f"built manifest for {manifest['component']} {manifest['version']} "
        f"({len(manifest['files'])} payload entries)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
