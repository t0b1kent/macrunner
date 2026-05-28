#!/usr/bin/env python3
"""Benchmark harness for real MacRunner launches."""

from __future__ import annotations

import subprocess
import time
from dataclasses import asdict
from pathlib import Path
from typing import Any

from app.configurator.compatibility import build_compatibility_plan
from app.configurator.bottles import bottle_root_path
from app.configurator.profiles import resolve_profile
from app.configurator.reports import write_json
from app.game_runtime.fast_io import fast_io_index


def _dir_size(path: Path) -> int:
    total = 0
    if path.exists():
        for item in path.rglob("*"):
            if item.is_file():
                total += item.stat().st_size
    return total


def benchmark_launch(
    *,
    exe_path: str,
    profile_id: str | None = None,
    bottle_root: str | None = None,
    bottle_id: str | None = None,
    dry_run: bool = False,
    timeout_sec: int = 45,
) -> dict[str, Any]:
    start = time.monotonic()
    plan = build_compatibility_plan(
        exe_path=exe_path,
        profile_id=profile_id,
        bottle_id=bottle_id,
        bottle_root=bottle_root,
        materialize=not dry_run,
    )
    plan_build_ms = int((time.monotonic() - start) * 1000)

    if plan.errors:
        return {
            "launch_plan_build_ms": plan_build_ms,
            "errors": plan.errors,
            "warnings": plan.warnings,
            "plan": asdict(plan),
        }

    bottle_dir = Path(plan.wineprefix)
    report_dir = bottle_dir / "reports"
    logs_dir = bottle_dir / "logs"
    report_dir.mkdir(parents=True, exist_ok=True)
    logs_dir.mkdir(parents=True, exist_ok=True)

    fast_io_index_time_ms = 0
    if not dry_run:
        index_start = time.monotonic()
        try:
            fast_io_index(resolve_profile(profile_id), bottle_root_path(bottle_root), plan.bottle_id)
            fast_io_index_time_ms = int((time.monotonic() - index_start) * 1000)
        except Exception:
            fast_io_index_time_ms = -1

    if dry_run:
        result_payload = {
            "plan_build_ms": plan_build_ms,
            "process_start_ms": 0,
            "total_runtime_ms": 0,
            "stdout_bytes": 0,
            "stderr_bytes": 0,
            "shader_cache_size_bytes": _dir_size(bottle_dir / "shader-cache"),
            "bottle_size_bytes": _dir_size(bottle_dir),
            "fast_io_index_time_ms": 0,
            "exit_code": 0,
            "timed_out": False,
            "dry_run": True,
            "warnings": plan.warnings,
            "errors": plan.errors,
            "plan": asdict(plan),
        }
        return result_payload

    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime())
    stdout_path = logs_dir / f"benchmark-{stamp}.stdout.log"
    stderr_path = logs_dir / f"benchmark-{stamp}.stderr.log"

    spawn_start = time.monotonic()
    timed_out = False
    stdout = ""
    stderr = ""
    exit_code = 124
    try:
        proc = subprocess.Popen(plan.command, env=plan.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        process_spawn_ms = int((time.monotonic() - spawn_start) * 1000)
        try:
            stdout, stderr = proc.communicate(timeout=timeout_sec)
            exit_code = proc.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            stdout, stderr = proc.communicate()
            exit_code = 124
    except OSError as exc:
        process_spawn_ms = int((time.monotonic() - spawn_start) * 1000)
        stderr = str(exc)
        exit_code = 127

    total_runtime_ms = int((time.monotonic() - spawn_start) * 1000)
    stdout_path.write_text(stdout or "", encoding="utf-8")
    stderr_path.write_text(stderr or "", encoding="utf-8")

    report_path = report_dir / f"benchmark-{stamp}.json"
    result_payload = {
        "plan_build_ms": plan_build_ms,
        "process_spawn_ms": process_spawn_ms,
        "total_runtime_ms": total_runtime_ms,
        "stdout_bytes": len((stdout or "").encode("utf-8")),
        "stderr_bytes": len((stderr or "").encode("utf-8")),
        "bottle_size_bytes": _dir_size(bottle_dir),
        "shader_cache_size_bytes": _dir_size(bottle_dir / "shader-cache"),
        "fast_io_index_time_ms": fast_io_index_time_ms,
        "exit_code": exit_code,
        "timed_out": timed_out,
        "timeout_sec": timeout_sec,
        "stdout_path": str(stdout_path),
        "stderr_path": str(stderr_path),
        "report_path": str(report_path),
        "warnings": plan.warnings,
        "errors": plan.errors,
        "plan": asdict(plan),
    }
    write_json(report_path, result_payload)
    return result_payload
