#!/usr/bin/env python3
"""Persistent shader cache management."""

from __future__ import annotations

import json
import shutil
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


@dataclass(frozen=True)
class ShaderCachePlan:
    enabled: bool
    cache_root: str
    backend_cache_root: str
    warmup_enabled: bool
    cache_size_mb: int | None
    env: dict[str, str] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


def shader_cache_root(bottle_root: str | Path, bottle_id: str) -> Path:
    return Path(bottle_root).expanduser() / bottle_id / "shader-cache"


def shader_cache_backend_root(bottle_root: str | Path, bottle_id: str, backend: str) -> Path:
    return shader_cache_root(bottle_root, bottle_id) / backend


def _metadata_path(cache_root: Path) -> Path:
    return cache_root / "metadata.json"


def _metadata_payload(bottle_id: str, backend: str, entries_estimated: int = 0, size_bytes: int = 0) -> dict[str, Any]:
    now = _utc_now()
    return {
        "schema_version": 1,
        "bottle_id": bottle_id,
        "backend": backend,
        "created_at": now,
        "updated_at": now,
        "entries_estimated": entries_estimated,
        "size_bytes": size_bytes,
    }


def _write_metadata(cache_root: Path, bottle_id: str, backend: str) -> None:
    cache_root.mkdir(parents=True, exist_ok=True)
    payload = _metadata_payload(bottle_id, backend)
    _metadata_path(cache_root).write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")


def _count_backend(cache_root: Path) -> tuple[int, int]:
    total_size = 0
    entries = 0
    if cache_root.exists():
        for path in cache_root.rglob("*"):
            if path.is_file():
                entries += 1
                total_size += path.stat().st_size
    return entries, total_size


def build_shader_cache_plan(
    *,
    bottle_root: str | Path,
    bottle_id: str,
    backend: str,
    enabled: bool = True,
    warmup_enabled: bool = False,
    cache_size_mb: int | None = None,
    materialize: bool = False,
) -> ShaderCachePlan:
    root = shader_cache_root(bottle_root, bottle_id)
    backend_root = shader_cache_backend_root(bottle_root, bottle_id, backend)
    warnings: list[str] = []
    if materialize and enabled:
        backend_root.mkdir(parents=True, exist_ok=True)
        _write_metadata(root, bottle_id, backend)
    env = {
        "MACRUNNER_SHADER_CACHE_ROOT": str(root),
        "MACRUNNER_SHADER_CACHE_BACKEND": backend,
    }
    return ShaderCachePlan(
        enabled=enabled,
        cache_root=str(root),
        backend_cache_root=str(backend_root),
        warmup_enabled=warmup_enabled,
        cache_size_mb=cache_size_mb,
        env=env,
        warnings=warnings,
    )


def shader_cache_init(bottle_root: str | Path, bottle_id: str, backend: str) -> dict[str, Any]:
    root = shader_cache_root(bottle_root, bottle_id)
    backend_root = shader_cache_backend_root(bottle_root, bottle_id, backend)
    backend_root.mkdir(parents=True, exist_ok=True)
    _write_metadata(root, bottle_id, backend)
    entries, size_bytes = _count_backend(backend_root)
    return {
        "cache_root": str(root),
        "backend_cache_root": str(backend_root),
        "backend": backend,
        "entries_estimated": entries,
        "size_bytes": size_bytes,
    }


def shader_cache_stats(bottle_root: str | Path, bottle_id: str, backend: str | None = None) -> dict[str, Any]:
    root = shader_cache_root(bottle_root, bottle_id)
    if backend:
        backend_root = shader_cache_backend_root(bottle_root, bottle_id, backend)
        entries, size_bytes = _count_backend(backend_root)
        metadata_path = _metadata_path(root)
        metadata = json.loads(metadata_path.read_text(encoding="utf-8")) if metadata_path.exists() else None
        return {
            "cache_root": str(root),
            "backend_cache_root": str(backend_root),
            "backend": backend,
            "entries_estimated": entries,
            "size_bytes": size_bytes,
            "metadata": metadata,
        }

    total_size = 0
    entries = 0
    backends: dict[str, dict[str, int]] = {}
    if root.exists():
        for child in root.iterdir():
            if child.is_dir():
                b_entries, b_size = _count_backend(child)
                backends[child.name] = {"entries_estimated": b_entries, "size_bytes": b_size}
                entries += b_entries
                total_size += b_size
    metadata_path = _metadata_path(root)
    metadata = json.loads(metadata_path.read_text(encoding="utf-8")) if metadata_path.exists() else None
    return {
        "cache_root": str(root),
        "entries_estimated": entries,
        "size_bytes": total_size,
        "backends": backends,
        "metadata": metadata,
    }


def clean_shader_cache(bottle_root: str | Path, bottle_id: str, backend: str | None = None) -> None:
    root = shader_cache_root(bottle_root, bottle_id)
    if backend:
        backend_root = shader_cache_backend_root(bottle_root, bottle_id, backend)
        if backend_root.exists():
            shutil.rmtree(backend_root)
        return
    if root.exists():
        shutil.rmtree(root)
