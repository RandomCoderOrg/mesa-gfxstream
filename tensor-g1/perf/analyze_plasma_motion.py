#!/usr/bin/env python3
"""Summarize Plasma window-motion runs and render a dependency-free SVG."""

from __future__ import annotations

import argparse
import html
import json
import re
import statistics
from pathlib import Path


# SurfaceFlinger assigns a new hexadecimal layer id whenever Termux:X11 is
# recreated. Match the stable application/window portion instead.
LAYER_MARKER = "SurfaceView[com.termux.x11/com.termux.x11.MainActivity]"


def surface_fps(path: Path) -> float:
    text = path.read_text(errors="replace")
    marker = text.find(LAYER_MARKER)
    if marker < 0:
        raise ValueError(f"Termux:X11 layer missing from {path}")
    match = re.search(r"averageFPS = ([0-9.]+)", text[marker:])
    if not match:
        raise ValueError(f"averageFPS missing from {path}")
    return float(match.group(1))


def thermal_status(path: Path) -> int:
    match = re.search(r"Thermal Status: (\d+)", path.read_text(errors="replace"))
    if not match:
        raise ValueError(f"thermal status missing from {path}")
    return int(match.group(1))


def process_cpu(path: Path) -> tuple[float, float]:
    by_run: dict[str, list[dict[str, object]]] = {}
    for line in path.read_text().splitlines():
        record = json.loads(line)
        by_run.setdefault(str(record["run_id"]), []).append(record)

    run_cores = []
    observer_percent = []
    for records in by_run.values():
        samples: dict[tuple[int, int], list[dict[str, object]]] = {}
        sampler_cpu = 0
        sampler_elapsed = 0
        for record in records:
            if record["kind"] == "process_sample":
                key = (int(record["pid"]), int(record["start_ticks"]))
                samples.setdefault(key, []).append(record)
            elif record["kind"] == "sampler_summary":
                sampler_cpu += int(record["sampler_cpu_ns"])
                sampler_elapsed += int(record["elapsed_ns"])

        cpu_seconds = 0.0
        first_timestamp = None
        last_timestamp = None
        for process in samples.values():
            process.sort(key=lambda item: int(item["timestamp_ns"]))
            first, last = process[0], process[-1]
            ticks = (int(last["utime_ticks"]) + int(last["stime_ticks"]) -
                     int(first["utime_ticks"]) - int(first["stime_ticks"]))
            cpu_seconds += ticks / int(last["clock_ticks_per_second"])
            first_timestamp = (int(first["timestamp_ns"]) if first_timestamp is None
                               else min(first_timestamp, int(first["timestamp_ns"])))
            last_timestamp = (int(last["timestamp_ns"]) if last_timestamp is None
                              else max(last_timestamp, int(last["timestamp_ns"])))
        wall_seconds = ((last_timestamp - first_timestamp) / 1e9
                        if first_timestamp is not None and last_timestamp else 0.0)
        run_cores.append(cpu_seconds / wall_seconds if wall_seconds else 0.0)
        observer_percent.append(100 * sampler_cpu / sampler_elapsed)
    return statistics.median(run_cores), statistics.median(observer_percent)


def summarize(root: Path) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    pattern = re.compile(r"(.+)-(\d+)-surfaceflinger\.txt$")
    grouped: dict[str, list[Path]] = {}
    for path in root.glob("*-surfaceflinger.txt"):
        match = pattern.fullmatch(path.name)
        if match:
            grouped.setdefault(match.group(1), []).append(path)

    for name, paths in grouped.items():
        values = [surface_fps(path) for path in sorted(paths)]
        statuses = [thermal_status(root / path.name.replace(
            "surfaceflinger.txt", "thermal.txt")) for path in sorted(paths)]
        cpu_path = root / f"{name}-process-all.jsonl"
        cpu_cores, observer = process_cpu(cpu_path) if cpu_path.exists() else (0.0, 0.0)
        result[name] = {
            "displayed_fps": values,
            "displayed_fps_median": statistics.median(values),
            "displayed_fps_min": min(values),
            "displayed_fps_max": max(values),
            "thermal_statuses": statuses,
            "process_cpu_cores_median": cpu_cores,
            "sampler_cpu_percent_median": observer,
        }
    return result


def write_markdown(root: Path, results: dict[str, dict[str, object]]) -> None:
    labels = {
        "stable": "CPU presenter + sync",
        "cpu-batchsync": "CPU presenter + batchsync",
        "no-compositor": "No KWin compositor",
    }
    lines = [
        "# Plasma window-motion comparison",
        "",
        "| Configuration | Displayed FPS median | Range | CPU cores | Thermal |",
        "| --- | ---: | ---: | ---: | --- |",
    ]
    for name in ("stable", "cpu-batchsync", "no-compositor"):
        if name not in results:
            continue
        item = results[name]
        thermal = ", ".join(str(value) for value in item["thermal_statuses"])
        lines.append(
            f"| {labels[name]} | {item['displayed_fps_median']:.2f} | "
            f"{item['displayed_fps_min']:.2f}-{item['displayed_fps_max']:.2f} | "
            f"{item['process_cpu_cores_median']:.3f} | {thermal} |"
        )
    lines.extend([
        "| DMA-BUF/DRI3 + sync | **FAIL** | KWin SIGBUS | - | - |",
        "| DMA-BUF/DRI3 + batchsync | **FAIL** | KWin SIGBUS | - | - |",
        "",
        "Displayed FPS comes from Android SurfaceFlinger TimeStats for the "
        "Termux:X11 SurfaceView. Each working configuration has one warm-up and "
        "five measured 120-move runs at a 60 Hz request rate.",
    ])
    (root / "comparison.md").write_text("\n".join(lines) + "\n")


def write_svg(root: Path, results: dict[str, dict[str, object]]) -> None:
    order = ["stable", "cpu-batchsync", "no-compositor"]
    labels = ["Stable sync", "CPU batchsync", "No compositor"]
    colors = ["#4472c4", "#70ad47", "#ed7d31"]
    width, height = 920, 500
    left, top, chart_height = 95, 55, 330
    baseline_y = top + chart_height
    scale = chart_height / 65.0
    bars = []
    for index, name in enumerate(order):
        item = results[name]
        median = float(item["displayed_fps_median"])
        low = float(item["displayed_fps_min"])
        high = float(item["displayed_fps_max"])
        x = left + 95 + index * 245
        y = baseline_y - median * scale
        error_top = baseline_y - high * scale
        error_bottom = baseline_y - low * scale
        bars.extend([
            f'<rect x="{x}" y="{y:.1f}" width="115" height="{median * scale:.1f}" '
            f'fill="{colors[index]}" rx="4"/>',
            f'<line x1="{x + 57}" y1="{error_top:.1f}" x2="{x + 57}" '
            f'y2="{error_bottom:.1f}" stroke="#20242b" stroke-width="3"/>',
            f'<line x1="{x + 42}" y1="{error_top:.1f}" x2="{x + 72}" '
            f'y2="{error_top:.1f}" stroke="#20242b" stroke-width="3"/>',
            f'<line x1="{x + 42}" y1="{error_bottom:.1f}" x2="{x + 72}" '
            f'y2="{error_bottom:.1f}" stroke="#20242b" stroke-width="3"/>',
            f'<text x="{x + 57}" y="{y - 12:.1f}" text-anchor="middle" '
            f'font-size="22" font-weight="700">{median:.2f}</text>',
            f'<text x="{x + 57}" y="{baseline_y + 34}" text-anchor="middle" '
            f'font-size="18">{html.escape(labels[index])}</text>',
        ])
    grid = []
    for value in range(0, 66, 10):
        y = baseline_y - value * scale
        grid.append(f'<line x1="{left}" y1="{y:.1f}" x2="870" y2="{y:.1f}" '
                    'stroke="#d8dde5" stroke-width="1"/>')
        grid.append(f'<text x="78" y="{y + 6:.1f}" text-anchor="end" '
                    f'font-size="16">{value}</text>')
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
<rect width="100%" height="100%" fill="#f7f8fa"/>
<text x="460" y="32" text-anchor="middle" font-family="sans-serif" font-size="24" font-weight="700">Plasma window movement delivered to Android</text>
<g font-family="sans-serif" fill="#20242b">{''.join(grid)}{''.join(bars)}
<text x="25" y="230" transform="rotate(-90 25 230)" text-anchor="middle" font-size="18">Displayed FPS</text>
<text x="460" y="466" text-anchor="middle" font-size="16">Median of five runs; whiskers show min-max. DRI3 candidates crashed KWin with SIGBUS.</text>
</g></svg>'''
    (root / "window-motion-fps.svg").write_text(svg)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    args = parser.parse_args()
    results = summarize(args.results)
    (args.results / "summary.json").write_text(json.dumps(
        results, indent=2, sort_keys=True) + "\n")
    write_markdown(args.results, results)
    write_svg(args.results, results)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
