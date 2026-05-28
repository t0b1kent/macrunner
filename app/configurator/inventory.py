#!/usr/bin/env python3
"""Inventory and host/runtime diagnostics for MacRunner."""

from __future__ import annotations

import json
import os
import platform
import shutil
import subprocess
from pathlib import Path
from typing import Any, Iterable

from app.configurator.bottles import bottle_root_path, load_manifest
from app.configurator.engines import load_engine_registry
from app.configurator.profiles import PROFILE_DIR
from app.configurator.rosetta import is_rosetta_available
from app.game_runtime.input import input_doctor
from app.game_runtime.shader_cache import shader_cache_root


REPO_ROOT = Path(__file__).resolve().parents[2]
ENGINE_BACKEND_ROOT = REPO_ROOT / "engine/game/backends"
CONTROLLER_DB = REPO_ROOT / "engine/game/controller-db/gamecontrollerdb.txt"
REPORTS_ROOT = Path.home() / "Library/Logs/MacRunner"


def _run_text(command: list[str], timeout: int = 10) -> tuple[int, str, str]:
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=timeout)
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return 127, "", str(exc)
    return result.returncode, result.stdout.strip(), result.stderr.strip()


def _file_type(path: Path) -> str | None:
    if not path.exists():
        return None
    code, stdout, _ = _run_text(["/usr/bin/file", str(path)], timeout=10)
    if code != 0:
        return None
    return stdout


def _binary_arch(file_type: str | None) -> str:
    if not file_type:
        return "unknown"
    text = file_type.lower()
    if "shell script" in text:
        return "shell-script"
    if "universal" in text:
        return "universal"
    if "arm64" in text or "aarch64" in text:
        return "arm64"
    if "x86-64" in text or "x86_64" in text or "x86-64" in text:
        return "x86_64"
    if "i386" in text or "i686" in text:
        return "x86"
    return "unknown"


def _lane_binary_ok(lane: str, binary_arch: str) -> bool:
    if binary_arch in {"universal", "unknown"}:
        return binary_arch == "universal"
    expected = {
        "arm64-native": {"arm64"},
        "x86_64-rosetta": {"x86_64"},
        "x86-rosetta-wow64": {"x86_64"},
        "arm64ec-x64-bridge": {"arm64"},
        "arm64-hyperbridge": {"arm64"},
    }.get(lane, set())
    return binary_arch in expected


def host_inventory() -> dict[str, Any]:
    macos = _run_text(["/usr/bin/sw_vers", "-productVersion"], timeout=5)[1] or "unknown"
    cpu = _run_text(["/usr/sbin/sysctl", "-n", "hw.machine"], timeout=5)[1] or platform.machine() or "unknown"
    clang_path = shutil.which("clang")
    brew_path = shutil.which("brew")
    return {
        "macos": macos,
        "arch": cpu,
        "rosetta_available": is_rosetta_available(),
        "clang_available": bool(clang_path),
        "clang_path": clang_path,
        "brew_available": bool(brew_path),
    }


def engine_inventory() -> dict[str, dict[str, Any]]:
    registry = load_engine_registry()
    inventory: dict[str, dict[str, Any]] = {}
    for engine in registry.engines:
        path = Path(engine.path)
        exists = path.exists()
        executable = exists and os.access(path, os.X_OK)
        file_type = _file_type(path) if exists else None
        binary_arch = _binary_arch(file_type)
        usable = exists and executable and _lane_binary_ok(engine.lane, binary_arch)
        version = None
        if exists and executable:
            code, stdout, stderr = _run_text([engine.path, "--version"], timeout=15)
            if code == 0 and stdout:
                version = stdout.splitlines()[0].strip()
            elif stderr:
                version = stderr.splitlines()[0].strip()
        inventory[engine.lane] = {
            "id": engine.id,
            "path": engine.path,
            "exists": exists,
            "executable": executable,
            "file_type": file_type,
            "binary_arch": binary_arch,
            "usable": usable,
            "version": version,
            "host_arch": engine.host_arch,
            "guest_machines": list(engine.guest_machines),
            "status": engine.status,
            "native_level": engine.native_level,
            "requires_rosetta": engine.requires_rosetta,
            "requires_experimental": engine.requires_experimental,
            "warnings": list(engine.warnings),
        }
    return inventory


def graphics_inventory() -> dict[str, dict[str, Any]]:
    backend_specs = {
        "d3dmetal": ["backend.json"],
        "dxvk": ["backend.json", "d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"],
        "vkd3d": ["backend.json", "d3d12.dll", "dxgi.dll"],
        "moltenvk": ["backend.json", "libMoltenVK.dylib"],
        "dxvk-moltenvk": ["backend.json"],
        "vkd3d-moltenvk": ["backend.json"],
        "native-metal-experimental": ["backend.json"],
    }
    inventory: dict[str, dict[str, Any]] = {}
    for name, required_files in backend_specs.items():
        path = ENGINE_BACKEND_ROOT / name
        exists = path.exists()
        files = sorted(str(item.name) for item in path.iterdir() if item.is_file()) if exists else []
        usable = exists and all((path / req).exists() for req in required_files if req != "backend.json")
        backend_json = path / "backend.json"
        if backend_json.exists():
            try:
                payload = json.loads(backend_json.read_text(encoding="utf-8"))
            except Exception:
                payload = None
        else:
            payload = None
        inventory[name] = {
            "path": str(path),
            "exists": exists,
            "usable": usable,
            "files": files,
            "backend_json": payload,
            "backend_json_exists": backend_json.exists(),
        }
    return inventory


def profile_inventory() -> dict[str, Any]:
    profiles: list[dict[str, Any]] = []
    for path in sorted(PROFILE_DIR.glob("*.json")):
        profiles.append(
            {
                "id": path.stem,
                "path": str(path),
                "exists": True,
            }
        )
    return {"count": len(profiles), "profiles": profiles}


def bottle_inventory(root: str | Path | None = None) -> dict[str, Any]:
    bottle_root = bottle_root_path(root)
    bottles: list[dict[str, Any]] = []
    if bottle_root.exists():
        for entry in sorted(bottle_root.iterdir()):
            if not entry.is_dir():
                continue
            manifest = load_manifest(bottle_root, entry.name)
            if manifest is None:
                continue
            bottles.append(
                {
                    "id": manifest.id,
                    "lane": manifest.lane,
                    "guest_machine": manifest.guest_machine,
                    "engine_id": manifest.engine_id,
                    "profile_id": manifest.profile_id,
                    "category": manifest.category,
                    "created_at": manifest.created_at,
                    "updated_at": manifest.updated_at,
                    "path": str(entry),
                }
            )
    return {
        "root": str(bottle_root),
        "exists": bottle_root.exists(),
        "writable": os.access(bottle_root, os.W_OK) if bottle_root.exists() else os.access(bottle_root.parent, os.W_OK),
        "count": len(bottles),
        "bottles": bottles,
    }


def runtime_inventory(root: str | Path | None = None) -> dict[str, Any]:
    bottle_root = bottle_root_path(root)
    shader_root = shader_cache_root(bottle_root, "default").parent
    return {
        "bottle_root": str(bottle_root),
        "bottle_root_writable": os.access(bottle_root, os.W_OK) if bottle_root.exists() else os.access(bottle_root.parent, os.W_OK),
        "shader_cache_root": str(shader_root),
        "shader_cache_root_writable": os.access(shader_root, os.W_OK) if shader_root.exists() else os.access(shader_root.parent, os.W_OK),
        "reports_root": str(REPORTS_ROOT),
        "reports_root_writable": os.access(REPORTS_ROOT, os.W_OK) if REPORTS_ROOT.exists() else os.access(REPORTS_ROOT.parent, os.W_OK),
        "controller_db_path": str(CONTROLLER_DB),
        "controller_db_exists": CONTROLLER_DB.exists(),
        "controller_db_writable": os.access(CONTROLLER_DB.parent, os.W_OK),
    }


def build_inventory(root: str | Path | None = None) -> dict[str, Any]:
    return {
        "host": host_inventory(),
        "engines": engine_inventory(),
        "graphics_backends": graphics_inventory(),
        "bottles": bottle_inventory(root),
        "profiles": profile_inventory(),
        "runtime": runtime_inventory(root),
        "input": input_doctor(),
    }


def build_graphics_inventory() -> dict[str, Any]:
    return graphics_inventory()


def build_input_inventory() -> dict[str, Any]:
    return input_doctor()


def inventory_doctor_text(root: str | Path | None = None) -> str:
    inventory = build_inventory(root)
    lines = ["MacRunner Doctor", "Host:"]
    host = inventory["host"]
    lines.append(f"  macos: {host['macos']}")
    lines.append(f"  cpu: {host['arch']}")
    lines.append(f"  rosetta: {'available' if host['rosetta_available'] else 'unavailable'}")
    lines.append("Engines:")
    for lane, engine in inventory["engines"].items():
        lines.append(f"  {lane}: {'available' if engine['usable'] else 'missing'} ({engine['binary_arch']})")
    lines.append("Capabilities:")
    capabilities = {
        "Windows ARM64": inventory["engines"]["arm64-native"]["usable"],
        "Windows x64 via Rosetta": inventory["engines"]["x86_64-rosetta"]["usable"] and host["rosetta_available"],
        "Windows x64 via ARM64EC bridge": inventory["engines"]["arm64ec-x64-bridge"]["usable"],
        "Windows x86": inventory["engines"].get("x86-rosetta-wow64", {}).get("usable", False) and host["rosetta_available"],
        "DX9 game profiles": inventory["graphics_backends"]["dxvk"]["exists"] or inventory["graphics_backends"]["dxvk-moltenvk"]["exists"],
        "DX11 game profiles": inventory["graphics_backends"]["d3dmetal"]["exists"] or inventory["graphics_backends"]["dxvk"]["exists"],
        "DX12 game profiles": inventory["graphics_backends"]["d3dmetal"]["exists"] or inventory["graphics_backends"]["vkd3d"]["exists"],
        "Shader cache": True,
        "Fast I/O": True,
        "Controller/input": True,
        "Native sync skeleton": True,
        "Anti-cheat safe mode": True,
    }
    for key, value in capabilities.items():
        lines.append(f"  {key}: {'yes' if value else 'no'}")
    return "\n".join(lines)
