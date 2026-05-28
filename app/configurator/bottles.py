#!/usr/bin/env python3
"""Bottle management for MacRunner."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Mapping


DEFAULT_BOTTLE_ROOT = Path.home() / "Library/Application Support/MacRunner/Bottles"


class BottleError(Exception):
    """Base bottle error."""


class BottleMismatchError(BottleError):
    """Raised when an existing bottle does not match the requested lane or arch."""


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


@dataclass(frozen=True)
class BottleManifest:
    schema_version: int
    id: str
    name: str
    category: str
    lane: str
    guest_machine: str
    host_execution: str
    native_level: str
    engine_id: str
    profile_id: str | None
    created_at: str
    updated_at: str
    features: dict[str, Any] = field(default_factory=dict)


def bottle_root_path(root: str | Path | None = None) -> Path:
    return (Path(root).expanduser() if root is not None else DEFAULT_BOTTLE_ROOT).resolve()


def bottle_path(root: str | Path | None, bottle_id: str) -> Path:
    return bottle_root_path(root) / bottle_id


def manifest_path(root: str | Path | None, bottle_id: str) -> Path:
    return bottle_path(root, bottle_id) / "bottle.json"


def ensure_layout(root: str | Path | None, bottle_id: str) -> dict[str, Path]:
    bottle_dir = bottle_path(root, bottle_id)
    layout = {
        "bottle": bottle_dir,
        "drive_c": bottle_dir / "drive_c",
        "logs": bottle_dir / "logs",
        "cache": bottle_dir / "cache",
        "shader_cache": bottle_dir / "shader-cache",
        "reports": bottle_dir / "reports",
    }
    for path in layout.values():
        path.mkdir(parents=True, exist_ok=True)
    return layout


def manifest_to_dict(manifest: BottleManifest) -> dict[str, Any]:
    return {
        "schema_version": manifest.schema_version,
        "id": manifest.id,
        "name": manifest.name,
        "category": manifest.category,
        "lane": manifest.lane,
        "guest_machine": manifest.guest_machine,
        "host_execution": manifest.host_execution,
        "native_level": manifest.native_level,
        "engine_id": manifest.engine_id,
        "profile_id": manifest.profile_id,
        "created_at": manifest.created_at,
        "updated_at": manifest.updated_at,
        "features": manifest.features,
    }


def manifest_from_dict(data: Mapping[str, Any]) -> BottleManifest:
    return BottleManifest(
        schema_version=int(data.get("schema_version", 0)),
        id=str(data.get("id", "")),
        name=str(data.get("name", "")),
        category=str(data.get("category", "")),
        lane=str(data.get("lane", "")),
        guest_machine=str(data.get("guest_machine", "")),
        host_execution=str(data.get("host_execution", "")),
        native_level=str(data.get("native_level", "")),
        engine_id=str(data.get("engine_id", "")),
        profile_id=data.get("profile_id"),
        created_at=str(data.get("created_at", "")),
        updated_at=str(data.get("updated_at", "")),
        features=dict(data.get("features") or {}),
    )


def load_manifest(root: str | Path | None, bottle_id: str) -> BottleManifest | None:
    path = manifest_path(root, bottle_id)
    if not path.exists():
        return None
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, Mapping):
        raise BottleError(f"{path}: bottle manifest must be an object")
    return manifest_from_dict(payload)


def write_manifest(root: str | Path | None, manifest: BottleManifest) -> Path:
    layout = ensure_layout(root, manifest.id)
    path = layout["bottle"] / "bottle.json"
    path.write_text(json.dumps(manifest_to_dict(manifest), indent=2, ensure_ascii=False), encoding="utf-8")
    return path


def ensure_bottle(
    root: str | Path | None,
    *,
    bottle_id: str,
    name: str,
    category: str,
    lane: str,
    guest_machine: str,
    host_execution: str,
    native_level: str,
    engine_id: str,
    profile_id: str | None,
    features: dict[str, Any] | None = None,
    allow_migration: bool = False,
) -> BottleManifest:
    existing = load_manifest(root, bottle_id)
    now = _utc_now()
    if existing is not None:
        if existing.lane != lane:
            if allow_migration:
                raise BottleMismatchError("Bottle migration is not implemented yet.")
            raise BottleMismatchError(f"bottle lane mismatch: {existing.lane} != {lane}")
        if existing.guest_machine != guest_machine:
            raise BottleMismatchError(f"bottle arch mismatch: {existing.guest_machine} != {guest_machine}")
        updated = BottleManifest(
            schema_version=existing.schema_version or 2,
            id=existing.id,
            name=existing.name or name,
            category=existing.category or category,
            lane=lane,
            guest_machine=guest_machine,
            host_execution=host_execution,
            native_level=native_level,
            engine_id=engine_id,
            profile_id=profile_id or existing.profile_id,
            created_at=existing.created_at or now,
            updated_at=now,
            features=existing.features or dict(features or {}),
        )
        write_manifest(root, updated)
        return updated

    manifest = BottleManifest(
        schema_version=2,
        id=bottle_id,
        name=name,
        category=category,
        lane=lane,
        guest_machine=guest_machine,
        host_execution=host_execution,
        native_level=native_level,
        engine_id=engine_id,
        profile_id=profile_id,
        created_at=now,
        updated_at=now,
        features=dict(features or {}),
    )
    write_manifest(root, manifest)
    return manifest
