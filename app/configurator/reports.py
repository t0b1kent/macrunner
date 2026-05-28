#!/usr/bin/env python3
"""Structured launch report helpers."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, is_dataclass
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class LaunchReport:
    launch_plan: dict[str, Any]
    exit_code: int | None
    duration_ms: int | None
    stdout_path: str | None
    stderr_path: str | None
    warnings: list[str]
    errors: list[str]


def _json_default(value: Any) -> Any:
    if is_dataclass(value):
        return asdict(value)
    if isinstance(value, Path):
        return str(value)
    return value


def dump_json(value: Any) -> str:
    return json.dumps(value, indent=2, ensure_ascii=False, default=_json_default)


def write_json(path: str | Path, value: Any) -> Path:
    out_path = Path(path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(dump_json(value), encoding="utf-8")
    return out_path

