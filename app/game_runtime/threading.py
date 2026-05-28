#!/usr/bin/env python3
"""Threading/sync planning for games."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class ThreadingPlan:
    mode: str
    esync_enabled: bool
    msync_enabled: bool
    ntsync_like_enabled: bool
    macos_native_sync_enabled: bool
    env: dict[str, str] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


def build_threading_plan(*, profile) -> ThreadingPlan:
    requested = str((profile.performance or {}).get("threading", "auto"))
    warnings: list[str] = []
    ntsync_like_enabled = False
    if requested == "auto":
        mode = "esync"
    elif requested == "safe":
        mode = "disabled"
    elif requested == "experimental-ntsync-like":
        mode = "experimental-ntsync-like"
        warnings.append("ntsync-like is planned only; native kernel equivalent is not implemented.")
    elif requested in {"esync", "msync", "macos-native-sync", "disabled"}:
        mode = requested
    else:
        mode = "esync"
        warnings.append(f"Unknown threading mode {requested}; using esync.")

    env = {
        "MACRUNNER_THREADING_MODE": mode,
        "WINEESYNC": "1" if mode == "esync" else "0",
        "WINEFSYNC": "1" if mode == "msync" else "0",
        "MACRUNNER_NATIVE_SYNC": "1" if mode == "macos-native-sync" else "0",
        "MACRUNNER_NTSYNC_LIKE": "0",
    }
    return ThreadingPlan(
        mode=mode,
        esync_enabled=mode == "esync",
        msync_enabled=mode == "msync",
        ntsync_like_enabled=ntsync_like_enabled,
        macos_native_sync_enabled=mode == "macos-native-sync",
        env=env,
        warnings=warnings,
    )

