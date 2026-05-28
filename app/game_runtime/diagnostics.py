#!/usr/bin/env python3
"""Diagnostics helpers for MacRunner game runtime."""

from __future__ import annotations

from dataclasses import asdict
from typing import Any


def build_runtime_diagnostics(*, profile, bottle, game_runtime, graphics_plan, shader_cache_plan, input_plan, fast_io_plan, threading_plan) -> dict[str, Any]:
    return {
        "profile_id": profile.id,
        "bottle_id": bottle.id,
        "game_runtime": asdict(game_runtime),
        "graphics": asdict(graphics_plan),
        "shader_cache": asdict(shader_cache_plan),
        "input": asdict(input_plan),
        "fast_io": asdict(fast_io_plan),
        "threading": asdict(threading_plan),
    }

