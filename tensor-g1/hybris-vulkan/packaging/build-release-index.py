#!/usr/bin/env python3
"""Build the explicit release index consumed by uDroid and udroid-gpu."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path
from typing import Any
from urllib.parse import quote, urlparse


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
VERSION_RE = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+-]{0,63}$")
TARGET_KEYS = ("os", "arch", "libc")


class IndexError(RuntimeError):
    pass


def load_metadata(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise IndexError(f"cannot read package metadata {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise IndexError(f"package metadata must be an object: {path}")
    if value.get("schema") != 1 or value.get("component") != "udroid-graphics":
        raise IndexError(f"unexpected package metadata identity: {path}")
    return value


def target_id(target: dict[str, Any]) -> str:
    values = [target.get(key) for key in TARGET_KEYS]
    if not all(isinstance(value, str) and value for value in values):
        raise IndexError("package metadata has an invalid target")
    return "-".join(values)


def release_from_metadata(metadata: dict[str, Any], base_url: str) -> dict[str, Any]:
    version = metadata.get("version")
    if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
        raise IndexError("package metadata has an invalid version")
    target = metadata.get("target")
    if not isinstance(target, dict):
        raise IndexError("package metadata target must be an object")
    identifier = target_id(target)
    archive = metadata.get("archive")
    expected_archive = f"udroid-graphics-{version}-{identifier}.tar.xz"
    if archive != expected_archive:
        raise IndexError(f"unexpected archive name: {archive!r}; expected {expected_archive!r}")
    if metadata.get("format") != "tar.xz":
        raise IndexError("unsupported package archive format")
    digest = metadata.get("sha256")
    manifest_digest = metadata.get("manifestSha256")
    if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
        raise IndexError("package metadata has an invalid archive checksum")
    if not isinstance(manifest_digest, str) or not SHA256_RE.fullmatch(manifest_digest):
        raise IndexError("package metadata has an invalid manifest checksum")
    size = metadata.get("size")
    if not isinstance(size, int) or size <= 0:
        raise IndexError("package metadata has an invalid archive size")
    profiles = metadata.get("profiles")
    if not isinstance(profiles, list) or not profiles or not all(
        isinstance(profile, str) and profile for profile in profiles
    ):
        raise IndexError("package metadata has invalid profiles")
    return {
        "version": version,
        "target": target,
        "profiles": profiles,
        "manifestSha256": manifest_digest,
        "asset": {
            "name": archive,
            "url": f"{base_url}/{quote(archive)}",
            "sha256": digest,
            "size": size,
            "format": "tar.xz",
        },
    }


def build_index(
    metadata_paths: list[Path],
    base_url: str,
    recommended_version: str,
    epoch: int,
) -> dict[str, Any]:
    parsed_url = urlparse(base_url)
    if parsed_url.scheme != "https" or not parsed_url.netloc or parsed_url.query or parsed_url.fragment:
        raise IndexError("release asset base URL must be an HTTPS URL without query or fragment")
    normalized_base = base_url.rstrip("/")
    releases = [
        release_from_metadata(load_metadata(path), normalized_base)
        for path in metadata_paths
    ]
    identities = [(release["version"], target_id(release["target"])) for release in releases]
    if len(identities) != len(set(identities)):
        raise IndexError("release index contains a duplicate version and target")
    matching = [release for release in releases if release["version"] == recommended_version]
    if not matching:
        raise IndexError("recommended version is not present in the release index")
    recommended: dict[str, str] = {}
    for release in matching:
        identifier = target_id(release["target"])
        if identifier in recommended:
            raise IndexError("recommended version has duplicate target assets")
        recommended[identifier] = recommended_version
    generated = dt.datetime.fromtimestamp(epoch, tz=dt.timezone.utc)
    return {
        "schema": 1,
        "component": "udroid-graphics",
        "channel": "experimental",
        "generatedAt": generated.isoformat().replace("+00:00", "Z"),
        "recommended": recommended,
        "releases": sorted(
            releases,
            key=lambda release: (release["version"], target_id(release["target"])),
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata", nargs="+", type=Path, help="package metadata JSON files")
    parser.add_argument("--base-url", required=True, help="HTTPS directory containing the assets")
    parser.add_argument("--recommended", required=True, help="recommended component version")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--source-date-epoch",
        type=int,
        default=0,
        help="release index generation time",
    )
    args = parser.parse_args()
    if args.source_date_epoch < 0:
        parser.error("--source-date-epoch must be non-negative")
    try:
        index = build_index(
            args.metadata,
            args.base_url,
            args.recommended,
            args.source_date_epoch,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(index, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except (IndexError, OSError) as exc:
        print(f"release index build failed: {exc}", file=sys.stderr)
        return 1
    print(f"built {args.output} ({len(index['releases'])} release assets)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
