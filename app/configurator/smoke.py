#!/usr/bin/env python3
"""Real smoke matrix runner for MacRunner."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from app.configurator.ai.error_parser import analyze_smoke_stderr
from app.configurator.inventory import build_inventory
from app.configurator.reports import dump_json, write_json


PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MATRIX = PROJECT_ROOT / "config/smoke-matrix.json"
REPORT_ROOT = PROJECT_ROOT / "reports"


def _load_matrix(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _engine_usable(lane: str) -> bool:
    return bool(build_inventory()["engines"].get(lane, {}).get("usable", False))


def _run_one(test: dict[str, Any], timeout_sec: int) -> dict[str, Any]:
    test_id = test["id"]
    lane = test.get("lane", "arm64-native")
    if test.get("skip_if_engine_missing") and not _engine_usable(lane):
        return {"id": test_id, "status": "SKIP", "reason": f"engine lane is not usable: {lane}"}

    if "exe" in test and not (PROJECT_ROOT / test["exe"]).exists():
        return {"id": test_id, "status": "SKIP", "reason": f"fixture missing: {test['exe']}"}

    bottle = test.get("bottle", f"smoke-{test_id}")
    if "engine_command" in test:
        cmd = [
            sys.executable,
            "-m",
            "app.configurator",
            "--engine-command",
            lane,
            "--bottle",
            bottle,
            "--",
            *test["engine_command"],
        ]
    else:
        cmd = [
            sys.executable,
            "-m",
            "app.configurator",
            "--real",
            "--bottle",
            bottle,
            str(PROJECT_ROOT / test["exe"]),
        ]

    start = time.monotonic()
    timed_out = False
    try:
        proc = subprocess.run(cmd, cwd=PROJECT_ROOT, text=True, capture_output=True, timeout=test.get("timeout_sec", timeout_sec), check=False)
        exit_code = proc.returncode
        stdout = proc.stdout or ""
        stderr = proc.stderr or ""
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        exit_code = 124
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
    duration_ms = int((time.monotonic() - start) * 1000)

    expected_exit = int(test.get("expected_exit", 0))
    expected_text = test.get("expected_stdout_contains")
    passed = exit_code == expected_exit and (not expected_text or expected_text in stdout)
    error_analysis = analyze_smoke_stderr(stderr)
    return {
        "id": test_id,
        "status": "PASS" if passed else "FAIL",
        "lane": lane,
        "command": cmd,
        "exit_code": exit_code,
        "expected_exit": expected_exit,
        "timed_out": timed_out,
        "duration_ms": duration_ms,
        "stdout_tail": stdout[-2000:],
        "stderr_tail": stderr[-2000:],
        "expected_stdout_contains": expected_text,
        "error_analysis": error_analysis,
    }


def run_smoke_matrix(matrix_path: Path = DEFAULT_MATRIX, timeout_sec: int = 45) -> dict[str, Any]:
    matrix = _load_matrix(matrix_path)
    results = [_run_one(test, timeout_sec) for test in matrix.get("tests", [])]
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime())
    report = {
        "schema_version": 1,
        "matrix": str(matrix_path),
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "summary": {
            "pass": sum(1 for item in results if item["status"] == "PASS"),
            "fail": sum(1 for item in results if item["status"] == "FAIL"),
            "skip": sum(1 for item in results if item["status"] == "SKIP"),
        },
        "results": results,
    }
    report_path = REPORT_ROOT / f"smoke-matrix-{stamp}.json"
    report["report_path"] = str(report_path)
    write_json(report_path, report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run MacRunner real smoke matrix")
    parser.add_argument("--matrix", default=str(DEFAULT_MATRIX))
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--json", action="store_true")
    ns = parser.parse_args(argv)
    report = run_smoke_matrix(Path(ns.matrix), timeout_sec=ns.timeout)
    print(dump_json(report) if ns.json else f"smoke matrix: {report['summary']} report={report['report_path']}")
    return 1 if report["summary"]["fail"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
