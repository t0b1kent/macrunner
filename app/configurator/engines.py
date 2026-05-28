#!/usr/bin/env python3
"""Engine registry loading and selection."""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Sequence

from app.configurator.pe import PEMachine


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ENGINE_REGISTRY = REPO_ROOT / "config/engines.json"


class EngineRegistryError(Exception):
    """Raised when the engine registry is invalid or unusable."""


@dataclass(frozen=True)
class EngineDefinition:
    id: str
    lane: str
    host_arch: str
    guest_machines: tuple[str, ...]
    path: str
    status: str
    native_level: str
    requires_rosetta: bool
    requires_experimental: bool
    available: bool = True
    warnings: tuple[str, ...] = ()


@dataclass(frozen=True)
class EngineRegistry:
    schema_version: int
    engines: tuple[EngineDefinition, ...]
    source: str | None = None
    warnings: tuple[str, ...] = ()

    def by_lane(self, lane: str) -> EngineDefinition | None:
        for engine in self.engines:
            if engine.lane == lane:
                return engine
        return None

    def matching(self, machine: PEMachine) -> tuple[EngineDefinition, ...]:
        machine_key = machine.value
        return tuple(
            engine
            for engine in self.engines
            if machine_key in engine.guest_machines or machine == PEMachine.UNKNOWN
        )


def _path_exists(path: str) -> bool:
    return _resolve_path(path).exists()


def _resolve_path(path: str) -> Path:
    candidate = Path(path)
    return candidate if candidate.is_absolute() else (REPO_ROOT / candidate)


def _mach_o_arch(path: Path) -> str | None:
    try:
        with path.open("rb") as handle:
            header = handle.read(8)
    except OSError:
        return None
    if len(header) < 8:
        return None
    magic, cputype = struct.unpack_from("<II", header, 0)
    if magic != 0xFEEDFACF:
        return None
    if cputype == 0x0100000C:
        return "arm64"
    if cputype == 0x01000007:
        return "x86_64"
    return f"mach-o-0x{cputype:x}"


def _engine_from_dict(item: dict[str, Any]) -> EngineDefinition:
    warnings: list[str] = []
    path = str(item.get("path", ""))
    resolved_path = _resolve_path(path)
    available = resolved_path.exists()
    if not available:
        warnings.append(f"engine path missing: {path}")
    else:
        lane = str(item.get("lane", ""))
        requires_rosetta = bool(item.get("requires_rosetta", False))
        binary_arch = _mach_o_arch(resolved_path)
        if lane == "arm64-native" and binary_arch != "arm64":
            available = False
            warnings.append(f"arm64-native engine is not an arm64 Mach-O binary: {path}")
        if requires_rosetta and binary_arch != "x86_64":
            available = False
            warnings.append(f"rosetta engine is not an x86_64 Mach-O binary: {path}")
    return EngineDefinition(
        id=str(item.get("id", "")),
        lane=str(item.get("lane", "")),
        host_arch=str(item.get("host_arch", "")),
        guest_machines=tuple(str(machine) for machine in item.get("guest_machines", [])),
        path=str(resolved_path),
        status=str(item.get("status", "unknown")),
        native_level=str(item.get("native_level", "unknown")),
        requires_rosetta=bool(item.get("requires_rosetta", False)),
        requires_experimental=bool(item.get("requires_experimental", False)),
        available=available,
        warnings=tuple(warnings),
    )


def load_engine_registry(path: str | Path | None = None) -> EngineRegistry:
    registry_path = Path(path) if path is not None else DEFAULT_ENGINE_REGISTRY
    payload = json.loads(registry_path.read_text(encoding="utf-8"))
    schema_version = int(payload.get("schema_version", 0))
    if schema_version != 2:
        raise EngineRegistryError(f"{registry_path}: unsupported schema_version {schema_version}")

    engines = []
    warnings: list[str] = []
    for item in payload.get("engines", []):
        if not isinstance(item, dict):
            raise EngineRegistryError(f"{registry_path}: engine entry must be an object")
        engine = _engine_from_dict(item)
        engines.append(engine)
        warnings.extend(engine.warnings)

    return EngineRegistry(
        schema_version=schema_version,
        engines=tuple(engines),
        source=str(registry_path),
        warnings=tuple(warnings),
    )


def required_engine(registry: EngineRegistry, lane: str) -> EngineDefinition:
    engine = registry.by_lane(lane)
    if engine is None:
        raise EngineRegistryError(f"no engine registered for lane {lane}")
    if not engine.available:
        raise EngineRegistryError(f"required engine missing: {engine.path}")
    return engine
