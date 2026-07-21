#!/usr/bin/env python3
"""Low-overhead /proc sampler for Tensor G1 benchmark runs.

The sampler deliberately records raw counters as JSONL.  Rates are calculated
after the run so sampling never has to make policy decisions on the device.
It can follow exact PIDs, process-name regular expressions, or both.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import sys
import time
from pathlib import Path


def read_text(path: Path) -> str | None:
    try:
        return path.read_text(errors="replace")
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        return None


def parse_stat(text: str) -> dict[str, int | str]:
    # comm may contain spaces and parentheses, so split after the final ')'.
    close = text.rfind(")")
    if close < 0:
        raise ValueError("malformed /proc/PID/stat")
    pid, comm = text[:close + 1].split(" (", 1)
    fields = text[close + 2:].split()
    return {
        "pid": int(pid),
        "comm": comm[:-1],
        "state": fields[0],
        "ppid": int(fields[1]),
        "utime_ticks": int(fields[11]),
        "stime_ticks": int(fields[12]),
        "threads": int(fields[17]),
        "start_ticks": int(fields[19]),
        "rss_pages": int(fields[21]),
    }


def parse_key_values(text: str) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in text.splitlines():
        key, separator, value = line.partition(":")
        if not separator:
            continue
        token = value.strip().split(maxsplit=1)[0]
        try:
            result[key] = int(token)
        except ValueError:
            continue
    return result


def snapshot(pid: int, timestamp_ns: int, page_size: int, clock_ticks: int,
             run_id: str) -> dict[str, int | str] | None:
    root = Path("/proc") / str(pid)
    stat_text = read_text(root / "stat")
    if stat_text is None:
        return None
    try:
        stat = parse_stat(stat_text)
    except (ValueError, IndexError):
        return None

    status = parse_key_values(read_text(root / "status") or "")
    io = parse_key_values(read_text(root / "io") or "")
    cmdline = (read_text(root / "cmdline") or "").replace("\0", " ").strip()
    return {
        "schema": "tensor-perf-v1",
        "kind": "process_sample",
        "run_id": run_id,
        "timestamp_ns": timestamp_ns,
        "clock_ticks_per_second": clock_ticks,
        "page_size": page_size,
        **stat,
        "rss_bytes": int(stat["rss_pages"]) * page_size,
        "read_bytes": io.get("read_bytes", 0),
        "write_bytes": io.get("write_bytes", 0),
        "voluntary_ctxt_switches": status.get("voluntary_ctxt_switches", 0),
        "nonvoluntary_ctxt_switches": status.get(
            "nonvoluntary_ctxt_switches", 0
        ),
        "cmdline": cmdline,
    }


def matching_pids(pattern: re.Pattern[str] | None, fixed: set[int]) -> set[int]:
    result = set(fixed)
    if pattern is None:
        return result
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        if int(entry.name) == os.getpid():
            continue
        cmdline = (read_text(entry / "cmdline") or "").replace("\0", " ")
        haystack = cmdline
        if not haystack:
            haystack = read_text(entry / "comm") or ""
        if pattern.search(haystack):
            result.add(int(entry.name))
    return result


def self_test() -> int:
    sample = (
        "123 (RDD Process) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 "
        "17 18 19 20 21 22 23 24"
    )
    parsed = parse_stat(sample)
    assert parsed["pid"] == 123
    assert parsed["comm"] == "RDD Process"
    assert parsed["utime_ticks"] == 11
    assert parsed["stime_ticks"] == 12
    assert parsed["threads"] == 17
    assert parsed["start_ticks"] == 19
    assert parsed["rss_pages"] == 21
    values = parse_key_values("VmRSS: 1234 kB\nread_bytes: 99\nName: firefox\n")
    assert values == {"VmRSS": 1234, "read_bytes": 99}
    print("process_sampler self-test: pass")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=False, default="-")
    parser.add_argument("--interval-ms", type=int, default=250)
    parser.add_argument(
        "--rescan-interval-ms", type=int, default=5000,
        help="refresh --match PID discovery at this interval (default: 5000)",
    )
    parser.add_argument("--duration", type=float, default=15.0)
    parser.add_argument("--pid", type=int, action="append", default=[])
    parser.add_argument("--match", help="regular expression matched against comm/cmdline")
    parser.add_argument("--run-id", help="stable identifier shared by this repetition")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.interval_ms < 50:
        parser.error("--interval-ms must be at least 50 to bound observer cost")
    if args.rescan_interval_ms < args.interval_ms:
        parser.error("--rescan-interval-ms must be at least --interval-ms")
    if args.duration <= 0:
        parser.error("--duration must be positive")

    pattern = re.compile(args.match) if args.match else None
    fixed = set(args.pid)
    if not pattern and not fixed:
        parser.error("provide at least one --pid or --match")

    output = sys.stdout if args.output == "-" else open(
        args.output, "a", encoding="utf-8", buffering=1
    )
    clock_ticks = os.sysconf("SC_CLK_TCK")
    page_size = os.sysconf("SC_PAGE_SIZE")
    started_ns = time.monotonic_ns()
    run_id = args.run_id or f"sampler-{os.getpid()}-{started_ns}"
    deadline_ns = started_ns + int(args.duration * 1_000_000_000)
    sampler_cpu_start = time.process_time_ns()
    stopped = False
    dynamic_pids: set[int] = set()
    next_rescan_ns = 0

    def stop(_signum: int, _frame: object) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    samples = 0
    try:
        while not stopped and time.monotonic_ns() < deadline_ns:
            loop_started = time.monotonic_ns()
            if pattern is not None and loop_started >= next_rescan_ns:
                dynamic_pids = matching_pids(pattern, set())
                next_rescan_ns = (
                    loop_started + args.rescan_interval_ms * 1_000_000
                )
            for pid in sorted(fixed | dynamic_pids):
                record = snapshot(
                    pid, loop_started, page_size, clock_ticks, run_id
                )
                if record is not None:
                    output.write(json.dumps(record, separators=(",", ":")) + "\n")
                    samples += 1
            target = loop_started + args.interval_ms * 1_000_000
            remaining = target - time.monotonic_ns()
            if remaining > 0:
                time.sleep(remaining / 1_000_000_000)
    finally:
        ended_ns = time.monotonic_ns()
        summary = {
            "schema": "tensor-perf-v1",
            "kind": "sampler_summary",
            "run_id": run_id,
            "timestamp_ns": ended_ns,
            "elapsed_ns": ended_ns - started_ns,
            "sampler_cpu_ns": time.process_time_ns() - sampler_cpu_start,
            "samples": samples,
            "interval_ms": args.interval_ms,
            "rescan_interval_ms": args.rescan_interval_ms,
        }
        output.write(json.dumps(summary, separators=(",", ":")) + "\n")
        if output is not sys.stdout:
            output.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
