#!/usr/bin/env python3
import json
import os
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[3]
RUN_DIR = ROOT / "reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104"
OLD_RUN_DIR = ROOT / "reports/phase4-hollow-knight/laneA-post-scene-guest-loop-map-20260722-162042"
PREFIX = ROOT / "artifacts/_mr-run-aa-hk-post-scene-guest-loop-capture-hash-retraction-20260722-2104"
STAGED_DIST = ROOT / "reports/phase4-hollow-knight/laneA-post-scene-passive-stack-20260722-143030/staged-dist"
DXMT_ROOT = ROOT / "reports/phase4-hollow-knight/checkpoints/20260722-present-capability-02b4-VERIFIED_NOT_GOLDEN/payload/runtime/dxmt-builtin-overlay"
EXE = ROOT.parent / "game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe"
OLD_FINAL_CHILD = OLD_RUN_DIR / "final-child.json"


def load_child_environment():
    data = json.loads(OLD_FINAL_CHILD.read_text(encoding="utf-8"))
    env = {entry["name"]: str(entry.get("value", ""))
           for entry in data["environment"]["entries"]}
    old_run = str(OLD_RUN_DIR)
    new_run = str(RUN_DIR)
    old_prefix = str(ROOT / "artifacts/_mr-run-aa-hk-post-scene-guest-loop-map-20260722-162042")
    new_prefix = str(PREFIX)
    for name, value in list(env.items()):
        env[name] = value.replace(old_run, new_run).replace(old_prefix, new_prefix)
    env["MACRUNNER_RUN_DIR"] = new_run
    env["MACRUNNER_MR_RUN_PREFIX_PATH"] = new_prefix
    env["WINEPREFIX"] = new_prefix
    env["MACRUNNER_LANEA_WINE_DIST"] = str(STAGED_DIST)
    env["MACRUNNER_DXMT_ROOT"] = str(DXMT_ROOT)

    child_entries = env.get("WINEDLLPATH", "").split(":")
    dxmt = str(DXMT_ROOT)
    if child_entries and child_entries[0] == dxmt:
        env["WINEDLLPATH"] = ":".join(child_entries[1:])
    else:
        raise SystemExit("expected child WINEDLLPATH to start with DXMT root")

    forbidden = [
        "MACRUNNER_HB_TRACE_HELPER_LOOP_TOP",
        "MACRUNNER_HB_SHADER_VALUE_OVERRIDE",
        "MACRUNNER_HB_SHADER_VALUE_PRODUCTION",
    ]
    for name in forbidden:
        env.pop(name, None)
    return env


def main():
    if not OLD_FINAL_CHILD.exists():
        raise SystemExit("old final-child evidence missing")
    if PREFIX.exists():
        raise SystemExit("corrected prefix already exists")
    if not STAGED_DIST.is_dir():
        raise SystemExit("staged dist missing")
    if not EXE.exists():
        raise SystemExit("Hollow Knight exe missing")

    env = load_child_environment()
    argv = [
        str(ROOT / "scripts/mr-run.sh"),
        str(STAGED_DIST),
        str(EXE),
        "2400",
    ]
    os.execve(argv[0], argv, env)


if __name__ == "__main__":
    main()
