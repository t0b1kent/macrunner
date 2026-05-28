#!/usr/bin/env python3
"""Game runtime plan synthesis."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class GameRuntimePlan:
    graphics_api: str | None
    graphics_backend: str | None
    shader_cache_enabled: bool
    shader_cache_root: str | None
    input_mode: str | None
    fast_io_enabled: bool
    threading_mode: str | None
    anti_cheat_mode: str | None
    online_supported: bool


def build_game_runtime_plan(*, profile, bottle, graphics_plan, shader_cache_plan, input_plan, fast_io_plan, threading_plan) -> GameRuntimePlan:
    anti_cheat = profile.anti_cheat or {}
    graphics_api = graphics_plan.requested_api
    return GameRuntimePlan(
        graphics_api=graphics_api,
        graphics_backend=graphics_plan.selected_backend,
        shader_cache_enabled=shader_cache_plan.enabled,
        shader_cache_root=shader_cache_plan.cache_root if shader_cache_plan.enabled else None,
        input_mode=input_plan.controller_mode,
        fast_io_enabled=fast_io_plan.enabled,
        threading_mode=threading_plan.mode,
        anti_cheat_mode=str(anti_cheat.get("mode", "none")),
        online_supported=bool(anti_cheat.get("online_supported", False)),
    )

