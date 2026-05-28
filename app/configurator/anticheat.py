#!/usr/bin/env python3
"""Anti-cheat safe mode planning."""

from __future__ import annotations

from dataclasses import dataclass, field

from app.configurator.profiles import ProgramProfile


@dataclass(frozen=True)
class AntiCheatPlan:
    mode: str
    online_supported: bool
    debug_hooks_enabled: bool
    overlays_enabled: bool
    clean_bottle_recommended: bool
    env: dict[str, str] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


def build_anticheat_plan(profile: ProgramProfile) -> AntiCheatPlan:
    anti_cheat = profile.anti_cheat or {}
    mode = str(anti_cheat.get("mode", "none"))
    online_supported = bool(anti_cheat.get("online_supported", False))
    clean_bottle_recommended = mode in {"safe", "none"}
    warnings: list[str] = []
    if online_supported:
        warnings.append("Online anti-cheat support requires vendor/developer opt-in.")
    env = {
        "MACRUNNER_SAFE_MODE": "1" if mode == "safe" else "0",
        "MACRUNNER_DISABLE_OVERLAYS": "1" if mode in {"safe", "none"} else "0",
        "MACRUNNER_NO_DEBUG_HOOKS": "1" if mode in {"safe", "none"} else "0",
    }
    return AntiCheatPlan(
        mode=mode,
        online_supported=online_supported,
        debug_hooks_enabled=mode not in {"safe", "none"},
        overlays_enabled=mode not in {"safe", "none"},
        clean_bottle_recommended=clean_bottle_recommended,
        env=env,
        warnings=warnings,
    )

