#!/usr/bin/env python3
"""Fast I/O runtime planning."""

from __future__ import annotations

import json
import os
import shutil
import tempfile
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class FastIOPlan:
    enabled: bool
    prefetch_enabled: bool
    asset_cache_enabled: bool
    case_sensitivity_check: bool
    bottle_on_fast_volume: bool | None
    env: dict[str, str] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


def _cache_dirs(bottle_root: str | Path, bottle_id: str) -> dict[str, Path]:
    bottle = Path(bottle_root).expanduser() / bottle_id
    return {
        "root": bottle / "cache",
        "file_index": bottle / "cache/file-index",
        "prefetch": bottle / "cache/prefetch",
        "assets": bottle / "cache/assets",
    }


def _safe_stat(path: Path) -> dict[str, Any]:
    st = path.stat()
    return {
        "path": str(path),
        "size_bytes": st.st_size,
        "mtime": int(st.st_mtime),
    }


def _case_sensitive_check(path: Path) -> bool:
    probe = path / "CaseProbe"
    alt = path / "caseprobe"
    try:
        probe.write_text("1", encoding="utf-8")
        exists_before = alt.exists()
        if not exists_before:
            alt.write_text("2", encoding="utf-8")
        return probe.read_text(encoding="utf-8") != alt.read_text(encoding="utf-8")
    except OSError:
        return False
    finally:
        for item in (probe, alt):
            try:
                if item.exists():
                    item.unlink()
            except OSError:
                pass


def _dir_writable(path: Path) -> bool:
    try:
        path.mkdir(parents=True, exist_ok=True)
        probe = path / ".macrunner-write-probe"
        probe.write_text("1", encoding="utf-8")
        probe.unlink()
        return True
    except OSError:
        return False


def _walk_files(root: Path) -> list[Path]:
    if not root.exists():
        return []
    return [path for path in root.rglob("*") if path.is_file()]


def _largest_files(files: list[Path], limit: int = 20) -> list[dict[str, Any]]:
    return [
        _safe_stat(path)
        for path in sorted(files, key=lambda item: item.stat().st_size, reverse=True)[:limit]
    ]


def _write_json(path: Path, payload: dict[str, Any]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
    return path


def build_fast_io_plan(
    *,
    profile,
    bottle,
    bottle_root: str | Path | None = None,
    bottle_id: str | None = None,
    materialize: bool = False,
) -> FastIOPlan:
    bottle_id = bottle_id or getattr(bottle, "id", "default")
    bottle_root = bottle_root or Path.home() / "Library/Application Support/MacRunner/Bottles"
    dirs = _cache_dirs(bottle_root, bottle_id)
    warnings: list[str] = []
    case_sensitive = False
    bottle_on_fast_volume: bool | None = None
    if materialize:
        for path in dirs.values():
            path.mkdir(parents=True, exist_ok=True)
        case_sensitive = _case_sensitive_check(dirs["root"])
        bottle_on_fast_volume = os.access(Path(bottle_root), os.W_OK)
    env = {
        "MACRUNNER_FAST_IO": "1" if bool((profile.performance or {}).get("fast_io", False)) else "0",
        "MACRUNNER_FAST_IO_CACHE": str(dirs["root"]),
        "MACRUNNER_FAST_IO_PREFETCH": str(dirs["prefetch"]),
    }
    return FastIOPlan(
        enabled=bool((profile.performance or {}).get("fast_io", False)),
        prefetch_enabled=True,
        asset_cache_enabled=True,
        case_sensitivity_check=case_sensitive,
        bottle_on_fast_volume=bottle_on_fast_volume,
        env=env,
        warnings=warnings,
    )


def fast_io_check(profile, bottle, bottle_root: str | Path | None = None, bottle_id: str | None = None) -> dict[str, Any]:
    plan = build_fast_io_plan(profile=profile, bottle=bottle, bottle_root=bottle_root, bottle_id=bottle_id, materialize=True)
    dirs = _cache_dirs(bottle_root or Path.home() / "Library/Application Support/MacRunner/Bottles", bottle_id or getattr(bottle, "id", "default"))
    root = Path(bottle_root or Path.home() / "Library/Application Support/MacRunner/Bottles") / (bottle_id or getattr(bottle, "id", "default"))
    result = {
        "enabled": plan.enabled,
        "prefetch_enabled": plan.prefetch_enabled,
        "asset_cache_enabled": plan.asset_cache_enabled,
        "case_sensitive": plan.case_sensitivity_check,
        "bottle_on_fast_volume": plan.bottle_on_fast_volume,
        "writable": _dir_writable(dirs["root"]),
        "cache_root": str(dirs["root"]),
        "file_index_root": str(dirs["file_index"]),
        "prefetch_root": str(dirs["prefetch"]),
        "assets_root": str(dirs["assets"]),
        "warnings": plan.warnings,
    }
    result["plan"] = {
        "enabled": result["enabled"],
        "prefetch_enabled": result["prefetch_enabled"],
        "asset_cache_enabled": result["asset_cache_enabled"],
        "case_sensitivity_check": result["case_sensitive"],
        "bottle_on_fast_volume": result["bottle_on_fast_volume"],
    }
    return result


def fast_io_index(profile, bottle_root: str | Path, bottle_id: str) -> dict[str, Any]:
    dirs = _cache_dirs(bottle_root, bottle_id)
    for path in dirs.values():
        path.mkdir(parents=True, exist_ok=True)
    bottle_drive = Path(bottle_root).expanduser() / bottle_id / "drive_c"
    files = _walk_files(bottle_drive)
    payload = {
        "schema_version": 1,
        "bottle_id": bottle_id,
        "case_sensitive": _case_sensitive_check(dirs["root"]),
        "writable": _dir_writable(dirs["root"]),
        "file_count": len(files),
        "total_bytes": sum(path.stat().st_size for path in files),
        "largest_files": _largest_files(files),
        "drive_c": str(bottle_drive),
    }
    _write_json(dirs["file_index"] / "file-index.json", payload)
    return payload


def fast_io_prefetch(profile, bottle_root: str | Path, bottle_id: str) -> dict[str, Any]:
    dirs = _cache_dirs(bottle_root, bottle_id)
    for path in dirs.values():
        path.mkdir(parents=True, exist_ok=True)
    manifest = dirs["prefetch"] / "prefetch.json"
    prefetch_paths = list(profile.performance.get("prefetch_paths", []))
    if manifest.exists():
        try:
            existing = json.loads(manifest.read_text(encoding="utf-8"))
            prefetch_paths = list(existing.get("paths", prefetch_paths))
        except Exception:
            pass
    payload = {
        "schema_version": 1,
        "enabled": True,
        "paths": prefetch_paths,
        "last_run": None,
    }
    manifest.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
    prefetched: list[dict[str, Any]] = []
    for rel_path in prefetch_paths:
        target = Path(bottle_root).expanduser() / bottle_id / rel_path
        if not target.exists():
            continue
        if target.is_file():
            with target.open("rb") as fh:
                prefetched.append({"path": rel_path, "size_bytes": target.stat().st_size, "header_bytes": fh.read(64)[:16].hex()})
        else:
            count = len([item for item in target.rglob("*") if item.is_file()])
            prefetched.append({"path": rel_path, "file_count": count})
    return {
        "prefetch_manifest": str(manifest),
        "cache_root": str(dirs["root"]),
        "paths": payload["paths"],
        "prefetched": prefetched,
        "writable": _dir_writable(dirs["root"]),
    }
