#!/usr/bin/env python3
"""Turn Termux:X11 extra-key pulses into usable SuperTuxKart controls.

Termux:X11 emits a complete key-down/key-up pair for every extra-key repeat.
That is fine for terminals, but a game may never sample the key as held.  This
helper listens for otherwise-unused F-key pulses and holds the configured game
key through XTEST until the pulse stream stops.
"""

from __future__ import annotations

import os
import re
import select
import signal
import subprocess
import sys
import time


# Xorg's standard F1..F8 keycodes. Termux:X11 uses this standard keymap.
TRIGGERS = {
    67: ("toggle", "Up", "GAS"),
    68: ("momentary", "Left", "LEFT"),
    69: ("momentary", "Down", "BRAKE"),
    70: ("momentary", "Right", "RIGHT"),
    71: ("momentary", "space", "FIRE"),
    72: ("momentary", "n", "NITRO"),
    73: ("toggle", "v", "SKID"),
    74: ("momentary", "BackSpace", "RESCUE"),
}

MOMENTARY_HOLD_SECONDS = 0.35
TOGGLE_QUIET_SECONDS = 0.30

held: set[str] = set()
deadlines: dict[str, float] = {}
last_trigger_pulse: dict[int, float] = {}
running = True


def xdotool(action: str, key: str) -> None:
    subprocess.run(
        ["xdotool", action, key],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )


def set_held(key: str, value: bool) -> None:
    if value and key not in held:
        xdotool("keydown", key)
        held.add(key)
    elif not value and key in held:
        xdotool("keyup", key)
        held.remove(key)


def stop(_signum: int, _frame: object) -> None:
    global running
    running = False


def handle_trigger(keycode: int, now: float) -> None:
    mode, target, label = TRIGGERS[keycode]
    previous = last_trigger_pulse.get(keycode, 0.0)
    last_trigger_pulse[keycode] = now

    if mode == "toggle":
        # A long press repeats every ~80 ms. Treat one uninterrupted pulse
        # train as a single toggle, and a later tap as the next toggle.
        if now - previous >= TOGGLE_QUIET_SECONDS:
            set_held(target, target not in held)
            print(f"{label}: {'ON' if target in held else 'OFF'}", flush=True)
    else:
        set_held(target, True)
        deadlines[target] = now + MOMENTARY_HOLD_SECONDS


def release_expired(now: float) -> None:
    for key, deadline in list(deadlines.items()):
        if now >= deadline:
            set_held(key, False)
            deadlines.pop(key, None)


def xtest_keyboard_id() -> int | None:
    result = subprocess.run(
        ["xinput", "list", "--id-only", "Virtual core XTEST keyboard"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    try:
        return int(result.stdout.strip())
    except ValueError:
        return None


def main() -> int:
    global running
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)

    xtest_id = xtest_keyboard_id()
    proc = subprocess.Popen(
        ["stdbuf", "-oL", "xinput", "test-xi2", "--root"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=0,
    )
    if proc.stdout is None:
        return 1

    print("STK touch controls active (GAS and SKID toggle; others hold).", flush=True)
    fd = proc.stdout.fileno()
    buffered = b""
    raw_press = False
    source_id: int | None = None

    try:
        while running and proc.poll() is None:
            ready, _, _ = select.select([fd], [], [], 0.02)
            if ready:
                chunk = os.read(fd, 4096)
                if not chunk:
                    break
                buffered += chunk
                lines = buffered.split(b"\n")
                buffered = lines.pop()

                for raw_line in lines:
                    line = raw_line.decode("utf-8", "replace").strip()
                    if line.startswith("EVENT type"):
                        raw_press = "(RawKeyPress)" in line
                        source_id = None
                    elif raw_press and line.startswith("device:"):
                        match = re.search(r"\((\d+)\)", line)
                        source_id = int(match.group(1)) if match else None
                    elif raw_press and line.startswith("detail:"):
                        try:
                            keycode = int(line.split(":", 1)[1].strip())
                        except ValueError:
                            continue
                        if keycode in TRIGGERS and source_id != xtest_id:
                            handle_trigger(keycode, time.monotonic())

            release_expired(time.monotonic())
    finally:
        proc.terminate()
        for key in list(held):
            set_held(key, False)

    return 0


if __name__ == "__main__":
    sys.exit(main())
