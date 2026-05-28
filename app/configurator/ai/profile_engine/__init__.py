"""MacRunner AI Configurator — Profile Engine (Phase A.2).

Template inheritance system: resolves template chains into materialized
ProgramProfile instances with full provenance tracking.

Resolution rules:
1. Chain order: left to right. _base → category → specific.
2. Deep merge: nested dicts merged at leaf level; child overrides parent.
3. List append: tuples/lists concatenated.
4. Env merge: dicts merged; child key wins on conflict.
5. Null override: explicit null removes parent value.
"""

from __future__ import annotations

import copy
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping

from app.configurator.profiles import ProgramProfile, default_profile, profile_from_dict


REPO_ROOT = Path(__file__).resolve().parents[4]
PROFILE_DIR = REPO_ROOT / "profiles"
TEMPLATE_DIR = PROFILE_DIR / "templates"


def _deep_merge(base: Any, override: Any) -> Any:
    """Deep-merge override into base.

    - dict: recursive merge, override key wins at leaf.
    - list/tuple: concatenate.
    - None in override: removes key (returns sentinel).
    - scalar: override replaces.
    """
    if override is None:
        return None  # sentinel for removal
    if isinstance(base, dict) and isinstance(override, dict):
        out = copy.deepcopy(base)
        for key, val in override.items():
            if val is None:
                out.pop(key, None)
            elif key in out:
                out[key] = _deep_merge(out[key], val)
            else:
                out[key] = copy.deepcopy(val)
        return out
    if isinstance(base, (list, tuple)) and isinstance(override, (list, tuple)):
        combined = list(base) + list(override)
        # De-duplicate preserving order
        seen: set[Any] = set()
        out: list[Any] = []
        for item in combined:
            # dict items not hashable; use id or string repr for dedup
            key = json.dumps(item, sort_keys=True) if isinstance(item, dict) else item
            if key not in seen:
                seen.add(key)
                out.append(item)
        return out
    return copy.deepcopy(override)


def _load_template(template_id: str) -> dict[str, Any]:
    """Load a template JSON by id."""
    path = TEMPLATE_DIR / f"{template_id}.json"
    if not path.exists():
        raise FileNotFoundError(f"template not found: {template_id}")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, Mapping):
        raise ValueError(f"{path}: template must be a JSON object")
    return dict(data)


def _resolve_template(template_id: str, _stack: set[str] | None = None) -> dict[str, Any]:
    """Recursively resolve a single template with circular-guard."""
    if _stack is None:
        _stack = set()
    if template_id in _stack:
        raise ValueError(f"circular template inheritance detected: {template_id}")
    tpl = _load_template(template_id)
    parent_ids = list(tpl.get("inherits", []))
    merged: dict[str, Any] = {}
    if parent_ids:
        _stack.add(template_id)
        for pid in parent_ids:
            merged = _deep_merge(merged, _resolve_template(pid, _stack))
        _stack.discard(template_id)
    overrides = tpl.get("overrides", {})
    merged = _deep_merge(merged, overrides)
    return merged


def _resolve_template_chain(template_ids: list[str]) -> dict[str, Any]:
    """Resolve a list of template ids into a single merged dict."""
    merged: dict[str, Any] = {}
    for tid in template_ids:
        merged = _deep_merge(merged, _resolve_template(tid))
    return merged


def resolve_profile_with_templates(
    profile_path: Path | str,
) -> tuple[ProgramProfile, dict[str, str], tuple[str, ...]]:
    """Load a v3 profile and resolve its template chain.

    Returns:
        - materialized ProgramProfile
        - provenance dict: field_name -> "template:_base" or "override:profile"
        - template_chain tuple
    """
    p = Path(profile_path)
    data = json.loads(p.read_text(encoding="utf-8"))
    if not isinstance(data, Mapping):
        raise ValueError(f"{p}: profile must be a JSON object")

    schema_version = int(data.get("schema_version", 2))
    if schema_version < 3:
        # Lazy migration: wrap flat v2 as v3 with empty inherits
        # All fields that differ from default_profile() go into overrides
        base_defaults = default_profile()
        overrides: dict[str, Any] = {}
        for key, val in data.items():
            if key in {"schema_version", "id", "kind", "inherits", "overrides", "source"}:
                continue
            default_val = getattr(base_defaults, key, None)
            if val != default_val:
                overrides[key] = val
        data = {
            "schema_version": 3,
            "id": data.get("id", "unknown"),
            "kind": "profile",
            "name": data.get("name", "Unknown"),
            "inherits": [],
            "overrides": overrides,
        }

    inherits = list(data.get("inherits", []))
    overrides = dict(data.get("overrides", {}))
    # Pull identity fields from top-level into overrides so they participate in materialization
    for identity_key in ("name", "category", "region", "status", "windows_version",
                         "architecture", "supported_machines", "default_lane",
                         "future_preferred_lane", "notes"):
        if identity_key in data and data[identity_key] is not None:
            overrides[identity_key] = data[identity_key]
    template_chain = tuple(inherits)

    # Start with default profile as base
    base = default_profile()
    base_dict = {k: v for k, v in base.__dict__.items() if not k.startswith("_")}

    # Resolve template chain
    if inherits:
        template_merged = _resolve_template_chain(inherits)
        base_dict = _deep_merge(base_dict, template_merged)

    # Apply profile-specific overrides
    base_dict = _deep_merge(base_dict, overrides)

    # Build provenance
    provenance: dict[str, str] = {}
    # Mark all fields from templates
    if inherits:
        template_merged = _resolve_template_chain(inherits)
        for key in template_merged:
            provenance[key] = f"template:{template_chain[-1]}"
    # Mark override fields
    for key in overrides:
        provenance[key] = "override:profile"

    # Convert back to ProgramProfile
    profile = profile_from_dict(base_dict, source=str(p))
    return profile, provenance, template_chain


def materialize_profile(
    profile_ref: str | Path,
) -> tuple[ProgramProfile, dict[str, str], tuple[str, ...]]:
    """Public API: resolve any profile (v2 or v3) with templates."""
    p = Path(profile_ref)
    if not p.exists():
        # Try under profiles/
        p = PROFILE_DIR / f"{profile_ref}.json"
        if not p.exists():
            p = PROFILE_DIR / profile_ref
    if not p.exists():
        raise FileNotFoundError(f"profile not found: {profile_ref}")
    return resolve_profile_with_templates(p)
