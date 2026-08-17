#!/usr/bin/env python3
"""Create a deterministic, verified uDroid graphics runtime archive."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import stat
import sys
import tarfile
import tempfile
from pathlib import Path
from typing import Any, Iterable


VERIFY_PATH = Path(__file__).with_name("verify-runtime.py")
VERIFY_SPEC = importlib.util.spec_from_file_location("verify_runtime_for_package", VERIFY_PATH)
assert VERIFY_SPEC is not None and VERIFY_SPEC.loader is not None
VERIFY_RUNTIME = importlib.util.module_from_spec(VERIFY_SPEC)
VERIFY_SPEC.loader.exec_module(VERIFY_RUNTIME)


class PackageError(RuntimeError):
    pass


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def archive_root_name(manifest: dict[str, Any]) -> str:
    target = manifest["target"]
    target_name = f"{target['os']}-{target['arch']}-{target['libc']}"
    return f"udroid-graphics-{manifest['version']}-{target_name}"


def runtime_entries(root: Path) -> Iterable[Path]:
    yield root
    entries: list[Path] = []
    for directory, dir_names, file_names in os.walk(root, followlinks=False):
        base = Path(directory)
        dir_names.sort()
        file_names.sort()
        for name in dir_names:
            entries.append(base / name)
        for name in file_names:
            entries.append(base / name)
    yield from sorted(entries, key=lambda path: path.relative_to(root).as_posix())


def normalized_info(path: Path, root: Path, archive_root: str, epoch: int) -> tarfile.TarInfo:
    relative = path.relative_to(root.parent)
    if path == root:
        archive_name = archive_root
    else:
        archive_name = f"{archive_root}/{path.relative_to(root).as_posix()}"
    metadata = path.lstat()
    info = tarfile.TarInfo(archive_name)
    info.mode = stat.S_IMODE(metadata.st_mode)
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = epoch
    if path.is_symlink():
        info.type = tarfile.SYMTYPE
        info.linkname = os.readlink(path)
    elif path.is_dir():
        info.type = tarfile.DIRTYPE
    elif path.is_file():
        info.type = tarfile.REGTYPE
        info.size = metadata.st_size
    else:
        raise PackageError(f"unsupported runtime entry: {relative}")
    return info


def package(root: Path, output: Path, epoch: int) -> dict[str, Any]:
    root = root.resolve()
    output = output.resolve()
    if output.suffixes[-2:] != [".tar", ".xz"]:
        raise PackageError("runtime archive must end in .tar.xz")
    try:
        output.relative_to(root)
    except ValueError:
        pass
    else:
        raise PackageError("runtime archive cannot be written inside the staging root")
    try:
        verification = VERIFY_RUNTIME.verify(root)
    except VERIFY_RUNTIME.VerificationError as exc:
        raise PackageError(f"runtime verification failed: {exc}") from exc

    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected_root = archive_root_name(manifest)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{output.name}.",
            suffix=".tmp",
            dir=output.parent,
        )
        os.close(descriptor)
        temporary = Path(temporary_name)
        with tarfile.open(temporary, mode="w:xz", format=tarfile.PAX_FORMAT) as archive:
            for path in runtime_entries(root):
                info = normalized_info(path, root, expected_root, epoch)
                if info.isreg():
                    with path.open("rb") as handle:
                        archive.addfile(info, handle)
                else:
                    archive.addfile(info)
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)

    digest = sha256_file(output)
    checksum_path = output.with_name(f"{output.name}.sha256")
    checksum_path.write_text(f"{digest}  {output.name}\n", encoding="utf-8")
    return {
        "schema": 1,
        "component": "udroid-graphics",
        "archive": output.name,
        "format": "tar.xz",
        "archiveRoot": expected_root,
        "sha256": digest,
        "size": output.stat().st_size,
        "version": verification["version"],
        "target": verification["target"],
        "profiles": verification["profiles"],
        "manifestSha256": sha256_file(manifest_path),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="finalized and verified runtime root")
    parser.add_argument("output", type=Path, help="output .tar.xz archive")
    parser.add_argument(
        "--source-date-epoch",
        type=int,
        default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")),
        help="normalized archive modification time",
    )
    parser.add_argument("--json", action="store_true", help="emit package metadata as JSON")
    parser.add_argument(
        "--metadata-output",
        type=Path,
        help="write package metadata for the release-index builder",
    )
    args = parser.parse_args()
    if args.source_date_epoch < 0:
        parser.error("--source-date-epoch must be non-negative")
    try:
        result = package(args.root, args.output, args.source_date_epoch)
    except (PackageError, OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"component packaging failed: {exc}", file=sys.stderr)
        return 1
    if args.metadata_output is not None:
        args.metadata_output.parent.mkdir(parents=True, exist_ok=True)
        args.metadata_output.write_text(
            json.dumps(result, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(
            f"packaged {result['archive']} ({result['size']} bytes, "
            f"sha256 {result['sha256']})"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
