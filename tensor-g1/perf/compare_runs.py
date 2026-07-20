#!/usr/bin/env python3
"""Summarize or compare tensor-perf-v1 JSONL benchmark artifacts."""

from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable


def load(path: Path) -> list[dict[str, Any]]:
    records = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if line.startswith("TENSOR_PERF "):
            line = line.removeprefix("TENSOR_PERF ")
        if not line or not line.startswith("{"):
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(f"{path}:{number}: {error}") from error
        if record.get("schema") != "tensor-perf-v1":
            raise ValueError(f"{path}:{number}: unsupported schema")
        records.append(record)
    return records


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    return ordered[math.ceil(fraction * len(ordered)) - 1]


def one_process_run(records: Iterable[dict[str, Any]]) -> dict[str, float]:
    by_identity: dict[tuple[int, int], list[dict[str, Any]]] = defaultdict(list)
    sampler_cpu = 0
    sampler_elapsed = 0
    for record in records:
        if record.get("kind") == "process_sample":
            identity = (int(record["pid"]), int(record["start_ticks"]))
            by_identity[identity].append(record)
        elif record.get("kind") == "sampler_summary":
            sampler_cpu += int(record.get("sampler_cpu_ns", 0))
            sampler_elapsed += int(record.get("elapsed_ns", 0))

    cpu_seconds = 0.0
    wall_start = math.inf
    wall_end = 0
    rss_peak = 0
    read_bytes = 0
    write_bytes = 0
    voluntary = 0
    involuntary = 0
    for samples in by_identity.values():
        samples.sort(key=lambda item: item["timestamp_ns"])
        first, last = samples[0], samples[-1]
        ticks = (last["utime_ticks"] + last["stime_ticks"] -
                 first["utime_ticks"] - first["stime_ticks"])
        cpu_seconds += ticks / last["clock_ticks_per_second"]
        wall_start = min(wall_start, first["timestamp_ns"])
        wall_end = max(wall_end, last["timestamp_ns"])
        rss_peak = max(rss_peak, *(item["rss_bytes"] for item in samples))
        read_bytes += max(0, last["read_bytes"] - first["read_bytes"])
        write_bytes += max(0, last["write_bytes"] - first["write_bytes"])
        voluntary += max(0, last["voluntary_ctxt_switches"] -
                         first["voluntary_ctxt_switches"])
        involuntary += max(0, last["nonvoluntary_ctxt_switches"] -
                           first["nonvoluntary_ctxt_switches"])
    wall_seconds = ((wall_end - wall_start) / 1e9
                    if wall_end and wall_start != math.inf else 0.0)
    return {
        "process_cpu_cores": cpu_seconds / wall_seconds if wall_seconds else 0.0,
        "process_cpu_seconds": cpu_seconds,
        "rss_peak_mib": rss_peak / (1024 * 1024),
        "read_mib": read_bytes / (1024 * 1024),
        "write_mib": write_bytes / (1024 * 1024),
        "voluntary_context_switches": float(voluntary),
        "involuntary_context_switches": float(involuntary),
        "sampler_cpu_percent": (100 * sampler_cpu / sampler_elapsed
                                if sampler_elapsed else 0.0),
    }


def process_metrics(records: Iterable[dict[str, Any]]) -> dict[str, float]:
    runs: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        if record.get("kind") in {"process_sample", "sampler_summary"}:
            runs[str(record.get("run_id", "legacy"))].append(record)
    if not runs:
        return {}
    per_run = [one_process_run(run) for run in runs.values()]
    result: dict[str, float] = {}
    for key in per_run[0]:
        values = [run[key] for run in per_run]
        result[key] = percentile(values, 0.5)
        result[f"{key}_p95"] = percentile(values, 0.95)
    return result


def stage_metrics(records: Iterable[dict[str, Any]]) -> dict[str, float]:
    totals: dict[str, float] = defaultdict(float)
    counts: dict[str, float] = defaultdict(float)
    maxima: dict[str, float] = defaultdict(float)
    run_means: dict[str, list[float]] = defaultdict(list)
    for record in records:
        if record.get("kind") == "stage":
            stage = str(record["stage"])
            totals[stage] += float(record.get("total_ns", 0))
            counts[stage] += float(record.get("count", 0))
            maxima[stage] = max(maxima[stage], float(record.get("max_ns", 0)))
            count = float(record.get("count", 0))
            if count:
                run_means[stage].append(float(record.get("total_ns", 0)) / count)
    result: dict[str, float] = {}
    for stage in totals:
        result[f"stage.{stage}.total_ms"] = totals[stage] / 1e6
        result[f"stage.{stage}.mean_us"] = (
            totals[stage] / counts[stage] / 1e3 if counts[stage] else 0.0
        )
        result[f"stage.{stage}.max_us"] = maxima[stage] / 1e3
        result[f"stage.{stage}.run_mean_p50_us"] = (
            percentile(run_means[stage], 0.5) / 1e3
        )
        result[f"stage.{stage}.run_mean_p95_us"] = (
            percentile(run_means[stage], 0.95) / 1e3
        )
        result[f"stage.{stage}.count"] = counts[stage]
    return result


def benchmark_metrics(records: Iterable[dict[str, Any]]) -> dict[str, float]:
    benchmark_records = [
        record for record in records if record.get("kind") == "benchmark"
    ]
    if any(record.get("final") is True for record in benchmark_records):
        benchmark_records = [
            record for record in benchmark_records if record.get("final") is True
        ]
    values: dict[str, list[float]] = defaultdict(list)
    for record in benchmark_records:
        for key, value in record.items():
            if (key not in {"schema", "kind", "name", "run_id", "final"}
                    and isinstance(value, (int, float))
                    and not isinstance(value, bool)):
                values[key].append(float(value))
    result: dict[str, float] = {}
    for key, samples in values.items():
        result[f"bench.{key}.median"] = percentile(samples, 0.5)
        result[f"bench.{key}.p95"] = percentile(samples, 0.95)
    return result


def summarize(records: list[dict[str, Any]]) -> dict[str, float]:
    return {
        **process_metrics(records),
        **stage_metrics(records),
        **benchmark_metrics(records),
    }


def display_value(key: str, value: float) -> str:
    if key.endswith(".count") or "switches" in key:
        return f"{value:.0f}"
    return f"{value:.3f}"


def markdown(current: dict[str, float], baseline: dict[str, float] | None) -> str:
    lines = ["| Metric | Current | Baseline | Delta |", "| --- | ---: | ---: | ---: |"]
    for key in sorted(current):
        value = current[key]
        if baseline is None or key not in baseline:
            lines.append(f"| `{key}` | {display_value(key, value)} | - | - |")
            continue
        old = baseline[key]
        delta = (100 * (value - old) / old) if old else math.nan
        delta_text = f"{delta:+.2f}%" if math.isfinite(delta) else "n/a"
        lines.append(
            f"| `{key}` | {display_value(key, value)} | "
            f"{display_value(key, old)} | {delta_text} |"
        )
    return "\n".join(lines)


def self_test() -> int:
    records = [
        {"schema": "tensor-perf-v1", "kind": "process_sample", "pid": 1,
         "run_id": "test", "start_ticks": 7, "timestamp_ns": 0,
         "clock_ticks_per_second": 100,
         "utime_ticks": 2, "stime_ticks": 1, "rss_bytes": 1048576,
         "read_bytes": 10, "write_bytes": 20, "voluntary_ctxt_switches": 1,
         "nonvoluntary_ctxt_switches": 2},
        {"schema": "tensor-perf-v1", "kind": "process_sample", "pid": 1,
         "run_id": "test", "start_ticks": 7,
         "timestamp_ns": 1_000_000_000,
         "clock_ticks_per_second": 100, "utime_ticks": 52, "stime_ticks": 1,
         "rss_bytes": 2097152, "read_bytes": 30, "write_bytes": 60,
         "voluntary_ctxt_switches": 4, "nonvoluntary_ctxt_switches": 7},
        {"schema": "tensor-perf-v1", "kind": "stage", "stage": "copy",
         "count": 2, "total_ns": 2000, "max_ns": 1500},
        {"schema": "tensor-perf-v1", "kind": "benchmark", "name": "web",
         "final": False, "presented_fps": 1.0},
        {"schema": "tensor-perf-v1", "kind": "benchmark", "name": "web",
         "final": True, "presented_fps": 60.0},
    ]
    result = summarize(records)
    assert result["process_cpu_cores"] == 0.5
    assert result["rss_peak_mib"] == 2.0
    assert result["stage.copy.mean_us"] == 1.0
    assert result["stage.copy.max_us"] == 1.5
    assert result["stage.copy.run_mean_p50_us"] == 1.0
    assert result["bench.presented_fps.median"] == 60.0
    print("compare_runs self-test: pass")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("current", type=Path, nargs="?")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.current is None:
        parser.error("current JSONL artifact is required")
    current = summarize(load(args.current))
    baseline = summarize(load(args.baseline)) if args.baseline else None
    print(markdown(current, baseline))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
