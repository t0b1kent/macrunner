#!/usr/bin/env python3
"""Graphics backend selection for game profiles."""

from __future__ import annotations

import os
import platform
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
BACKEND_ROOT = REPO_ROOT / "engine/graphics/dist"

# Architecture-specific subdirectories for Windows PE DLLs.
_MACHINE_TO_DLL_DIR = {
    "arm64": "aarch64-windows",
    "aarch64": "aarch64-windows",
    "x86_64": "x86_64-windows",
    "amd64": "x86_64-windows",
}
_CURRENT_DLL_DIR = _MACHINE_TO_DLL_DIR.get(platform.machine().lower(), "aarch64-windows")

# Logical backend name -> dist directory name.
_BACKEND_DIR_MAP = {
    "d3dmetal": "dxmt",
    "dxvk-moltenvk": "dxvk",
    "vkd3d-moltenvk": "vkd3d",
    "wined3d": None,
    "native-metal-experimental": None,
    "auto": None,
}


class GraphicsBackend(str, Enum):
    D3DMETAL = "d3dmetal"
    DXVK_MOLTENVK = "dxvk-moltenvk"
    VKD3D_MOLTENVK = "vkd3d-moltenvk"
    WINE_D3D = "wined3d"
    NATIVE_METAL_EXPERIMENTAL = "native-metal-experimental"
    AUTO = "auto"


@dataclass(frozen=True)
class GraphicsPlan:
    requested_api: str | None
    selected_backend: str
    fallback_backend: str | None
    shader_cache_enabled: bool
    backend_available: bool
    backend_path: str | None
    env: dict[str, str] = field(default_factory=dict)
    dll_overrides: dict[str, str] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)


def _dist_dir(name: str | None) -> str | None:
    if not name:
        return None
    return _BACKEND_DIR_MAP.get(name)


def _backend_path(name: str | None) -> Path | None:
    dist_dir = _dist_dir(name)
    if not dist_dir:
        return None
    return BACKEND_ROOT / dist_dir


def _has_dlls(path: Path) -> bool:
    """Return True if the backend has at least one .dll in the current arch dir."""
    arch_dir = path / _CURRENT_DLL_DIR
    return arch_dir.exists() and any(arch_dir.glob("*.dll"))


def _exists(name: str | None) -> bool:
    if not name:
        return False
    # wined3d and native-metal are always available (builtin or OS-level).
    if name in {GraphicsBackend.WINE_D3D.value, GraphicsBackend.NATIVE_METAL_EXPERIMENTAL.value}:
        return True
    path = _backend_path(name)
    return bool(path and path.exists() and _has_dlls(path))


def _join_overrides(overrides: dict[str, str]) -> str:
    return ";".join(f"{key}={value}" for key, value in overrides.items())


def _select_for_api(api: str | None) -> tuple[str, str | None, list[str], list[str]]:
    warnings: list[str] = []
    errors: list[str] = []
    api_key = (api or "").lower()
    if api_key == "d3d12":
        if _exists(GraphicsBackend.D3DMETAL.value):
            return GraphicsBackend.D3DMETAL.value, GraphicsBackend.VKD3D_MOLTENVK.value, warnings, errors
        warnings.append("D3DMetal backend not found.")
        if _exists(GraphicsBackend.VKD3D_MOLTENVK.value):
            warnings.append("Falling back to vkd3d-moltenvk.")
            return GraphicsBackend.VKD3D_MOLTENVK.value, None, warnings, errors
        errors.append("DX12 is unsupported because no D3DMetal or VKD3D backend was found.")
        return GraphicsBackend.WINE_D3D.value, None, warnings, errors
    if api_key == "d3d11":
        if _exists(GraphicsBackend.D3DMETAL.value):
            return GraphicsBackend.D3DMETAL.value, GraphicsBackend.DXVK_MOLTENVK.value, warnings, errors
        warnings.append("D3DMetal backend not found.")
        if _exists(GraphicsBackend.DXVK_MOLTENVK.value):
            warnings.append("Falling back to dxvk-moltenvk.")
            return GraphicsBackend.DXVK_MOLTENVK.value, GraphicsBackend.WINE_D3D.value, warnings, errors
        warnings.append("Falling back to wined3d.")
        return GraphicsBackend.WINE_D3D.value, None, warnings, errors
    if api_key == "d3d9":
        if _exists(GraphicsBackend.DXVK_MOLTENVK.value):
            return GraphicsBackend.DXVK_MOLTENVK.value, GraphicsBackend.WINE_D3D.value, warnings, errors
        warnings.append("DXVK backend not found.")
        warnings.append("Falling back to wined3d.")
        return GraphicsBackend.WINE_D3D.value, None, warnings, errors
    if api_key in {"opengl", "vulkan"}:
        return GraphicsBackend.WINE_D3D.value, None, warnings, errors
    if api_key in {"", "none"}:
        return GraphicsBackend.AUTO.value, None, warnings, errors
    warnings.append(f"Unknown graphics API {api}; using wined3d.")
    return GraphicsBackend.WINE_D3D.value, None, warnings, errors


def build_graphics_plan(*, profile, bottle) -> GraphicsPlan:
    graphics = profile.graphics or {}
    api = graphics.get("api")
    requested_backend = graphics.get("preferred_backend")
    force_dxmt = os.environ.get("MACRUNNER_FORCE_DXMT") == "1"
    if force_dxmt:
        selected_backend = GraphicsBackend.D3DMETAL.value
        fallback_backend = None
        warnings = ["MACRUNNER_FORCE_DXMT=1: overriding backend selection."]
        errors: list[str] = []
    else:
        selected_backend, fallback_backend, warnings, errors = _select_for_api(api)

    if not force_dxmt and requested_backend and _exists(str(requested_backend)):
        selected_backend = str(requested_backend)

    backend_path = _backend_path(selected_backend)
    available = bool(backend_path and backend_path.exists() and _has_dlls(backend_path))
    # Builtin backends are always available.
    if selected_backend in {GraphicsBackend.WINE_D3D.value, GraphicsBackend.NATIVE_METAL_EXPERIMENTAL.value}:
        available = True

    dll_overrides: dict[str, str] = {}
    if selected_backend == GraphicsBackend.DXVK_MOLTENVK.value:
        dll_overrides = {
            "d3d9": "n,b",
            "d3d10core": "n,b",
            "d3d11": "n,b",
            "dxgi": "n,b",
        }
    elif selected_backend == GraphicsBackend.VKD3D_MOLTENVK.value:
        dll_overrides = {
            "d3d12": "n,b",
            "dxgi": "n,b",
        }
    elif selected_backend == GraphicsBackend.D3DMETAL.value:
        dll_overrides = {
            "d3d10core": "n,b",
            "d3d11": "n,b",
            "d3d12": "n,b",
            "dxgi": "n,b",
            "winemetal": "n,b",
        }

    env: dict[str, str] = {
        "MACRUNNER_GRAPHICS_BACKEND": selected_backend,
    }
    if dll_overrides:
        env["WINEDLLOVERRIDES"] = _join_overrides(dll_overrides)

    return GraphicsPlan(
        requested_api=api,
        selected_backend=selected_backend,
        fallback_backend=fallback_backend,
        shader_cache_enabled=bool(graphics.get("shader_cache", True)),
        backend_available=available,
        backend_path=str(backend_path) if backend_path else None,
        env=env,
        dll_overrides=dll_overrides,
        warnings=warnings,
        errors=errors,
    )
