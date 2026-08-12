#!/usr/bin/env python3
"""Render bounded AHardwareBuffer probe JSONL as timing graphs."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


STAGES = (
    ("allocate_us", "Allocate"),
    ("write_us", "Write + fence"),
    ("send_us", "Send"),
    ("receive_us", "Receive"),
    ("read_us", "Read + verify"),
)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def load_rows(path: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    with path.open(encoding="utf-8") as stream:
        for line_number, raw_line in enumerate(stream, start=1):
            line = raw_line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as error:
                raise SystemExit(f"{path}:{line_number}: invalid JSON: {error}") from error
            if not isinstance(row, dict):
                raise SystemExit(f"{path}:{line_number}: expected a JSON object")
            rows.append(row)
    if not rows:
        raise SystemExit(f"{path}: no probe rows")
    return rows


def summarize(rows: list[dict[str, object]]) -> tuple[list[dict[str, object]], int]:
    grouped: dict[int, list[dict[str, object]]] = defaultdict(list)
    failures = 0
    for row in rows:
        result = int(row.get("result", -1))
        if result != 0:
            failures += 1
            continue
        grouped[int(row["width"])].append(row)
    if not grouped:
        raise SystemExit("no successful probe rows to graph")

    summary: list[dict[str, object]] = []
    for width in sorted(grouped):
        samples = grouped[width]
        totals = [float(sample["total_us"]) for sample in samples]
        stage_medians = {
            key: statistics.median(float(sample[key]) for sample in samples)
            for key, _ in STAGES
        }
        summary.append(
            {
                "width": width,
                "height": int(samples[0]["height"]),
                "stride": int(samples[0]["stride"]),
                "samples": len(samples),
                "total_median_us": statistics.median(totals),
                "total_p95_us": percentile(totals, 0.95),
                "stages_median_us": stage_medians,
            }
        )
    return summary, failures


def render(summary: list[dict[str, object]], failures: int, output: Path, title: str) -> None:
    labels = [str(row["width"]) for row in summary]
    positions = list(range(len(summary)))
    medians = [float(row["total_median_us"]) for row in summary]
    p95 = [float(row["total_p95_us"]) for row in summary]

    figure, (total_axis, stage_axis) = plt.subplots(
        2,
        1,
        figsize=(12, 8),
        constrained_layout=True,
        gridspec_kw={"height_ratios": (1.15, 1)},
    )
    figure.suptitle(f"{title}\n{sum(int(row['samples']) for row in summary)} passed · {failures} failed")

    total_axis.plot(positions, medians, marker="o", linewidth=2, label="Median")
    total_axis.plot(positions, p95, marker="o", linewidth=2, label="P95")
    total_axis.set_ylabel("End-to-end latency (µs)")
    total_axis.set_xticks(positions, labels)
    total_axis.set_xlabel("Visible width (pixels)")
    total_axis.grid(axis="y", alpha=0.25)
    total_axis.legend()

    bottoms = [0.0] * len(summary)
    colors = ("#2f855a", "#38a169", "#68d391", "#3182ce", "#805ad5")
    for (key, label), color in zip(STAGES, colors):
        values = [float(row["stages_median_us"][key]) for row in summary]  # type: ignore[index]
        stage_axis.bar(positions, values, bottom=bottoms, label=label, color=color)
        bottoms = [bottom + value for bottom, value in zip(bottoms, values)]
    stage_axis.set_ylabel("Median measured stage time (µs)")
    stage_axis.set_xticks(positions, labels)
    stage_axis.set_xlabel("Visible width (pixels)")
    stage_axis.grid(axis="y", alpha=0.2)
    stage_axis.legend(ncols=3, fontsize="small")

    output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output, dpi=160)
    plt.close(figure)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path, help="AHB probe JSONL")
    parser.add_argument("--output", type=Path, required=True, help="output PNG")
    parser.add_argument("--summary-json", type=Path, help="optional aggregate JSON")
    parser.add_argument("--title", default="AHardwareBuffer lifecycle probe")
    arguments = parser.parse_args()

    summary, failures = summarize(load_rows(arguments.results))
    render(summary, failures, arguments.output, arguments.title)
    if arguments.summary_json is not None:
        arguments.summary_json.parent.mkdir(parents=True, exist_ok=True)
        arguments.summary_json.write_text(
            json.dumps({"failures": failures, "widths": summary}, indent=2) + "\n",
            encoding="utf-8",
        )


if __name__ == "__main__":
    main()
