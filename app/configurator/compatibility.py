#!/usr/bin/env python3
"""Compatibility decision engine and CLI entrypoint."""

from __future__ import annotations

import argparse
import glob
import json
import os
import signal
import shutil
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field, is_dataclass, replace
from pathlib import Path
from typing import Any, Sequence

from app.configurator.ai.error_parser import analyze_smoke_stderr
from app.configurator.ai.tuner import (
    format_tuning_report,
    run_tuning_session,
)
from app.configurator.anticheat import AntiCheatPlan, build_anticheat_plan
from app.configurator.bottles import (
    BottleManifest,
    BottleMismatchError,
    bottle_root_path,
    ensure_bottle,
    load_manifest,
)
from app.configurator.engines import (
    DEFAULT_ENGINE_REGISTRY,
    EngineDefinition,
    EngineRegistry,
    EngineRegistryError,
    load_engine_registry,
    required_engine,
)
from app.configurator.pe import PEMachine, PEFormatError, detect_pe_machine
from app.configurator.inventory import (
    ENGINE_BACKEND_ROOT,
    build_graphics_inventory,
    build_input_inventory,
    build_inventory,
    inventory_doctor_text,
    _run_text,
)
from app.configurator.profiles import DEFAULT_PROFILE_ID, ProgramProfile, load_profile, normalize_profile_arch, resolve_profile
from app.configurator.reports import dump_json, write_json
from app.configurator.rosetta import is_rosetta_available, rosetta_install_hint


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


@dataclass(frozen=True)
class CompatibilityPlan:
    exe_path: str
    pe_machine: str
    selected_lane: str
    native_level: str
    engine_id: str
    engine_path: str | None
    profile_id: str | None
    bottle_id: str
    bottle_root: str
    wineprefix: str
    rosetta_required: bool
    rosetta_available: bool
    experimental: bool
    native_score: int
    compatibility_score: int
    reason: str
    command: list[str]
    env: dict[str, str]
    game_runtime: GameRuntimePlan | None = None
    warnings: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

    @property
    def prefix(self) -> str:
        suffix = self.selected_lane
        if suffix == "arm64-native":
            suffix = "arm64"
        elif not suffix or suffix == "unknown":
            suffix = self.pe_machine or "unknown"
        return str(Path(self.wineprefix) / f"prefix-{suffix}")

    @property
    def target_arch(self) -> str:
        return self.pe_machine

    @property
    def env_overrides(self) -> dict[str, str]:
        return self.env


def _auto_detect_profile(exe_path: Path) -> tuple[ProgramProfile, list[str], list[str]]:
    """Auto-detect profile overrides from PE analysis using ML predictor."""
    warnings: list[str] = []
    errors: list[str] = []
    try:
        from app.configurator.ai.pe_analyzer import analyze_pe
        from app.configurator.ai.pe_analyzer.features import build_feature_vector
        from app.configurator.ai.model import ProfilePredictor, _pe_features_to_flat

        pe_result = analyze_pe(exe_path)
        pe_features = build_feature_vector(pe_result)
        flat_features = _pe_features_to_flat(pe_features)

        predictor = ProfilePredictor(strategy="xgboost")
        result = predictor.predict(flat_features)
        profile_dict = predictor.predict_profile_dict(flat_features)

        base = resolve_profile(None)
        category = profile_dict.get("category", base.category)
        default_lane = profile_dict.get("default_lane", base.default_lane)

        graphics = dict(base.graphics or {})
        if "graphics" in profile_dict:
            graphics.update(profile_dict["graphics"])

        performance = dict(base.performance or {})
        if "performance" in profile_dict:
            performance.update(profile_dict["performance"])

        anti_cheat = dict(base.anti_cheat or {})
        if "anti_cheat" in profile_dict:
            anti_cheat.update(profile_dict["anti_cheat"])

        bottle_policy = dict(base.bottle_policy or {})
        if "bottle_policy" in profile_dict:
            bottle_policy.update(profile_dict["bottle_policy"])

        wine_settings = dict(base.wine_settings or {})
        if "wine_settings" in profile_dict:
            wine_settings.update(profile_dict["wine_settings"])

        required_dlls: tuple[str, ...] = base.required_dlls
        if "required_dlls" in profile_dict:
            rd = profile_dict["required_dlls"]
            required_dlls = tuple(rd) if isinstance(rd, (list, tuple)) else (str(rd),)

        profile = replace(
            base,
            category=category,
            default_lane=default_lane,
            graphics=graphics,
            performance=performance,
            anti_cheat=anti_cheat,
            bottle_policy=bottle_policy,
            wine_settings=wine_settings,
            required_dlls=required_dlls,
        )
        warnings.append(
            f"Auto-detected profile: {result.predicted_category} "
            f"(confidence: {result.confidence}, strategy: {result.strategy})"
        )
        return profile, warnings, errors
    except ImportError as exc:
        errors.append(f"auto-profile detection unavailable: {exc}")
        return resolve_profile(None), warnings, errors
    except Exception as exc:
        errors.append(f"auto-profile detection failed: {exc}")
        return resolve_profile(None), warnings, errors


def _profile_env(profile: ProgramProfile) -> dict[str, str]:
    env: dict[str, str] = {}
    env.update({str(k): str(v) for k, v in (profile.env or {}).items()})
    env.update({str(k): str(v) for k, v in (profile.wine_settings or {}).items() if str(k) not in {"locale", "winedebug"}})
    locale = profile.wine_settings.get("locale")
    if locale:
        env.setdefault("LANG", locale)
        env.setdefault("LC_ALL", locale)
    winedebug = profile.wine_settings.get("winedebug")
    if winedebug:
        env.setdefault("WINEDEBUG", winedebug)
    if profile.env.get("WINEDEBUG"):
        env["WINEDEBUG"] = profile.env["WINEDEBUG"]
    return env


def _profile_args(profile: ProgramProfile) -> list[str]:
    return list(profile.args or ())


def _engine_lane_to_execution(lane: str) -> str:
    if lane in {"x86_64-rosetta", "x86-rosetta-wow64"}:
        return "rosetta"
    if lane == "arm64ec-x64-bridge":
        return "bridge"
    if lane == "arm64-hyperbridge":
        return "hyperbridge"
    return "native"


def _select_lane(
    machine: PEMachine,
    *,
    experimental: bool,
    prefer_native: bool,
    prefer_compatibility: bool,
    user_lane_override: str | None,
    registry: EngineRegistry,
) -> tuple[str, list[str], list[str]]:
    warnings: list[str] = []
    errors: list[str] = []
    if user_lane_override:
        engine = registry.by_lane(user_lane_override)
        if engine is None:
            errors.append(f"unknown lane override: {user_lane_override}")
            return "", warnings, errors
        if machine != PEMachine.UNKNOWN and machine.value not in engine.guest_machines:
            supported = ", ".join(engine.guest_machines) or "none"
            errors.append(
                f"lane override {user_lane_override} does not support PE machine "
                f"{machine.value}; supported: {supported}"
            )
            return "", warnings, errors
        return user_lane_override, warnings, errors

    hyperbridge = registry.by_lane("arm64-hyperbridge")
    has_hyperbridge = hyperbridge is not None and hyperbridge.available

    if machine == PEMachine.ARM64:
        return "arm64-native", warnings, errors
    if machine == PEMachine.X86:
        return "x86-rosetta-wow64", warnings, errors
    if machine == PEMachine.ARM64EC:
        if not experimental:
            errors.append("ARM64EC requires --experimental")
            return "", warnings, errors
        return "arm64ec-x64-bridge", warnings, errors
    if machine == PEMachine.ARM64X:
        if not experimental:
            errors.append("ARM64X requires --experimental")
            return "", warnings, errors
        warnings.append("ARM64X slice selection is not implemented yet; using bridge lane.")
        return "arm64ec-x64-bridge", warnings, errors

    if machine == PEMachine.X86_64:
        if has_hyperbridge and not prefer_compatibility:
            return "arm64-hyperbridge", warnings, errors
        bridge = registry.by_lane("arm64ec-x64-bridge")
        if experimental and prefer_native and bridge and bridge.available and not prefer_compatibility:
            return "arm64ec-x64-bridge", warnings, errors
        return "x86_64-rosetta", warnings, errors

    errors.append(f"unsupported PE machine: {machine.value}")
    return "", warnings, errors


def _select_engine(registry: EngineRegistry, lane: str) -> EngineDefinition:
    return required_engine(registry, lane)


def _lane_requires_rosetta(lane: str) -> bool:
    return lane in {"x86_64-rosetta", "x86-rosetta-wow64"}


def _configure_wine_env_for_lane(env: dict[str, str], lane: str, engine: EngineDefinition | None) -> None:
    env.setdefault("WINE_MONO_NO_INSTALL", "1")
    if _lane_requires_rosetta(lane):
        env["ROSETTA_ADVERTISE_AVX"] = "1"
        env["WINEARCH"] = "win64"
    if lane == "x86-rosetta-wow64" and engine:
        loader_path = Path(engine.path)
        wine_lib = loader_path.parents[1]
        engine_root = wine_lib.parents[1]
        env["WINELOADER"] = str(loader_path)
        env["WINEDLLPATH"] = str(wine_lib)
        env["WINESERVER"] = str(engine_root / "bin" / "wineserver")
    if lane == "arm64-hyperbridge" and engine:
        loader_path = Path(engine.path)
        engine_root = loader_path.parents[1]
        env.setdefault("MACRUNNER_HB_X64_LOADER", "1")
        env.setdefault("MACRUNNER_WINE_DIST", str(engine_root))
        env["WINELOADER"] = str(loader_path)
        env["WINESERVER"] = str(engine_root / "bin" / "wineserver")
        env["WINEDLLPATH"] = str(engine_root / "lib" / "wine")


def _append_path_env(env: dict[str, str], name: str, paths: Sequence[Path]) -> None:
    entries = [str(path) for path in paths if path.exists()]
    if not entries:
        return
    existing = [item for item in env.get(name, "").split(os.pathsep) if item]
    merged: list[str] = []
    for item in [*entries, *existing]:
        if item not in merged:
            merged.append(item)
    env[name] = os.pathsep.join(merged)


def _merge_winedll_overrides(env: dict[str, str], overrides: dict[str, str]) -> None:
    merged: dict[str, str] = {}
    for item in env.get("WINEDLLOVERRIDES", "").split(";"):
        if not item or "=" not in item:
            continue
        key, value = item.split("=", 1)
        merged[key.strip().lower()] = value.strip()
    merged.update(overrides)
    env["WINEDLLOVERRIDES"] = ";".join(f"{key}={value}" for key, value in merged.items())


def _dxmt_windows_arch(selected_machine: str | None) -> str:
    key = (selected_machine or "").lower()
    if key in {"arm64", "aarch64", "aa64"}:
        return "aarch64-windows"
    return "x86_64-windows"


def _overlay_root_for_runtime(env: dict[str, str], backend_name: str) -> Path:
    run_dir = env.get("MACRUNNER_RUN_DIR")
    if run_dir:
        return Path(run_dir) / f"{backend_name}-runtime-overlay"
    prefix = env.get("WINEPREFIX")
    if prefix:
        return Path(prefix) / f"{backend_name}-runtime-overlay"
    return Path("/tmp") / f"{backend_name}-runtime-overlay"


def _install_overlay_dll(overlay_dir: Path, source: Path) -> Path:
    overlay_dir.mkdir(parents=True, exist_ok=True)
    destination = overlay_dir / source.name
    shutil.copy2(source, destination)
    return destination


def _apply_graphics_runtime_env(env: dict[str, str], graphics_backend: str | None) -> None:
    force_dxmt = env.get("MACRUNNER_FORCE_DXMT") == "1" or os.environ.get("MACRUNNER_FORCE_DXMT") == "1"
    if force_dxmt:
        env["MACRUNNER_FORCE_DXMT"] = "1"
        env["MACRUNNER_GRAPHICS_BACKEND"] = "d3dmetal"
        graphics_backend = "d3dmetal"

    root = Path(env.get("MACRUNNER_ROOT") or Path(__file__).resolve().parents[2])
    selected_machine = env.get("MACRUNNER_SELECTED_MACHINE")
    if graphics_backend == "d3dmetal":
        dxmt_root = root / "engine" / "graphics" / "dist" / "dxmt"
        dxmt_windows = dxmt_root / _dxmt_windows_arch(selected_machine)
        dxmt_unix = dxmt_root / "aarch64-unix"
        _append_path_env(env, "WINEDLLPATH", [dxmt_windows, dxmt_unix])
        _merge_winedll_overrides(
            env,
            {
                "d3d10core": "n,b",
                "d3d11": "n,b",
                "d3d12": "n,b",
                "dxgi": "n,b",
                "winemetal": "n,b",
            },
        )
    elif graphics_backend == "dxvk-moltenvk":
        dxvk_root = root / "engine" / "graphics" / "dist" / "dxvk"
        dxvk_windows = dxvk_root / _dxmt_windows_arch(selected_machine)
        d3d9_dll = dxvk_windows / "d3d9.dll"
        if d3d9_dll.exists():
            overlay_root = _overlay_root_for_runtime(env, "dxvk")
            overlay_machine_dir = overlay_root / _dxmt_windows_arch(selected_machine)
            _install_overlay_dll(overlay_machine_dir, d3d9_dll)
            _append_path_env(env, "WINEDLLPATH", [overlay_machine_dir])
            env["WINESYSTEMDLLPATH"] = str(overlay_machine_dir)
        _merge_winedll_overrides(
            env,
            {
                "d3d9": "n,b",
            },
        )


def _engine_command(engine: EngineDefinition, exe_path: str, args: Sequence[str], lane: str) -> list[str]:
    if lane in {"x86_64-rosetta", "x86-rosetta-wow64"}:
        return [engine.path, exe_path, *args]
    if lane == "arm64ec-x64-bridge":
        return [engine.path, exe_path, *args]
    return [engine.path, exe_path, *args]


def _lane_guest_machine(lane: str) -> str:
    if lane == "arm64-native":
        return "arm64"
    if lane == "x86_64-rosetta":
        return "x86_64"
    if lane == "x86-rosetta-wow64":
        return "x86"
    if lane == "arm64ec-x64-bridge":
        return "arm64ec"
    if lane == "arm64-hyperbridge":
        return "x86_64"
    return "unknown"


def _default_bottle_id_for_plan(profile: ProgramProfile, lane: str, machine: str) -> str:
    if profile.id == DEFAULT_PROFILE_ID:
        if lane == "arm64-native":
            return "generic-arm64"
        if lane == "x86_64-rosetta":
            return "generic-x86_64-rosetta"
        if lane == "x86-rosetta-wow64":
            return "generic-x86-rosetta-wow64"
        if lane == "arm64ec-x64-bridge":
            return "generic-arm64ec-bridge"
        return f"generic-{machine or 'unknown'}"
    return (profile.bottle_policy or {}).get("default_bottle_id") or profile.id or f"generic-{machine or 'unknown'}"


def _build_bottle_and_runtime(
    *,
    profile: ProgramProfile,
    registry: EngineRegistry,
    engine: EngineDefinition | None,
    lane: str,
    guest_machine: str,
    bottle_id: str,
    bottle_root_path_resolved: Path,
    experimental: bool,
    allow_bottle_migration: bool,
    materialize: bool,
    warnings: list[str],
    errors: list[str],
) -> tuple[BottleManifest, GameRuntimePlan | None]:
    bottle_category = profile.category or "utility"
    host_execution = _engine_lane_to_execution(lane) if lane else "native"
    native_level = engine.native_level if engine else "unknown"
    materialize_effective = materialize and not errors
    if materialize_effective:
        bottle = ensure_bottle(
            bottle_root_path_resolved,
            bottle_id=bottle_id,
            name=profile.name,
            category=bottle_category,
            lane=lane or "unknown",
            guest_machine=guest_machine,
            host_execution=host_execution,
            native_level=native_level,
            engine_id=engine.id if engine else "unknown",
            profile_id=profile.id,
            features={
                "shader_cache": bool((profile.graphics or {}).get("shader_cache", True)),
                "controller_profile": True,
                "fast_io": bool((profile.performance or {}).get("fast_io", False)),
                "ntsync_like": False,
            },
            allow_migration=allow_bottle_migration,
        )
    else:
        bottle = load_manifest(bottle_root_path_resolved, bottle_id) or BottleManifest(
            schema_version=2,
            id=bottle_id,
            name=profile.name,
            category=bottle_category,
            lane=lane or "unknown",
            guest_machine=guest_machine,
            host_execution=host_execution,
            native_level=native_level,
            engine_id=engine.id if engine else "unknown",
            profile_id=profile.id,
            created_at="1970-01-01T00:00:00Z",
            updated_at="1970-01-01T00:00:00Z",
            features={
                "shader_cache": bool((profile.graphics or {}).get("shader_cache", True)),
                "controller_profile": True,
                "fast_io": bool((profile.performance or {}).get("fast_io", False)),
                "ntsync_like": False,
            },
        )

    pre_bottle_error_count = len(errors)
    if bottle.lane != lane and lane:
        errors.append(f"bottle lane mismatch: {bottle.lane} != {lane}")
    if bottle.guest_machine != guest_machine:
        errors.append(f"bottle arch mismatch: {bottle.guest_machine} != {guest_machine}")

    game_runtime = None
    if len(errors) == pre_bottle_error_count:
        game_runtime = _build_game_runtime_plan(
            profile,
            bottle,
            bottle_root_path_resolved,
            warnings,
            errors,
            materialize=materialize_effective,
        )
    return bottle, game_runtime


def _build_game_runtime_plan(
    profile: ProgramProfile,
    bottle: BottleManifest,
    bottle_root: Path,
    warnings: list[str],
    errors: list[str],
    *,
    materialize: bool,
) -> GameRuntimePlan:
    from app.game_runtime.fast_io import build_fast_io_plan
    from app.game_runtime.graphics import build_graphics_plan
    from app.game_runtime.input import build_input_plan
    from app.game_runtime.shader_cache import build_shader_cache_plan
    from app.game_runtime.threading import build_threading_plan
    from app.game_runtime.profiles import build_game_runtime_plan

    graphics_plan = build_graphics_plan(profile=profile, bottle=bottle)
    shader_cache_plan = build_shader_cache_plan(
        bottle_root=bottle_root,
        bottle_id=bottle.id,
        backend=graphics_plan.selected_backend,
        enabled=bool((profile.graphics or {}).get("shader_cache", True)),
        materialize=materialize,
    )
    input_plan = build_input_plan(profile=profile, bottle=bottle)
    fast_io_plan = build_fast_io_plan(profile=profile, bottle=bottle, bottle_root=bottle_root, bottle_id=bottle.id, materialize=materialize)
    threading_plan = build_threading_plan(profile=profile)
    game_runtime = build_game_runtime_plan(
        profile=profile,
        bottle=bottle,
        graphics_plan=graphics_plan,
        shader_cache_plan=shader_cache_plan,
        input_plan=input_plan,
        fast_io_plan=fast_io_plan,
        threading_plan=threading_plan,
    )
    warnings.extend(graphics_plan.warnings)
    errors.extend(graphics_plan.errors)
    warnings.extend(shader_cache_plan.warnings)
    warnings.extend(input_plan.warnings)
    warnings.extend(fast_io_plan.warnings)
    warnings.extend(threading_plan.warnings)
    return game_runtime


def build_compatibility_plan(
    exe_path: str,
    profile_id: str | None = None,
    user_lane_override: str | None = None,
    experimental: bool = False,
    prefer_native: bool = True,
    prefer_compatibility: bool = False,
    bottle_id: str | None = None,
    new_bottle: bool = False,
    bottle_root: str | Path | None = None,
    allow_bottle_migration: bool = False,
    extra_args: Sequence[str] | None = None,
    materialize: bool = False,
) -> CompatibilityPlan:
    warnings: list[str] = []
    errors: list[str] = []
    target = Path(exe_path).expanduser().resolve()
    if not target.exists():
        raise FileNotFoundError(f"target not found: {target}")

    if profile_id is None:
        profile, auto_warnings, auto_errors = _auto_detect_profile(target)
        warnings.extend(auto_warnings)
        errors.extend(auto_errors)
    else:
        profile = resolve_profile(profile_id)
    registry = load_engine_registry()
    machine = detect_pe_machine(target)
    lane, lane_warnings, lane_errors = _select_lane(
        machine,
        experimental=experimental,
        prefer_native=prefer_native,
        prefer_compatibility=prefer_compatibility,
        user_lane_override=user_lane_override,
        registry=registry,
    )
    warnings.extend(lane_warnings)
    errors.extend(lane_errors)
    if errors:
        engine = None
    else:
        try:
            engine = _select_engine(registry, lane)
        except EngineRegistryError as exc:
            errors.append(str(exc))
            engine = None

    rosetta_available = is_rosetta_available()
    rosetta_required = _lane_requires_rosetta(lane)
    if rosetta_required and not rosetta_available:
        errors.append(rosetta_install_hint())

    bottle_root_path_resolved = bottle_root_path(bottle_root)
    bottle_id_resolved = bottle_id or _default_bottle_id_for_plan(profile, lane, machine.value)
    bottle_category = profile.category or "utility"
    bottle_guest_machine = machine.value if machine != PEMachine.UNKNOWN else "unknown"
    host_execution = _engine_lane_to_execution(lane) if lane else "native"
    native_level = engine.native_level if engine else "unknown"
    materialize_effective = materialize and not errors
    if materialize_effective:
        bottle = ensure_bottle(
            bottle_root_path_resolved,
            bottle_id=bottle_id_resolved,
            name=profile.name,
            category=bottle_category,
            lane=lane or "unknown",
            guest_machine=bottle_guest_machine,
            host_execution=host_execution,
            native_level=native_level,
            engine_id=engine.id if engine else "unknown",
            profile_id=profile.id,
            features={
                "shader_cache": bool((profile.graphics or {}).get("shader_cache", True)),
                "controller_profile": True,
                "fast_io": bool((profile.performance or {}).get("fast_io", False)),
                "ntsync_like": False,
            },
            allow_migration=allow_bottle_migration,
        )
    else:
        bottle = load_manifest(bottle_root_path_resolved, bottle_id_resolved) or BottleManifest(
            schema_version=2,
            id=bottle_id_resolved,
            name=profile.name,
            category=bottle_category,
            lane=lane or "unknown",
            guest_machine=bottle_guest_machine,
            host_execution=host_execution,
            native_level=native_level,
            engine_id=engine.id if engine else "unknown",
            profile_id=profile.id,
            created_at="1970-01-01T00:00:00Z",
            updated_at="1970-01-01T00:00:00Z",
            features={
                "shader_cache": bool((profile.graphics or {}).get("shader_cache", True)),
                "controller_profile": True,
                "fast_io": bool((profile.performance or {}).get("fast_io", False)),
                "ntsync_like": False,
            },
        )

    pre_bottle_error_count = len(errors)
    if bottle.lane != lane and lane:
        errors.append(f"bottle lane mismatch: {bottle.lane} != {lane}")
    if bottle.guest_machine != bottle_guest_machine:
        errors.append(f"bottle arch mismatch: {bottle.guest_machine} != {bottle_guest_machine}")

    game_runtime = None
    if len(errors) == pre_bottle_error_count:
        game_runtime = _build_game_runtime_plan(
            profile,
            bottle,
            bottle_root_path_resolved,
            warnings,
            errors,
            materialize=materialize_effective,
        )

    env = os.environ.copy()
    env.update(_profile_env(profile))
    target_dir = str(target.parent)
    env["MACRUNNER_EXE_DIR"] = target_dir
    env["PATH"] = target_dir + os.pathsep + env.get("PATH", "")
    env["WINEPREFIX"] = str(bottle_root_path_resolved / bottle.id)
    tmpdir = bottle_root_path_resolved / bottle.id / "tmp"
    if materialize_effective:
        tmpdir.mkdir(parents=True, exist_ok=True)
    env["TMPDIR"] = str(tmpdir)
    env["MACRUNNER_PROFILE_ID"] = profile.id
    env["MACRUNNER_BOTTLE_ID"] = bottle.id
    env["MACRUNNER_LANE"] = lane
    env["MACRUNNER_NATIVE_LEVEL"] = native_level
    env["MACRUNNER_SELECTED_MACHINE"] = machine.value
    env["MACRUNNER_ENGINE_ID"] = engine.id if engine else "unknown"
    _configure_wine_env_for_lane(env, lane, engine)

    if game_runtime:
        env["MACRUNNER_GRAPHICS_BACKEND"] = str(game_runtime.graphics_backend or "")
        env["MACRUNNER_SHADER_CACHE_ROOT"] = str(game_runtime.shader_cache_root or "")
        env["MACRUNNER_INPUT_MODE"] = str(game_runtime.input_mode or "")
        env["MACRUNNER_FAST_IO"] = "1" if game_runtime.fast_io_enabled else "0"
        env["MACRUNNER_THREADING_MODE"] = str(game_runtime.threading_mode or "")
        env["MACRUNNER_ANTICHEAT_MODE"] = str(game_runtime.anti_cheat_mode or "")
        _apply_graphics_runtime_env(env, game_runtime.graphics_backend)
    else:
        _apply_graphics_runtime_env(env, None)

    if engine and _lane_requires_rosetta(engine.lane):
        command = [str(Path(__file__).resolve().parents[2] / "scripts/run-with-rosetta.sh"), engine.path, str(target), *(extra_args or ())]
    else:
        command = [engine.path if engine else "unknown-engine", str(target), *(extra_args or ())]

    if profile.args:
        command.extend(profile.args)

    if engine is None:
        native_score = 0
        compatibility_score = 0
    elif lane == "arm64-native":
        native_score = 100
        compatibility_score = 70
    elif lane == "arm64ec-x64-bridge":
        native_score = 85
        compatibility_score = 80
    else:
        native_score = 60
        compatibility_score = 95

    reason = (
        "ARM64 PE routed to native lane"
        if machine == PEMachine.ARM64
        else "x86_64 routed through Rosetta compatibility lane"
        if lane == "x86_64-rosetta"
        else "x86 routed through Rosetta WoW64 compatibility lane"
        if lane == "x86-rosetta-wow64"
        else "experimental ARM64EC/x64 bridge lane selected"
        if lane == "arm64ec-x64-bridge"
        else "x86/x64 routed through native arm64 HyperBridge lane"
        if lane == "arm64-hyperbridge"
        else "unsupported machine"
    )

    return CompatibilityPlan(
        exe_path=str(target),
        pe_machine=machine.value,
        selected_lane=lane or "unknown",
        native_level=native_level,
        engine_id=engine.id if engine else "unknown",
        engine_path=engine.path if engine else None,
        profile_id=profile.id,
        bottle_id=bottle.id,
        bottle_root=str(bottle_root_path_resolved),
        wineprefix=str(bottle_root_path_resolved / bottle.id),
        rosetta_required=rosetta_required,
        rosetta_available=rosetta_available,
        experimental=experimental,
        native_score=native_score,
        compatibility_score=compatibility_score,
        reason=reason,
        command=command,
        env=env,
        game_runtime=game_runtime,
        warnings=warnings,
        errors=errors,
    )


def build_engine_command_plan(
    command_args: Sequence[str],
    *,
    lane: str,
    profile_id: str | None = None,
    bottle_id: str | None = None,
    bottle_root: str | Path | None = None,
    experimental: bool = False,
    prefer_native: bool = True,
    prefer_compatibility: bool = False,
    allow_bottle_migration: bool = False,
    materialize: bool = False,
) -> CompatibilityPlan:
    warnings: list[str] = []
    errors: list[str] = []
    if not command_args:
        raise ValueError("engine command requires at least one argument")

    profile = resolve_profile(profile_id)
    registry = load_engine_registry()
    engine = registry.by_lane(lane)
    if engine is None:
        errors.append(f"unknown lane: {lane}")
    else:
        probe = build_inventory(bottle_root)["engines"].get(lane, {})
        if not probe.get("usable", False):
            errors.append(f"engine lane {lane} is not usable yet: {probe.get('file_type') or 'missing engine binary'}")

    rosetta_available = is_rosetta_available()
    rosetta_required = _lane_requires_rosetta(lane)
    if rosetta_required and not rosetta_available:
        errors.append(rosetta_install_hint())

    bottle_root_path_resolved = bottle_root_path(bottle_root)
    guest_machine = _lane_guest_machine(lane)
    bottle_id_resolved = bottle_id or _default_bottle_id_for_plan(profile, lane, guest_machine)
    bottle, game_runtime = _build_bottle_and_runtime(
        profile=profile,
        registry=registry,
        engine=engine,
        lane=lane,
        guest_machine=guest_machine,
        bottle_id=bottle_id_resolved,
        bottle_root_path_resolved=bottle_root_path_resolved,
        experimental=experimental,
        allow_bottle_migration=allow_bottle_migration,
        materialize=materialize and not errors,
        warnings=warnings,
        errors=errors,
    )

    env = os.environ.copy()
    env.update(_profile_env(profile))
    env["WINEPREFIX"] = str(bottle_root_path_resolved / bottle.id)
    tmpdir = bottle_root_path_resolved / bottle.id / "tmp"
    if materialize and not errors:
        tmpdir.mkdir(parents=True, exist_ok=True)
    env["TMPDIR"] = str(tmpdir)
    env["MACRUNNER_PROFILE_ID"] = profile.id
    env["MACRUNNER_BOTTLE_ID"] = bottle.id
    env["MACRUNNER_LANE"] = lane
    env["MACRUNNER_NATIVE_LEVEL"] = engine.native_level if engine else "unknown"
    env["MACRUNNER_SELECTED_MACHINE"] = guest_machine
    env["MACRUNNER_ENGINE_ID"] = engine.id if engine else "unknown"
    _configure_wine_env_for_lane(env, lane, engine)
    if game_runtime:
        env["MACRUNNER_GRAPHICS_BACKEND"] = str(game_runtime.graphics_backend or "")
        env["MACRUNNER_SHADER_CACHE_ROOT"] = str(game_runtime.shader_cache_root or "")
        env["MACRUNNER_INPUT_MODE"] = str(game_runtime.input_mode or "")
        env["MACRUNNER_FAST_IO"] = "1" if game_runtime.fast_io_enabled else "0"
        env["MACRUNNER_THREADING_MODE"] = str(game_runtime.threading_mode or "")
        env["MACRUNNER_ANTICHEAT_MODE"] = str(game_runtime.anti_cheat_mode or "")
        _apply_graphics_runtime_env(env, game_runtime.graphics_backend)
    else:
        _apply_graphics_runtime_env(env, None)

    if engine and _lane_requires_rosetta(lane):
        command = [str(Path(__file__).resolve().parents[2] / "scripts/run-with-rosetta.sh"), engine.path, *command_args]
    elif engine:
        command = [engine.path, *command_args]
    else:
        command = ["unknown-engine", *command_args]

    native_score = 0 if errors else (100 if lane == "arm64-native" else 85 if lane == "arm64ec-x64-bridge" else 60)
    compatibility_score = 0 if errors else (95 if _lane_requires_rosetta(lane) else 80 if lane == "arm64ec-x64-bridge" else 70)
    reason = (
        "ARM64 command routed to native lane"
        if lane == "arm64-native"
        else "x86_64 command routed through Rosetta compatibility lane"
        if lane == "x86_64-rosetta"
        else "x86 command routed through Rosetta WoW64 compatibility lane"
        if lane == "x86-rosetta-wow64"
        else "experimental ARM64EC/x64 bridge lane selected"
        if lane == "arm64ec-x64-bridge"
        else "command routed through native arm64 HyperBridge lane"
        if lane == "arm64-hyperbridge"
        else "unsupported lane"
    )

    return CompatibilityPlan(
        exe_path=" ".join(command_args),
        pe_machine="command",
        selected_lane=lane,
        native_level=engine.native_level if engine else "unknown",
        engine_id=engine.id if engine else "unknown",
        engine_path=engine.path if engine else None,
        profile_id=profile.id,
        bottle_id=bottle.id,
        bottle_root=str(bottle_root_path_resolved),
        wineprefix=str(bottle_root_path_resolved / bottle.id),
        rosetta_required=rosetta_required,
        rosetta_available=rosetta_available,
        experimental=experimental,
        native_score=native_score,
        compatibility_score=compatibility_score,
        reason=reason,
        command=command,
        env=env,
        game_runtime=game_runtime,
        warnings=warnings,
        errors=errors,
    )


def plan_as_dict(plan: CompatibilityPlan) -> dict[str, Any]:
    return asdict(plan)


def _terminate_process_group(pid: int, grace_sec: float = 2.0) -> None:
    try:
        os.killpg(pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError, OSError):
        pass
    time.sleep(grace_sec)
    try:
        os.killpg(pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError, OSError):
        pass


def _cleanup_wine_runtime(plan: CompatibilityPlan) -> None:
    cleanup = Path(__file__).resolve().parents[2] / "scripts" / "cleanup-wine-runtime.py"
    if cleanup.exists():
        cmd = [str(cleanup), "--quiet", "--prefix", plan.env.get("WINEPREFIX") or plan.wineprefix]
        wineserver = plan.env.get("WINESERVER")
        if wineserver:
            cmd.extend(["--wineserver", wineserver])
        subprocess.run(cmd, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return
    prefix_tmp = Path(plan.env.get("TMPDIR") or Path(plan.wineprefix) / "tmp")
    for temp_path in glob.glob(str(prefix_tmp / "winetemp-*")):
        shutil.rmtree(temp_path, ignore_errors=True)


def _sync_hyperbridge_prefix(plan: CompatibilityPlan) -> tuple[bool, str]:
    if plan.env.get("MACRUNNER_LANE") != "arm64-hyperbridge":
        return True, ""

    root = Path(plan.env.get("MACRUNNER_ROOT") or Path(__file__).resolve().parents[2])
    script = root / "scripts" / "sync-prefix-from-dist.sh"
    prefix = plan.env.get("WINEPREFIX") or plan.wineprefix
    dist = plan.env.get("MACRUNNER_WINE_DIST_ARM64") or str(root / "engine/wine/dist-pure-arm64")
    if not script.exists():
        return False, f"missing prefix sync script: {script}"

    try:
        result = subprocess.run(
            [str(script), "--dist", dist, "--prefix", prefix],
            env=plan.env,
            text=True,
            capture_output=True,
            timeout=45,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return False, f"prefix sync timed out: {prefix}"
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        return False, f"prefix sync failed rc={result.returncode}: {detail[-1000:]}"
    return True, result.stdout.strip()


def _output_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return value


def _launch(
    plan: CompatibilityPlan,
    save_report: bool = False,
    timeout_sec: int | None = None,
    stdin_data: str | None = None,
    workdir: str | None = None,
) -> int:
    if plan.errors:
        for error in plan.errors:
            print(f"error: {error}", file=sys.stderr)
        return 2

    start = time.monotonic()
    timed_out = False
    stdout = ""
    stderr = ""
    returncode = 0
    try:
        sync_ok, sync_output = _sync_hyperbridge_prefix(plan)
        if not sync_ok:
            print(f"error: {sync_output}", file=sys.stderr)
            return 2
        if sync_output:
            print(sync_output, file=sys.stderr)

        proc = subprocess.Popen(
            plan.command,
            env=plan.env,
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.PIPE if stdin_data is not None else None,
            text=True,
            start_new_session=True,
        )
        if timeout_sec is None:
            stdout, stderr = proc.communicate(input=stdin_data)
            returncode = proc.returncode or 0
        else:
            try:
                stdout, stderr = proc.communicate(input=stdin_data, timeout=timeout_sec)
                returncode = proc.returncode or 0
            except subprocess.TimeoutExpired as exc:
                timed_out = True
                stdout = _output_text(exc.stdout)
                stderr = _output_text(exc.stderr)
                _terminate_process_group(proc.pid)
                try:
                    more_stdout, more_stderr = proc.communicate(timeout=3)
                    stdout += _output_text(more_stdout)
                    stderr += _output_text(more_stderr)
                except subprocess.TimeoutExpired as second_exc:
                    stdout += _output_text(second_exc.stdout)
                    stderr += _output_text(second_exc.stderr)
                    stderr += "\nMacRunner timeout: process group did not fully exit after SIGKILL.\n"
                returncode = 124
    finally:
        _cleanup_wine_runtime(plan)
    duration_ms = int((time.monotonic() - start) * 1000)
    if stdout:
        sys.stdout.write(stdout)
    if stderr:
        sys.stderr.write(stderr)

    if save_report:
        reports_dir = Path(plan.wineprefix) / "reports"
        logs_dir = Path(plan.wineprefix) / "logs"
        logs_dir.mkdir(parents=True, exist_ok=True)
        reports_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime())
        log_path = logs_dir / f"launch-{stamp}.log"
        stdout_path = logs_dir / f"launch-{stamp}.stdout.log"
        stderr_path = logs_dir / f"launch-{stamp}.stderr.log"
        stdout_path.write_text(stdout or "", encoding="utf-8")
        stderr_path.write_text(stderr or "", encoding="utf-8")
        log_path.write_text(
            "\n".join(
                [
                    f"command: {' '.join(plan.command)}",
                    f"exit_code: {returncode}",
                    f"timeout: {timed_out}",
                    f"timeout_sec: {timeout_sec}",
                    f"workdir: {workdir or ''}",
                    f"stdin: {stdin_data is not None}",
                    "--- stdout ---",
                    stdout or "",
                    "--- stderr ---",
                    stderr or "",
                ]
            ),
            encoding="utf-8",
        )
        report = {
            "CompatibilityPlan": plan_as_dict(plan),
            "exit_code": returncode,
            "duration_ms": duration_ms,
            "timeout": timed_out,
            "timeout_sec": timeout_sec,
            "workdir": workdir,
            "stdin_provided": stdin_data is not None,
            "log_path": str(log_path),
            "stdout_path": str(stdout_path),
            "stderr_path": str(stderr_path),
            "warnings": plan.warnings,
            "errors": plan.errors,
        }
        write_json(reports_dir / f"launch-{stamp}.json", report)

    return returncode


def _launch_capture(
    plan: CompatibilityPlan,
    timeout_sec: int | None = None,
    stdin_data: str | None = None,
    workdir: str | None = None,
) -> dict[str, Any]:
    """Launch and capture output without writing to sys.stdout/stderr."""
    if plan.errors:
        return {
            "returncode": 2,
            "stdout": "",
            "stderr": "\n".join(f"error: {e}" for e in plan.errors),
            "duration_ms": 0,
            "timed_out": False,
        }

    start = time.monotonic()
    timed_out = False
    stdout = ""
    stderr = ""
    returncode = 0
    try:
        proc = subprocess.Popen(
            plan.command,
            env=plan.env,
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdin=subprocess.PIPE if stdin_data is not None else None,
            text=True,
            start_new_session=True,
        )
        if timeout_sec is None:
            stdout, stderr = proc.communicate(input=stdin_data)
            returncode = proc.returncode or 0
        else:
            try:
                stdout, stderr = proc.communicate(input=stdin_data, timeout=timeout_sec)
                returncode = proc.returncode or 0
            except subprocess.TimeoutExpired as exc:
                timed_out = True
                stdout = _output_text(exc.stdout)
                stderr = _output_text(exc.stderr)
                _terminate_process_group(proc.pid)
                try:
                    more_stdout, more_stderr = proc.communicate(timeout=3)
                    stdout += _output_text(more_stdout)
                    stderr += _output_text(more_stderr)
                except subprocess.TimeoutExpired as second_exc:
                    stdout += _output_text(second_exc.stdout)
                    stderr += _output_text(second_exc.stderr)
                    stderr += "\nMacRunner timeout: process group did not fully exit after SIGKILL.\n"
                returncode = 124
    finally:
        _cleanup_wine_runtime(plan)
    duration_ms = int((time.monotonic() - start) * 1000)

    return {
        "returncode": returncode,
        "stdout": stdout,
        "stderr": stderr,
        "duration_ms": duration_ms,
        "timed_out": timed_out,
    }


def _apply_tuner_params_to_plan(plan: CompatibilityPlan, params: dict[str, Any]) -> CompatibilityPlan:
    """Apply tuner params to a compatibility plan.

    Only env.* params are applied directly; other params require
    rebuilding the plan and are skipped in this MVP.
    """
    env = dict(plan.env)
    for key, value in params.items():
        if key.startswith("env."):
            env[key[4:]] = str(value) if value is not None else ""
        elif key == "wine_settings.locale":
            env.setdefault("LANG", str(value))
            env.setdefault("LC_ALL", str(value))
        elif key == "wine_settings.font_smoothing":
            env.setdefault("MACRUNNER_FONT_SMOOTHING", str(value))
        elif key == "wine_settings.audio_backend":
            env.setdefault("MACRUNNER_AUDIO_BACKEND", str(value))
        elif key == "performance.threading":
            env.setdefault("MACRUNNER_THREADING_MODE", str(value))
        elif key == "bottle_policy.isolation":
            # Cannot change bottle isolation at runtime
            pass
        elif key == "default_lane":
            # Cannot change lane at runtime without rebuilding plan
            pass
    return replace(plan, env=env)


def _run_tuning(
    base_plan: CompatibilityPlan,
    profile_id: str,
    max_iterations: int,
    dry_run: bool,
    verbose: bool,
    timeout_sec: int,
) -> int:
    """Run auto-tuning session and print report."""
    category = base_plan.game_runtime.graphics_api if base_plan.game_runtime else "business"
    if category not in {"game", "business"}:
        category = "game"

    def run_fn(params: dict[str, Any]) -> dict[str, Any]:
        plan = _apply_tuner_params_to_plan(base_plan, params)
        if dry_run:
            if verbose:
                print(f"[dry-run] params={params}", file=sys.stderr)
            return {
                "launch_success": True,
                "fps": 30.0,
                "stability_penalty": 0.0,
                "error_count": 0,
                "duration_ms": 1000,
                "stderr_analysis": {},
            }

        result = _launch_capture(plan, timeout_sec=timeout_sec)
        stderr_text = result.get("stderr", "")
        stderr_analysis = analyze_smoke_stderr(stderr_text)
        error_count = stderr_analysis["error_summary"].get("errors_found", 0)

        if verbose:
            print(
                f"[tune] iter params={params} rc={result['returncode']} "
                f"errors={error_count} duration={result['duration_ms']}ms",
                file=sys.stderr,
            )

        return {
            "launch_success": result["returncode"] == 0 and not result["timed_out"],
            "fps": None,  # No FPS counter in general launch path yet
            "stability_penalty": error_count * 0.5,
            "error_count": error_count,
            "duration_ms": result["duration_ms"],
            "stderr_analysis": stderr_analysis,
        }

    session = run_tuning_session(
        profile_id=profile_id,
        category=category,
        run_fn=run_fn,
        max_iterations=max_iterations,
    )
    print(format_tuning_report(session), file=sys.stderr)
    return 0 if session.best_score > 0 else 1


def _apply_runtime_env_overrides(plan: CompatibilityPlan, overrides: Sequence[str] | None) -> CompatibilityPlan:
    if not overrides:
        return plan
    env = dict(plan.env)
    for item in overrides:
        if "=" not in item:
            raise ValueError(f"--env value must be NAME=VALUE: {item}")
        key, value = item.split("=", 1)
        if not key:
            raise ValueError("--env variable name must not be empty")
        env[key] = value
    return replace(plan, env=env)


def _print_plan(plan: CompatibilityPlan) -> None:
    print(f"MacRunner profile: {plan.profile_id}", file=sys.stderr)
    print(f"MacRunner lane: {plan.selected_lane}", file=sys.stderr)
    print(f"  target:  {plan.exe_path}", file=sys.stderr)
    print(f"  pe:      {plan.pe_machine}", file=sys.stderr)
    print(f"  prefix:  {plan.wineprefix}", file=sys.stderr)
    print(f"  engine:  {plan.engine_id}", file=sys.stderr)
    print(f"  native:  {plan.native_level}", file=sys.stderr)
    if plan.game_runtime:
        print(f"  graphics_backend: {plan.game_runtime.graphics_backend}", file=sys.stderr)
        print(f"  shader_cache: {plan.game_runtime.shader_cache_enabled}", file=sys.stderr)
        print(f"  input_mode: {plan.game_runtime.input_mode}", file=sys.stderr)
        print(f"  fast_io: {plan.game_runtime.fast_io_enabled}", file=sys.stderr)
        print(f"  threading: {plan.game_runtime.threading_mode}", file=sys.stderr)
    for warning in plan.warnings:
        print(f"warning: {warning}", file=sys.stderr)
    for error in plan.errors:
        print(f"error: {error}", file=sys.stderr)


def render_doctor() -> str:
    return inventory_doctor_text()


def _run_shader_cache_action(action: str, bottle_id: str, bottle_root: str | None) -> int:
    from app.game_runtime.shader_cache import clean_shader_cache, shader_cache_stats

    if action == "stats":
        stats = shader_cache_stats(bottle_root_path(bottle_root), bottle_id)
        print(dump_json(stats))
        return 0
    if action == "clean":
        clean_shader_cache(bottle_root_path(bottle_root), bottle_id)
        return 0
    return 2


def _run_fast_io_action(action: str, bottle_id: str, bottle_root: str | None) -> int:
    from app.game_runtime.fast_io import build_fast_io_plan, fast_io_prefetch, fast_io_check

    profile = resolve_profile(None)
    if action == "check":
        report = fast_io_check(profile=profile, bottle=None, bottle_root=bottle_root_path(bottle_root), bottle_id=bottle_id)
        print(dump_json(report))
        return 0
    if action == "prefetch":
        report = fast_io_prefetch(profile=profile, bottle_root=bottle_root_path(bottle_root), bottle_id=bottle_id)
        print(dump_json(report))
        return 0
    return 2


def _run_benchmark(
    exe_path: str,
    profile_id: str | None,
    bottle_root: str | None,
    *,
    bottle_id: str | None = None,
    dry_run: bool = False,
    timeout_sec: int = 45,
) -> int:
    from app.game_runtime.benchmark import benchmark_launch

    result = benchmark_launch(
        exe_path=exe_path,
        profile_id=profile_id,
        bottle_root=bottle_root,
        bottle_id=bottle_id,
        dry_run=dry_run,
        timeout_sec=timeout_sec,
    )
    print(dump_json(result))
    return 0 if result.get("exit_code", 1) == 0 or dry_run else 1


def _write_game_manifest(plan: CompatibilityPlan, exe_path: str) -> Path:
    manifest = {
        "schema_version": 1,
        "profile_id": plan.profile_id,
        "exe_path": exe_path,
        "pe_machine": plan.pe_machine,
        "selected_lane": plan.selected_lane,
        "graphics_backend": plan.game_runtime.graphics_backend if plan.game_runtime else None,
        "shader_cache": bool(plan.game_runtime.shader_cache_enabled) if plan.game_runtime else False,
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    bottle_dir = Path(plan.wineprefix)
    return write_json(bottle_dir / "game-manifest.json", manifest)


def _import_backend_action(backend: str, source: str) -> dict[str, Any]:
    source_path = Path(source).expanduser().resolve()
    if not source_path.exists():
        raise FileNotFoundError(f"backend source not found: {source_path}")
    if not source_path.is_dir():
        raise ValueError(f"backend source must be a directory: {source_path}")

    required_files = {
        "dxvk": {"d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"},
        "vkd3d": {"d3d12.dll", "dxgi.dll"},
        "moltenvk": {"libMoltenVK.dylib"},
        "d3dmetal": set(),
    }.get(backend, set())
    source_files = {str(path.relative_to(source_path)) for path in source_path.rglob("*") if path.is_file()}
    if required_files and not required_files.issubset({Path(name).name for name in source_files}):
        missing = sorted(required_files - {Path(name).name for name in source_files})
        raise ValueError(f"{backend}: missing required files: {', '.join(missing)}")
    if not source_files:
        raise ValueError(f"{backend}: source directory is empty")

    arch_candidates: list[str] = []
    for rel in sorted(source_files):
        file_path = source_path / rel
        code, stdout, _ = _run_text(["/usr/bin/file", str(file_path)], timeout=10)
        if code == 0 and stdout:
            text = stdout.lower()
            if "x86-64" in text or "x86_64" in text:
                arch_candidates.append("x86_64")
            elif "arm64" in text or "aarch64" in text:
                arch_candidates.append("arm64")
            elif "universal" in text:
                arch_candidates.append("universal")
    architecture = arch_candidates[0] if arch_candidates else "unknown"
    usable_lanes = {
        "dxvk": ["x86_64-rosetta"],
        "vkd3d": ["x86_64-rosetta"],
        "moltenvk": ["arm64-native"],
        "d3dmetal": ["arm64-native", "arm64ec-x64-bridge"],
    }.get(backend, [])
    dest_root = ENGINE_BACKEND_ROOT / backend
    if dest_root.exists():
        shutil.rmtree(dest_root)
    dest_root.mkdir(parents=True, exist_ok=True)
    for rel in sorted(source_files):
        src = source_path / rel
        dst = dest_root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    backend_json = {
        "schema_version": 1,
        "id": backend,
        "version": "unknown",
        "imported_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "files": sorted(source_files),
        "architecture": architecture,
        "usable_lanes": usable_lanes,
    }
    write_json(dest_root / "backend.json", backend_json)
    return backend_json


def _doctor_fix(root: str | Path | None = None) -> dict[str, Any]:
    bottle_root = bottle_root_path(root)
    bottle_root.mkdir(parents=True, exist_ok=True)
    REPORT_ROOT = Path.home() / "Library/Logs/MacRunner"
    REPORT_ROOT.mkdir(parents=True, exist_ok=True)
    ENGINE_BACKEND_ROOT.mkdir(parents=True, exist_ok=True)
    for name in ("d3dmetal", "dxvk", "vkd3d", "moltenvk", "dxvk-moltenvk", "vkd3d-moltenvk", "native-metal-experimental"):
        (ENGINE_BACKEND_ROOT / name).mkdir(parents=True, exist_ok=True)
    for name in ("logs", "reports", "cache", "shader-cache"):
        (bottle_root / "default" / name).mkdir(parents=True, exist_ok=True)
    return {
        "bottle_root": str(bottle_root),
        "reports_root": str(REPORT_ROOT),
        "engine_backends_root": str(ENGINE_BACKEND_ROOT),
    }


def _bottle_status(root: str | Path | None, bottle_id: str) -> dict[str, Any]:
    bottle_root = bottle_root_path(root)
    manifest = load_manifest(bottle_root, bottle_id)
    reports_dir = bottle_root / bottle_id / "reports"
    logs_dir = bottle_root / bottle_id / "logs"
    report_files = sorted(reports_dir.glob("*.json")) if reports_dir.exists() else []
    latest_report = None
    if report_files:
        try:
            latest_report = json.loads(report_files[-1].read_text(encoding="utf-8"))
        except Exception:
            latest_report = None
    return {
        "bottle_root": str(bottle_root),
        "bottle_id": bottle_id,
        "exists": (bottle_root / bottle_id).exists(),
        "manifest": asdict(manifest) if manifest else None,
        "latest_report": latest_report,
        "report_count": len(report_files),
        "logs_path": str(logs_dir),
        "reports_path": str(reports_dir),
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="MacRunner compatibility launcher")
    parser.add_argument("target", nargs="?", help="Path to the Windows executable")
    parser.add_argument("args", nargs=argparse.REMAINDER, help="Arguments for the executable")
    parser.add_argument("--engine-command")
    parser.add_argument("--profile")
    parser.add_argument("--exe")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--real", action="store_true", default=False)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--lane")
    parser.add_argument("--prefer-native", action="store_true", default=False)
    parser.add_argument("--prefer-compatibility", action="store_true", default=False)
    parser.add_argument("--experimental", action="store_true", default=False)
    parser.add_argument("--bottle")
    parser.add_argument("--new-bottle", action="store_true", default=False)
    parser.add_argument("--bottle-root")
    parser.add_argument("--allow-bottle-migration", action="store_true", default=False)
    parser.add_argument("--save-report", action="store_true", default=False)
    parser.add_argument("--stdin", dest="stdin_data")
    parser.add_argument("--workdir")
    parser.add_argument("--env", dest="env_overrides", action="append", default=[])
    parser.add_argument("--doctor", action="store_true", default=False)
    parser.add_argument("--fix", action="store_true", default=False)
    parser.add_argument("--inventory", action="store_true", default=False)
    parser.add_argument("--graphics-inventory", action="store_true", default=False)
    parser.add_argument("--input-doctor", action="store_true", default=False)
    parser.add_argument("--shader-cache-stats", action="store_true", default=False)
    parser.add_argument("--shader-cache-clean", action="store_true", default=False)
    parser.add_argument("--shader-cache-init", action="store_true", default=False)
    parser.add_argument("--backend")
    parser.add_argument("--fast-io-check", action="store_true", default=False)
    parser.add_argument("--fast-io-index", action="store_true", default=False)
    parser.add_argument("--fast-io-prefetch", action="store_true", default=False)
    parser.add_argument("--benchmark", action="store_true", default=False)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--add-game", action="store_true", default=False)
    parser.add_argument("--import-backend")
    parser.add_argument("--from", dest="from_path")
    parser.add_argument("--status", action="store_true", default=False)
    parser.add_argument("--tune", action="store_true", default=False, help="Run auto-tuning loop")
    parser.add_argument("--tune-iterations", type=int, default=10, help="Max tuning iterations")
    parser.add_argument("--tune-dry-run", action="store_true", default=False, help="Print params without running")
    parser.add_argument("--tune-verbose", action="store_true", default=False, help="Verbose iteration logging")
    ns = parser.parse_args(list(argv) if argv is not None else None)

    command_args = list(ns.args)
    if command_args and command_args[0] == "--":
        command_args = command_args[1:]
    dry_run_effective = ns.dry_run and not ns.real

    if ns.inventory:
        print(dump_json(build_inventory(ns.bottle_root)))
        return 0
    if ns.graphics_inventory:
        print(dump_json(build_graphics_inventory()))
        return 0
    if ns.input_doctor:
        print(dump_json(build_input_inventory()))
        return 0
    if ns.status:
        bottle_id = ns.bottle or ns.profile or "default"
        print(dump_json(_bottle_status(ns.bottle_root, bottle_id)))
        return 0

    if ns.doctor:
        if ns.fix:
            _doctor_fix(ns.bottle_root)
        if ns.json:
            print(dump_json(build_inventory(ns.bottle_root)))
        else:
            print(render_doctor())
        return 0

    if ns.import_backend:
        if not ns.from_path:
            print("error: --import-backend requires --from", file=sys.stderr)
            return 2
        try:
            backend_json = _import_backend_action(ns.import_backend, ns.from_path)
        except (FileNotFoundError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        print(dump_json(backend_json))
        return 0

    if ns.shader_cache_init:
        from app.game_runtime.shader_cache import shader_cache_init

        backend = ns.backend or "d3dmetal"
        print(dump_json(shader_cache_init(bottle_root_path(ns.bottle_root), ns.bottle or "default", backend)))
        return 0
    if ns.shader_cache_stats:
        from app.game_runtime.shader_cache import shader_cache_stats

        print(dump_json(shader_cache_stats(bottle_root_path(ns.bottle_root), ns.bottle or "default", backend=ns.backend)))
        return 0
    if ns.shader_cache_clean:
        from app.game_runtime.shader_cache import clean_shader_cache

        clean_shader_cache(bottle_root_path(ns.bottle_root), ns.bottle or "default", backend=ns.backend)
        return 0
    if ns.fast_io_check:
        return _run_fast_io_action("check", ns.bottle or "default", ns.bottle_root)
    if ns.fast_io_index:
        from app.game_runtime.fast_io import fast_io_index

        result = fast_io_index(resolve_profile(ns.profile), bottle_root_path(ns.bottle_root), ns.bottle or "default")
        print(dump_json(result))
        return 0
    if ns.fast_io_prefetch:
        return _run_fast_io_action("prefetch", ns.bottle or "default", ns.bottle_root)
    if ns.benchmark:
        if not ns.target and not ns.exe:
            print("error: benchmark requires a target executable", file=sys.stderr)
            return 2
        target = ns.target or ns.exe
        return _run_benchmark(
            target,
            ns.profile,
            ns.bottle_root,
            bottle_id=ns.bottle,
            dry_run=dry_run_effective,
            timeout_sec=ns.timeout,
        )

    if ns.engine_command:
        engine_args = []
        if ns.target:
            engine_args.append(ns.target)
        engine_args.extend(command_args)
        try:
            plan = build_engine_command_plan(
                engine_args,
                lane=ns.engine_command,
                profile_id=ns.profile,
                bottle_id=ns.bottle,
                bottle_root=ns.bottle_root,
                experimental=ns.experimental,
                prefer_native=ns.prefer_native,
                prefer_compatibility=ns.prefer_compatibility,
                allow_bottle_migration=ns.allow_bottle_migration,
                materialize=not dry_run_effective or ns.save_report or ns.real,
            )
        except (FileNotFoundError, EngineRegistryError, BottleMismatchError, ValueError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        try:
            plan = _apply_runtime_env_overrides(plan, ns.env_overrides)
        except ValueError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        if ns.tune:
            return _run_tuning(
                base_plan=plan,
                profile_id=ns.profile or "default",
                max_iterations=ns.tune_iterations,
                dry_run=ns.tune_dry_run or dry_run_effective,
                verbose=ns.tune_verbose,
                timeout_sec=ns.timeout,
            )
        if ns.json:
            print(dump_json(plan_as_dict(plan)))
            return 0
        if dry_run_effective:
            _print_plan(plan)
            return 0
        return _launch(
            plan,
            save_report=True,
            timeout_sec=ns.timeout,
            stdin_data=ns.stdin_data,
            workdir=ns.workdir,
        )

    target = ns.exe or ns.target
    if ns.add_game and not target:
        print("error: --add-game requires --exe or a target executable", file=sys.stderr)
        return 2
    if not target:
        parser.error("target executable is required")

    try:
        plan = build_compatibility_plan(
            exe_path=target,
            profile_id=ns.profile,
            user_lane_override=ns.lane,
            experimental=ns.experimental,
            prefer_native=ns.prefer_native,
            prefer_compatibility=ns.prefer_compatibility,
            bottle_id=ns.bottle,
            new_bottle=ns.new_bottle,
            bottle_root=ns.bottle_root,
            allow_bottle_migration=ns.allow_bottle_migration,
            extra_args=command_args,
            materialize=not dry_run_effective or ns.save_report or ns.add_game or ns.real,
        )
    except (PEFormatError, FileNotFoundError, EngineRegistryError, BottleMismatchError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    try:
        plan = _apply_runtime_env_overrides(plan, ns.env_overrides)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if ns.tune:
        return _run_tuning(
            base_plan=plan,
            profile_id=ns.profile or "default",
            max_iterations=ns.tune_iterations,
            dry_run=ns.tune_dry_run or dry_run_effective,
            verbose=ns.tune_verbose,
            timeout_sec=ns.timeout,
        )

    if ns.add_game:
        _write_game_manifest(plan, target)

    if ns.json:
        print(dump_json(plan_as_dict(plan)))
        return 0

    if dry_run_effective:
        _print_plan(plan)
        return 0

    return _launch(
        plan,
        save_report=True if (ns.real or ns.add_game or ns.save_report or not dry_run_effective) else ns.save_report,
        timeout_sec=ns.timeout,
        stdin_data=ns.stdin_data,
        workdir=ns.workdir,
    )
