#!/usr/bin/env python3
"""Controller and input runtime planning."""

from __future__ import annotations

from dataclasses import dataclass, field
import json
import shutil
import subprocess
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTROLLER_DB = REPO_ROOT / "engine/game/controller-db/gamecontrollerdb.txt"


@dataclass(frozen=True)
class InputPlan:
    controller_mode: str
    enable_game_controller_framework: bool
    enable_sdl_gamecontrollerdb: bool
    controller_db_path: str | None
    env: dict[str, str] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


def build_input_plan(*, profile, bottle) -> InputPlan:
    input_settings = profile.input_settings or {}
    controller_mode = str(input_settings.get("controller", "auto"))
    if controller_mode == "auto" and (profile.category or "") == "game":
        controller_mode = "xinput"

    warnings: list[str] = []
    if controller_mode in {"dualshock", "dualsense"}:
        warnings.append(f"{controller_mode} profile hints enabled.")

    enable_framework = controller_mode != "disabled"
    enable_db = CONTROLLER_DB.exists()
    env = {
        "MACRUNNER_INPUT_MODE": controller_mode,
        "MACRUNNER_CONTROLLER_DB": str(CONTROLLER_DB),
    }
    return InputPlan(
        controller_mode=controller_mode,
        enable_game_controller_framework=enable_framework,
        enable_sdl_gamecontrollerdb=enable_db,
        controller_db_path=str(CONTROLLER_DB) if enable_db else None,
        env=env,
        warnings=warnings,
    )


def _flatten_device_names(payload: object) -> list[str]:
    names: list[str] = []
    if isinstance(payload, dict):
        for key in ("_items", "items"):
            value = payload.get(key)
            if isinstance(value, list):
                for item in value:
                    names.extend(_flatten_device_names(item))
        for key in ("_name", "sppci_device", "spbluetooth", "device_name", "name", "product", "local_device_name"):
            value = payload.get(key)
            if isinstance(value, str):
                names.append(value)
        for value in payload.values():
            names.extend(_flatten_device_names(value))
    elif isinstance(payload, list):
        for item in payload:
            names.extend(_flatten_device_names(item))
    return names


def input_doctor() -> dict[str, Any]:
    warnings: list[str] = []
    devices: list[str] = []
    if shutil.which("system_profiler"):
        try:
            result = subprocess.run(
                ["/usr/sbin/system_profiler", "-json", "SPUSBDataType", "SPBluetoothDataType"],
                capture_output=True,
                text=True,
                check=False,
                timeout=20,
            )
            if result.returncode == 0 and result.stdout.strip():
                payload = json.loads(result.stdout)
                devices = sorted({name for name in _flatten_device_names(payload) if isinstance(name, str)})
            else:
                warnings.append("system_profiler did not return usable device data.")
        except Exception as exc:  # noqa: BLE001
            warnings.append(f"system_profiler probe failed: {exc}")
    else:
        warnings.append("system_profiler is not available.")

    steam_available = bool(shutil.which("Steam") or Path("/Applications/Steam.app").exists())
    controller_db_exists = CONTROLLER_DB.exists()
    controller_mode = "xinput"
    if any(term.lower() in " ".join(devices).lower() for term in ("dualsense", "dualshock")):
        controller_mode = "dualsense"
    elif any(term.lower() in " ".join(devices).lower() for term in ("xbox", "xinput", "controller")):
        controller_mode = "xinput"
    elif steam_available:
        controller_mode = "steam-input"
    else:
        controller_mode = "auto"

    return {
        "controller_db_path": str(CONTROLLER_DB),
        "controller_db_exists": controller_db_exists,
        "steam_available": steam_available,
        "detected_devices": devices,
        "recommended_mode": controller_mode,
        "warnings": warnings,
    }
