#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Compare stable descriptor and shader fields in two pandecode text dumps."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


ADDRESS_FIELD = re.compile(
    r"^(Address|Next|Binary|Resources|Shader|Thread storage|FAU|Fault Pointer|"
    r"TLS Base Pointer|WLS Base Pointer):"
)
OBJECT_ADDRESS = re.compile(r"(?:@[0-9a-f]+|Job Header \([0-9a-f]+\))")
INSTRUCTION = re.compile(r"^(?:[0-9a-f]{2} ){7}[0-9a-f]{2}\s{2,}.*$")
FAU_VALUE = re.compile(r"^u\d+\s+[0-9A-F]{8}\s+[0-9A-F]{8}$")


def extract(path: Path) -> tuple[list[str], list[str]]:
    semantic: list[str] = []
    shader: list[str] = []

    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        if INSTRUCTION.match(line):
            shader.append(line)
            continue
        if ADDRESS_FIELD.match(line) or OBJECT_ADDRESS.search(line) or FAU_VALUE.match(line):
            continue
        if line.startswith(("Exception Status:", "First Incomplete Task:")):
            # Hardware updates completion fields; the no-op shim cannot.
            continue
        if line.startswith("Shader 0x"):
            continue
        semantic.append(line)

    return semantic, shader


def digest(lines: list[str]) -> str:
    return hashlib.sha256(("\n".join(lines) + "\n").encode()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()

    reference_semantic, reference_shader = extract(args.reference)
    candidate_semantic, candidate_shader = extract(args.candidate)
    result = {
        "reference": str(args.reference),
        "candidate": str(args.candidate),
        "descriptor_semantics_equal": reference_semantic == candidate_semantic,
        "shader_text_equal": reference_shader == candidate_shader,
        "reference_descriptor_sha256": digest(reference_semantic),
        "candidate_descriptor_sha256": digest(candidate_semantic),
        "reference_shader_sha256": digest(reference_shader),
        "candidate_shader_sha256": digest(candidate_shader),
        "reference_semantic_lines": len(reference_semantic),
        "candidate_semantic_lines": len(candidate_semantic),
        "reference_shader_instructions": len(reference_shader),
        "candidate_shader_instructions": len(candidate_shader),
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if result["descriptor_semantics_equal"] and result["shader_text_equal"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
