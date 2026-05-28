#!/usr/bin/env python3
"""Program profile loading and bottle layout helpers."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping


REPO_ROOT = Path(__file__).resolve().parents[2]
PROFILE_DIR = REPO_ROOT / "profiles"
DEFAULT_PROFILE_ID = "default"


def _opt_str(value: object | None) -> str | None:
    if value is None:
        return None
    text = str(value).strip()
    return text or None


def _as_str_tuple(value: object | None) -> tuple[str, ...]:
    if not value:
        return ()
    if isinstance(value, (list, tuple)):
        return tuple(str(item) for item in value)
    return (str(value),)


def _as_patch_tuple(value: object | None) -> tuple[dict[str, Any], ...]:
    if not value:
        return ()
    if not isinstance(value, (list, tuple)):
        return ()
    patches: list[dict[str, Any]] = []
    for item in value:
        if isinstance(item, Mapping):
            patches.append(dict(item))
    return tuple(patches)


def _as_mapping(value: object | None) -> dict[str, str]:
    if not isinstance(value, Mapping):
        return {}
    out: dict[str, str] = {}
    for key, raw in value.items():
        text = _opt_str(raw)
        if text is not None:
            out[str(key)] = text
    return out


def _as_dict(value: object | None) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        return {}
    return dict(value)


@dataclass(frozen=True)
class ProgramProfile:
    schema_version: int = 1
    id: str = DEFAULT_PROFILE_ID
    name: str = "Default"
    category: str | None = None
    region: str | None = None
    status: str | None = None
    windows_version: str | None = None
    architecture: str | None = None
    supported_machines: tuple[str, ...] = ()
    default_lane: str | None = None
    future_preferred_lane: str | None = None
    bottle_policy: dict[str, Any] = field(default_factory=dict)
    graphics: dict[str, Any] = field(default_factory=dict)
    performance: dict[str, Any] = field(default_factory=dict)
    input_settings: dict[str, Any] = field(default_factory=dict)
    anti_cheat: dict[str, Any] = field(default_factory=dict)
    env: dict[str, str] = field(default_factory=dict)
    args: tuple[str, ...] = ()
    required_dlls: tuple[str, ...] = ()
    patches: tuple[dict[str, Any], ...] = ()
    wine_settings: dict[str, str] = field(default_factory=dict)
    known_issues: tuple[str, ...] = ()
    test_status: dict[str, Any] = field(default_factory=dict)
    notes: str | None = None
    source: str | None = None

    def merged_env(self) -> dict[str, str]:
        env = dict(self.wine_settings)
        env.update(self.env)
        return env


def default_profile() -> ProgramProfile:
    return ProgramProfile(
        schema_version=2,
        id=DEFAULT_PROFILE_ID,
        name="Default",
        category="utility",
        architecture="arm64",
        supported_machines=("arm64", "x86_64", "arm64ec", "arm64x"),
        default_lane="arm64-native",
        bottle_policy={"isolation": "shared", "default_bottle_id": DEFAULT_PROFILE_ID},
        graphics={"api": None, "shader_cache": False},
        performance={"fast_io": False, "threading": "auto"},
        input_settings={"controller": "auto"},
        anti_cheat={"mode": "none", "online_supported": False},
        env={},
        args=(),
        source=None,
    )


def _resolve_overrides(data: Mapping[str, Any]) -> Mapping[str, Any]:
    """Schema v3 places profile fields inside 'overrides'; fall back to top-level."""
    overrides = data.get("overrides")
    if isinstance(overrides, Mapping):
        # Merge top-level (e.g. id, name) with overrides taking precedence for nested fields.
        merged: dict[str, Any] = dict(data)
        merged.update(overrides)
        return merged
    return data


def profile_from_dict(data: Mapping[str, Any], source: str | None = None) -> ProgramProfile:
    schema_version = int(data.get("schema_version", 1))
    resolved = _resolve_overrides(data)
    return ProgramProfile(
        schema_version=schema_version,
        id=_opt_str(resolved.get("id")) or DEFAULT_PROFILE_ID,
        name=_opt_str(resolved.get("name")) or "Default",
        category=_opt_str(resolved.get("category")),
        region=_opt_str(resolved.get("region")),
        status=_opt_str(resolved.get("status")),
        windows_version=_opt_str(resolved.get("windows_version")),
        architecture=_opt_str(resolved.get("architecture")),
        supported_machines=_as_str_tuple(resolved.get("supported_machines")),
        default_lane=_opt_str(resolved.get("default_lane")),
        future_preferred_lane=_opt_str(resolved.get("future_preferred_lane")),
        bottle_policy=_as_dict(resolved.get("bottle_policy")),
        graphics=_as_dict(resolved.get("graphics")),
        performance=_as_dict(resolved.get("performance")),
        input_settings=_as_dict(resolved.get("input")),
        anti_cheat=_as_dict(resolved.get("anti_cheat")),
        env=_as_mapping(resolved.get("env")),
        args=_as_str_tuple(resolved.get("args")),
        required_dlls=_as_str_tuple(resolved.get("required_dlls")),
        patches=_as_patch_tuple(resolved.get("patches")),
        wine_settings=_as_mapping(resolved.get("wine_settings")),
        known_issues=_as_str_tuple(resolved.get("known_issues")),
        test_status=dict(resolved.get("test_status") or {}),
        notes=_opt_str(resolved.get("notes")),
        source=source,
    )


def load_profile(profile_ref: str) -> ProgramProfile:
    ref_path = Path(profile_ref)
    candidates: list[Path] = []

    if ref_path.exists():
        candidates.append(ref_path)
    if not ref_path.is_absolute():
        candidates.append(PROFILE_DIR / f"{profile_ref}.json")
        candidates.append(PROFILE_DIR / ref_path.name)

    seen: set[str] = set()
    for candidate in candidates:
        key = str(candidate.resolve()) if candidate.exists() else str(candidate)
        if key in seen:
            continue
        seen.add(key)
        if candidate.exists():
            payload = json.loads(candidate.read_text(encoding="utf-8"))
            if not isinstance(payload, Mapping):
                raise ValueError(f"{candidate}: profile JSON must be an object")
            return profile_from_dict(payload, source=str(candidate))

    raise FileNotFoundError(f"profile not found: {profile_ref}")


def resolve_profile(profile_ref: str | None) -> ProgramProfile:
    if not profile_ref:
        return default_profile()
    return load_profile(profile_ref)


def normalize_profile_arch(architecture: str | None) -> str:
    if architecture in {"x86_64", "i386"}:
        return "x86_64"
    if architecture == "arm64":
        return "arm64"
    if architecture in {"arm64ec", "arm64x"}:
        return architecture
    return "unknown"

