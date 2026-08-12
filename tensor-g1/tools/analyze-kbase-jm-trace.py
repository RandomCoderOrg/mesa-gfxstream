#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Summarize timestamped JM submit/event pairs from PAN_KBASE_VERBOSE logs."""

import argparse
import json
import re


LINE = re.compile(
    r"(?P<time>\d+\.\d+)\tJM (?P<kind>submit|event) "
    r"atom=(?P<atom>\d+) seq=(?P<seq>\d+)"
)


def percentile(values, fraction):
    if not values:
        return None
    return values[round((len(values) - 1) * fraction)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("trace")
    args = parser.parse_args()

    submitted = {}
    completed = {}
    with open(args.trace, "r", encoding="utf-8", errors="replace") as trace:
        for line in trace:
            match = LINE.search(line)
            if not match:
                continue
            target = submitted if match["kind"] == "submit" else completed
            target[int(match["seq"])] = float(match["time"])

    paired = sorted(set(submitted) & set(completed))
    latencies = sorted((completed[seq] - submitted[seq]) * 1000 for seq in paired)
    missing = sorted(set(submitted) - set(completed))

    summary = {
        "submitted": len(submitted),
        "completed": len(completed),
        "paired": len(paired),
        "missing_sequences": missing,
        "latency_ms": {
            "min": min(latencies) if latencies else None,
            "p50": percentile(latencies, 0.50),
            "p90": percentile(latencies, 0.90),
            "p95": percentile(latencies, 0.95),
            "p99": percentile(latencies, 0.99),
            "max": max(latencies) if latencies else None,
        },
    }
    print("TENSOR_G1_JM=" + json.dumps(summary, sort_keys=True))

    limits = [1, 5, 10, 50, 100, 250, 500, 1000, 2000, 5000]
    counts = []
    lower = 0
    for limit in limits:
        count = sum(lower < value <= limit for value in latencies)
        counts.append((f"{lower:>4}-{limit:<4} ms", count))
        lower = limit
    counts.append((f">{limits[-1]:>4} ms", sum(v > limits[-1] for v in latencies)))

    scale = max((count for _, count in counts), default=1)
    for label, count in counts:
        width = round(40 * count / scale) if scale else 0
        print(f"{label} | {'#' * width:<40} {count}")


if __name__ == "__main__":
    main()
