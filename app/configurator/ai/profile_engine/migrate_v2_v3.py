#!/usr/bin/env python3
"""Migration script: convert existing flat v2 profiles to v3 template-inheritance format.

Usage:
    python3 -m app.configurator.ai.profile_engine.migrate_v2_v3

One-way, idempotent. Backs up original files to .json.bak.
"""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path
from typing import Any, Mapping

REPO_ROOT = Path(__file__).resolve().parents[4]
PROFILE_DIR = REPO_ROOT / "profiles"


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


def _as_dict(value: object | None) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        return {}
    return dict(value)


def _default_for_field(field_name: str) -> Any:
    """Return the default value from default_profile() for a given field."""
    from app.configurator.profiles import default_profile
    base = default_profile()
    return getattr(base, field_name, None)


def migrate_single_profile(data: Mapping[str, Any]) -> dict[str, Any]:
    """Convert a flat v2 profile dict to v3 delta format."""
    # Fields that are part of the identity / metadata, not overrides
    identity_fields = {"schema_version", "id", "name", "category", "region", "status",
                       "windows_version", "architecture", "supported_machines",
                       "default_lane", "future_preferred_lane", "source", "notes"}

    v3: dict[str, Any] = {
        "schema_version": 3,
        "id": data.get("id", "unknown"),
        "kind": "profile",
        "name": data.get("name", "Unknown"),
        "inherits": [],
        "overrides": {},
    }

    # Determine best template guess from category
    category = data.get("category")
    if category == "game":
        graphics = _as_dict(data.get("graphics"))
        api = graphics.get("api") if graphics else None
        if api == "d3d12":
            v3["inherits"] = ["_base", "_category-game-dx12"]
        elif api == "d3d11":
            v3["inherits"] = ["_base", "_category-game-dx11"]
        elif api == "d3d9":
            v3["inherits"] = ["_base", "_category-game-dx9"]
        else:
            v3["inherits"] = ["_base", "_category-game"]
    elif category == "business":
        v3["inherits"] = ["_base", "_category-business"]
    elif category == "utility":
        v3["inherits"] = ["_base", "_category-utility"]
    else:
        v3["inherits"] = ["_base"]

    # Build overrides: any field that differs from default_profile()
    for key, value in data.items():
        if key in {"schema_version", "id", "name", "kind", "inherits", "overrides", "source"}:
            continue
        default_val = _default_for_field(key)
        if _values_equal(value, default_val):
            continue
        v3["overrides"][key] = value

    return v3


def _values_equal(a: Any, b: Any) -> bool:
    """Compare values for equality, handling list/tuple equivalence."""
    if type(a) != type(b) and not (isinstance(a, (list, tuple)) and isinstance(b, (list, tuple))):
        return False
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        return list(a) == list(b)
    if isinstance(a, dict) and isinstance(b, dict):
        return a == b
    return a == b


def migrate_all_profiles() -> dict[str, Any]:
    """Migrate all v2 profiles in profiles/ directory."""
    results: dict[str, Any] = {"migrated": [], "skipped": [], "errors": []}
    for path in sorted(PROFILE_DIR.glob("*.json")):
        if path.name.startswith("_"):
            continue  # skip templates
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(data, Mapping):
                results["skipped"].append({"path": str(path), "reason": "not a mapping"})
                continue
            version = int(data.get("schema_version", 2))
            if version >= 3:
                results["skipped"].append({"path": str(path), "reason": f"already v{version}"})
                continue
            v3 = migrate_single_profile(data)
            # Backup original
            backup = path.with_suffix(".json.bak")
            if not backup.exists():
                shutil.copy2(path, backup)
            # Write v3
            path.write_text(json.dumps(v3, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            results["migrated"].append({"path": str(path), "id": v3["id"], "inherits": v3["inherits"]})
        except Exception as exc:
            results["errors"].append({"path": str(path), "error": str(exc)})
    return results


def main(argv: list[str]) -> int:
    print(f"Migrating profiles in {PROFILE_DIR} ...")
    results = migrate_all_profiles()
    print(f"Migrated: {len(results['migrated'])}")
    for item in results["migrated"]:
        print(f"  + {item['id']} -> inherits {item['inherits']}")
    print(f"Skipped: {len(results['skipped'])}")
    for item in results["skipped"]:
        print(f"  - {Path(item['path']).name}: {item['reason']}")
    if results["errors"]:
        print(f"Errors: {len(results['errors'])}")
        for item in results["errors"]:
            print(f"  ! {item['path']}: {item['error']}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
