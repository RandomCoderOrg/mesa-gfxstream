#!/usr/bin/env python3
"""Verify an extracted uDroid graphics runtime without executing its files."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
from pathlib import Path, PurePosixPath
from typing import Any, Dict, Iterable, List, Set

sys.path.insert(0, str(Path(__file__).resolve().parent))
from runtime_contract import (  # noqa: E402
    ElfContractError,
    LIBHYBRIS_COMMON_PATH,
    PROFILE_NAMES,
    REQUIRED_EXECUTABLES,
    REQUIRED_PATHS,
    SOURCE_NAMES,
    validate_libhybris_common,
)

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$")
SUPPORTED_TARGET = ("linux", "aarch64", "glibc")


class VerificationError(RuntimeError):
    pass


def safe_relative_path(raw: Any) -> PurePosixPath:
    if not isinstance(raw, str) or not raw:
        raise VerificationError("manifest contains an empty or non-string path")
    path = PurePosixPath(raw)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        raise VerificationError(f"unsafe component path: {raw!r}")
    if "\\" in raw or raw.startswith("~"):
        raise VerificationError(f"non-portable component path: {raw!r}")
    return path


def safe_symlink_target(raw: Any) -> str:
    if not isinstance(raw, str) or not raw:
        raise VerificationError("manifest contains an empty or non-string symlink target")
    if PurePosixPath(raw).is_absolute() or "\\" in raw:
        raise VerificationError(f"unsafe symlink target: {raw!r}")
    return raw


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json(path: Path) -> Dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise VerificationError(f"cannot read {path.name}: {exc}") from exc
    if not isinstance(value, dict):
        raise VerificationError(f"{path.name} must contain a JSON object")
    return value


def parse_checksums(path: Path) -> Dict[str, str]:
    checksums: Dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise VerificationError(f"cannot read SHA256SUMS: {exc}") from exc
    for number, line in enumerate(lines, 1):
        if not line:
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            raise VerificationError(f"invalid SHA256SUMS line {number}")
        digest, raw_path = match.groups()
        relative = safe_relative_path(raw_path).as_posix()
        if relative in checksums:
            raise VerificationError(f"duplicate checksum path: {relative}")
        checksums[relative] = digest
    if not checksums:
        raise VerificationError("SHA256SUMS is empty")
    return checksums


def validate_manifest(manifest: Dict[str, Any]) -> Dict[str, Dict[str, Any]]:
    if manifest.get("schema") != 1:
        raise VerificationError("unsupported manifest schema")
    if manifest.get("component") != "udroid-graphics":
        raise VerificationError("unexpected component identity")
    version = manifest.get("version")
    if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
        raise VerificationError("invalid component version")

    target = manifest.get("target")
    if not isinstance(target, dict):
        raise VerificationError("target must be an object")
    actual_target = (target.get("os"), target.get("arch"), target.get("libc"))
    if actual_target != SUPPORTED_TARGET:
        raise VerificationError(f"unsupported target: {actual_target!r}")
    if not isinstance(target.get("androidApiMin"), int) or target["androidApiMin"] < 26:
        raise VerificationError("androidApiMin must be an integer of at least 26")
    x11 = manifest.get("x11")
    if not isinstance(x11, dict) or x11.get("ahardwareBufferProtocol") != 1:
        raise VerificationError("unsupported X11 AHardwareBuffer protocol")

    profiles = manifest.get("profiles")
    if not isinstance(profiles, list) or not profiles or not all(
        isinstance(profile, str) and profile for profile in profiles
    ):
        raise VerificationError("profiles must be a non-empty string list")
    if len(profiles) != len(set(profiles)):
        raise VerificationError("profiles contains duplicates")
    if not set(profiles).issubset(PROFILE_NAMES):
        raise VerificationError("profiles contains an unsupported launch profile")
    if PROFILE_NAMES[0] not in profiles:
        raise VerificationError("profiles is missing the AHardwareBuffer launch profile")

    sources = manifest.get("sources")
    if not isinstance(sources, dict) or set(sources) != set(SOURCE_NAMES):
        raise VerificationError("sources is missing a required revision")
    for name, revision in sources.items():
        if not isinstance(name, str) or not isinstance(revision, str) or not REVISION_RE.fullmatch(revision):
            raise VerificationError(f"invalid source revision for {name!r}")

    raw_files = manifest.get("files")
    if not isinstance(raw_files, list) or not raw_files:
        raise VerificationError("files must be a non-empty list")
    files: Dict[str, Dict[str, Any]] = {}
    for entry in raw_files:
        if not isinstance(entry, dict):
            raise VerificationError("file entry must be an object")
        relative = safe_relative_path(entry.get("path")).as_posix()
        if relative in files:
            raise VerificationError(f"duplicate manifest path: {relative}")
        kind = entry.get("type", "file")
        if kind not in {"file", "symlink"}:
            raise VerificationError(f"unsupported entry type for {relative}")
        if kind == "file" and not SHA256_RE.fullmatch(str(entry.get("sha256", ""))):
            raise VerificationError(f"invalid sha256 for {relative}")
        if kind == "symlink":
            safe_symlink_target(entry.get("target"))
        mode = entry.get("mode")
        if not isinstance(mode, str) or not re.fullmatch(r"0[0-7]{3}", mode):
            raise VerificationError(f"invalid mode for {relative}")
        files[relative] = entry
    missing_required = sorted(set(REQUIRED_PATHS) - set(files))
    if missing_required:
        raise VerificationError(f"required runtime paths are missing: {missing_required}")
    for relative in REQUIRED_EXECUTABLES:
        mode = int(files[relative]["mode"], 8)
        if mode & 0o111 == 0:
            raise VerificationError(f"required runtime path is not executable: {relative}")
    return files


def iter_payload_paths(root: Path) -> Iterable[str]:
    for directory, dir_names, file_names in os.walk(root, followlinks=False):
        base = Path(directory)
        for name in list(dir_names):
            candidate = base / name
            if candidate.is_symlink():
                dir_names.remove(name)
                yield candidate.relative_to(root).as_posix()
        for name in file_names:
            candidate = base / name
            relative = candidate.relative_to(root).as_posix()
            if relative not in {"manifest.json", "SHA256SUMS"}:
                yield relative


def verify_symlink(root: Path, relative: str, entry: Dict[str, Any]) -> None:
    path = root / relative
    if not path.is_symlink():
        raise VerificationError(f"expected symlink: {relative}")
    actual_target = os.readlink(path)
    if actual_target != entry["target"]:
        raise VerificationError(f"symlink target mismatch: {relative}")
    resolved = (path.parent / actual_target).resolve(strict=False)
    try:
        resolved.relative_to(root.resolve())
    except ValueError as exc:
        raise VerificationError(f"symlink escapes component root: {relative}") from exc


def verify(root: Path) -> Dict[str, Any]:
    if not root.is_dir():
        raise VerificationError(f"runtime root is not a directory: {root}")
    manifest_path = root / "manifest.json"
    sums_path = root / "SHA256SUMS"
    manifest = load_json(manifest_path)
    files = validate_manifest(manifest)
    checksums = parse_checksums(sums_path)

    expected_checksum_paths: Set[str] = {"manifest.json"}
    expected_checksum_paths.update(
        relative for relative, entry in files.items() if entry.get("type", "file") == "file"
    )
    if set(checksums) != expected_checksum_paths:
        missing = sorted(expected_checksum_paths - set(checksums))
        extra = sorted(set(checksums) - expected_checksum_paths)
        raise VerificationError(f"checksum inventory mismatch; missing={missing}, extra={extra}")

    actual_payload = set(iter_payload_paths(root))
    if actual_payload != set(files):
        missing = sorted(set(files) - actual_payload)
        extra = sorted(actual_payload - set(files))
        raise VerificationError(f"payload inventory mismatch; missing={missing}, extra={extra}")

    for relative, entry in files.items():
        path = root / relative
        kind = entry.get("type", "file")
        if kind == "symlink":
            verify_symlink(root, relative, entry)
            continue
        if path.is_symlink() or not path.is_file():
            raise VerificationError(f"expected regular file: {relative}")
        actual_hash = sha256_file(path)
        if actual_hash != entry["sha256"] or actual_hash != checksums[relative]:
            raise VerificationError(f"hash mismatch: {relative}")
        actual_mode = stat.S_IMODE(path.stat().st_mode)
        expected_mode = int(entry["mode"], 8)
        if actual_mode != expected_mode:
            raise VerificationError(
                f"mode mismatch: {relative} is {actual_mode:04o}, expected {expected_mode:04o}"
            )

    manifest_hash = sha256_file(manifest_path)
    if manifest_hash != checksums["manifest.json"]:
        raise VerificationError("manifest hash mismatch")

    try:
        tls_reserve_bytes = validate_libhybris_common(root / LIBHYBRIS_COMMON_PATH)
    except ElfContractError as exc:
        raise VerificationError(f"unsafe libhybris runtime: {exc}") from exc

    return {
        "ok": True,
        "component": manifest["component"],
        "version": manifest["version"],
        "target": manifest["target"],
        "profiles": manifest["profiles"],
        "filesVerified": len(files) + 1,
        "libhybrisTlsBytes": tls_reserve_bytes,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="extracted runtime component root")
    parser.add_argument("--json", action="store_true", help="always emit a JSON result")
    args = parser.parse_args()
    try:
        result = verify(args.root)
    except VerificationError as exc:
        if args.json:
            print(json.dumps({"ok": False, "error": str(exc)}, sort_keys=True))
        else:
            print(f"verification failed: {exc}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(
            f"verified {result['component']} {result['version']} "
            f"({result['filesVerified']} files)"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
