#!/usr/bin/env python3
import argparse
import ctypes
import ctypes.util
import json
import os
import subprocess
import sys
import time
from pathlib import Path


KEY_CODES = {"return": 36, "right": 124}
OBSERVER_PREFIX = "macrunner-hb-language-flow:"


def emit(path: Path, event: str, **fields) -> None:
    payload = {
        "schema": "macrunner.hk-language-input/v1",
        "event": event,
        "epoch": round(time.time(), 6),
        "monotonic": round(time.monotonic(), 6),
        **fields,
    }
    with path.open("a", encoding="utf-8") as output:
        output.write(json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n")
        output.flush()
        os.fsync(output.fileno())


def observer_lines(run_log: Path) -> list[str]:
    try:
        return [
            line
            for line in run_log.read_text(encoding="utf-8", errors="replace").splitlines()
            if OBSERVER_PREFIX in line
        ]
    except FileNotFoundError:
        return []


def wait_for(run_log: Path, description: str, predicate, timeout: float) -> list[str]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        lines = observer_lines(run_log)
        if predicate(lines):
            return lines
        time.sleep(0.25)
    raise TimeoutError(f"timed out waiting for {description}")


def exact_hk_window(probe: Path) -> dict | None:
    try:
        completed = subprocess.run(
            [str(probe)], check=True, capture_output=True, text=True, timeout=10
        )
        payload = json.loads(completed.stdout)
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        return None
    matches = []
    for window in payload.get("windows", []):
        if (
            window.get("owner") == "wine"
            and window.get("name") == "Hollow Knight"
            and int(window.get("layer", -1)) == 0
            and float(window.get("alpha", 0)) > 0
            and int(window.get("width", 0)) > 0
            and int(window.get("height", 0)) > 0
        ):
            matches.append(window)
    matches.sort(
        key=lambda window: (
            int(window.get("width", 0)) * int(window.get("height", 0)),
            int(window.get("window_id", 0)),
        ),
        reverse=True,
    )
    return matches[0] if matches else None


def wait_for_window(probe: Path, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if window := exact_hk_window(probe):
            return window
        time.sleep(0.5)
    raise TimeoutError("timed out waiting for exact Hollow Knight window")


class TargetedKeyboard:
    def __init__(self) -> None:
        core_graphics_path = ctypes.util.find_library("CoreGraphics")
        core_foundation_path = ctypes.util.find_library("CoreFoundation")
        if not core_graphics_path or not core_foundation_path:
            raise RuntimeError("CoreGraphics/CoreFoundation unavailable")
        self.cg = ctypes.CDLL(core_graphics_path)
        self.cf = ctypes.CDLL(core_foundation_path)
        self.cg.CGEventCreateKeyboardEvent.argtypes = [ctypes.c_void_p, ctypes.c_ushort, ctypes.c_bool]
        self.cg.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
        self.cg.CGEventPostToPid.argtypes = [ctypes.c_int, ctypes.c_void_p]
        self.cg.CGEventPostToPid.restype = None
        self.cf.CFRelease.argtypes = [ctypes.c_void_p]
        self.cf.CFRelease.restype = None

    def post_key(self, pid: int, key: str, output: Path, ordinal: int) -> None:
        key_code = KEY_CODES[key]
        for phase, down in (("down", True), ("up", False)):
            event = self.cg.CGEventCreateKeyboardEvent(None, key_code, down)
            if not event:
                raise RuntimeError(f"CGEventCreateKeyboardEvent failed: {key}/{phase}")
            self.cg.CGEventPostToPid(pid, event)
            self.cf.CFRelease(event)
            emit(
                output,
                "cg-event-posted",
                ordinal=ordinal,
                logical_key=key,
                key_code=key_code,
                phase=phase,
                target_pid=pid,
            )
            time.sleep(0.08)


def count_matching(lines: list[str], *needles: str) -> int:
    return sum(all(needle in line for needle in needles) for line in lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--run-log", type=Path, required=True)
    parser.add_argument("--window-probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ready-timeout", type=float, default=1200)
    parser.add_argument("--transition-timeout", type=float, default=120)
    args = parser.parse_args()

    plan = json.loads(args.plan.read_text(encoding="utf-8"))
    events = plan.get("events")
    if plan.get("schema") != "macrunner.hk-language-input-plan/v1" or not isinstance(events, list):
        raise ValueError("invalid sealed input plan")
    expected = [] if plan.get("mode") == "A" else ["return", "right", "return"]
    if events != expected:
        raise ValueError(f"plan event drift: expected {expected}, got {events}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.unlink(missing_ok=True)
    emit(args.output, "controller-start", mode=plan["mode"], events=events)

    lines = wait_for(
        args.run_log,
        "observer init",
        lambda current: count_matching(current, "event=observer-init", "status=armed") == 1,
        args.ready_timeout,
    )
    lines = wait_for(
        args.run_log,
        "initial EnglishButton preselection",
        lambda current: count_matching(
            current, "event=unity-selection", "method=HighlightDefault", "phase=enter"
        )
        >= 1,
        args.ready_timeout,
    )
    window = wait_for_window(args.window_probe, 60)
    window_proof = {
        key: window.get(key)
        for key in ("pid", "window_id", "owner", "name", "x", "y", "width", "height", "layer", "alpha")
    }
    emit(args.output, "target-window-proven", **window_proof)

    if not events:
        emit(args.output, "no-input-control-complete", observer_records=len(lines), **window_proof)
        return 0

    keyboard = TargetedKeyboard()
    target_pid = int(window["pid"])

    keyboard.post_key(target_pid, "return", args.output, 1)
    wait_for(
        args.run_log,
        "SetLanguage acceptance",
        lambda current: count_matching(
            current, "event=unity-ui-callback", "method=SetLanguage", "phase=enter"
        )
        == 1,
        args.transition_timeout,
    )
    wait_for(
        args.run_log,
        "LanguageConfirm CancelButton preselection",
        lambda current: count_matching(
            current, "event=unity-selection", "method=HighlightDefault", "phase=enter"
        )
        >= 2,
        args.transition_timeout,
    )
    emit(args.output, "event-accepted", ordinal=1, proof="SetLanguage+second-HighlightDefault")

    keyboard.post_key(target_pid, "right", args.output, 2)
    emit(args.output, "event-pending", ordinal=2, proof="requires-subsequent-ConfirmLanguage")
    time.sleep(0.35)

    keyboard.post_key(target_pid, "return", args.output, 3)
    lines = wait_for(
        args.run_log,
        "single ConfirmLanguage entry",
        lambda current: count_matching(
            current, "event=confirm-language", "method=ConfirmLanguage", "phase=enter", "call=1"
        )
        == 1,
        args.transition_timeout,
    )
    lines = wait_for(
        args.run_log,
        "ConfirmLanguage return with confirmedLanguage=true",
        lambda current: count_matching(
            current,
            "event=confirm-language",
            "method=ConfirmLanguage",
            "phase=return",
            "call=1",
            "confirmedLanguage=1",
        )
        == 1,
        args.transition_timeout,
    )
    emit(args.output, "event-accepted", ordinal=2, proof="ConfirmLanguage-after-horizontal")
    emit(args.output, "event-accepted", ordinal=3, proof="ConfirmLanguage-call-1")
    emit(args.output, "sequence-complete", logical_events=3, cg_events=6, observer_records=len(lines))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        output = None
        try:
            index = sys.argv.index("--output")
            output = Path(sys.argv[index + 1])
            output.parent.mkdir(parents=True, exist_ok=True)
            emit(output, "controller-error", error=type(error).__name__, message=str(error))
        except Exception:
            pass
        print(f"hk_language_input: {type(error).__name__}: {error}", file=sys.stderr)
        raise SystemExit(1)
