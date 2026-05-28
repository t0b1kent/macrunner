"""Automated validation pipeline for community profile uploads (Phase A.6).

Runs server-side on every upload AND client-side as pre-flight.
Aligned with ADR-A004 §2 automated checks.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

from app.configurator.ai.community.privacy import has_pii


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_MAX_ENV_VARS = 100
_MAX_ENV_VALUE_LEN = 4096
_MAX_PROFILE_JSON_BYTES = 256 * 1024  # 256 KiB
_MAX_STRING_LEN = 4096
_MAX_NESTING_DEPTH = 8

# Anti-cheat env vars that are NEVER allowed in community profiles.
_ANTI_CHEAT_BANNED: set[str] = {
    "disable_anticheat",
    "no_battleye",
    "no_eac",
    "bypass_vanguard",
    "cheat_mode",
    "dev_mode",
    "debug_anticheat",
}

# Dangerous Wine env vars that can break system security.
_DANGEROUS_WINE_VARS: set[str] = {
    "winedebug=+all",
    "winedebug=+relay",
    "wineheapcheck",
    "wine synchronous",
}

# Executable file extensions that must not appear in any field.
_EXECUTABLE_EXTS = re.compile(r"\.(exe|dll|bat|cmd|ps1|sh|py|js|vbs|wsf)\b", re.IGNORECASE)

# URL detection (broad) — used for whitelist check.
_URL_RE = re.compile(r"https?://[^\\s\"]+")

# Whitelisted domains (same as privacy module).
_WHITELISTED_DOMAINS: set[str] = {
    "github.com",
    "raw.githubusercontent.com",
    "gitlab.com",
    "winehq.org",
    "codeweavers.com",
    "playonlinux.com",
    "macrunner.dev",
}

# Valid profile schema v3 top-level keys (subset for quick check).
_EXPECTED_TOP_KEYS: set[str] = {
    "schema_version",
    "id",
    "name",
    "category",
    "architecture",
    "default_lane",
    "graphics",
    "performance",
    "input_settings",
    "anti_cheat",
    "env",
    "args",
    "required_dlls",
    "patches",
    "wine_settings",
    "bottle_policy",
    "inherits",
    "overrides",
    "kind",
    "known_issues",
    "notes",
}


# ---------------------------------------------------------------------------
# Validation result
# ---------------------------------------------------------------------------

class ValidationResult:
    """Result of running the validation pipeline."""

    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []
        self.passed: bool = False

    def add_error(self, msg: str) -> None:
        self.errors.append(msg)

    def add_warning(self, msg: str) -> None:
        self.warnings.append(msg)

    def finalize(self) -> "ValidationResult":
        self.passed = len(self.errors) == 0
        return self


# ---------------------------------------------------------------------------
# Pipeline steps
# ---------------------------------------------------------------------------

def validate_profile(profile_json: dict[str, Any]) -> ValidationResult:
    """Run the full automated validation pipeline on a profile.

    Steps (in order):
    1. Schema structure check
    2. Size limits
    3. No executable content
    4. URL whitelist
    5. Env var bounds
    6. Anti-cheat policy
    7. PII audit
    8. Dangerous Wine flags
    """
    result = ValidationResult()

    _check_schema_structure(profile_json, result)
    _check_size_limits(profile_json, result)
    _check_no_executable_content(profile_json, result)
    _check_url_whitelist(profile_json, result)
    _check_env_bounds(profile_json, result)
    _check_anticheat_policy(profile_json, result)
    _check_pii(profile_json, result)
    _check_dangerous_wine_flags(profile_json, result)

    return result.finalize()


def _check_schema_structure(node: Any, result: ValidationResult, depth: int = 0) -> None:
    if depth > _MAX_NESTING_DEPTH:
        result.add_error(f"JSON nesting exceeds {_MAX_NESTING_DEPTH} levels")
        return
    if isinstance(node, dict):
        for key in node:
            if not isinstance(key, str):
                result.add_error(f"Non-string key in JSON: {type(key).__name__}")
            if len(key) > 128:
                result.add_error(f"Key too long: {key[:64]}...")
        # Top-level keys sanity check (if this is the root)
        if depth == 0:
            unknown = set(node.keys()) - _EXPECTED_TOP_KEYS
            if unknown:
                result.add_warning(f"Unknown top-level keys: {sorted(unknown)}")
    elif isinstance(node, list):
        for item in node:
            _check_schema_structure(item, result, depth + 1)


def _check_size_limits(profile_json: dict[str, Any], result: ValidationResult) -> None:
    import json

    raw = json.dumps(profile_json)
    if len(raw.encode("utf-8")) > _MAX_PROFILE_JSON_BYTES:
        result.add_error(f"Profile JSON exceeds {_MAX_PROFILE_JSON_BYTES} bytes")


def _check_no_executable_content(node: Any, result: ValidationResult) -> None:
    if isinstance(node, dict):
        for key, value in node.items():
            if isinstance(value, str) and _EXECUTABLE_EXTS.search(value):
                result.add_error(f"Executable reference in field '{key}': {value}")
            _check_no_executable_content(value, result)
    elif isinstance(node, list):
        for item in node:
            _check_no_executable_content(item, result)
    elif isinstance(node, str):
        if _EXECUTABLE_EXTS.search(node):
            result.add_error(f"Executable reference in string: {node[:100]}")


def _check_url_whitelist(node: Any, result: ValidationResult) -> None:
    if isinstance(node, dict):
        for key, value in node.items():
            if isinstance(value, str):
                for url in _URL_RE.findall(value):
                    domain = url.split("/")[2].split(":")[0].lower().lstrip("www.")
                    if domain not in _WHITELISTED_DOMAINS:
                        result.add_error(f"URL not in whitelist: {url}")
            _check_url_whitelist(value, result)
    elif isinstance(node, list):
        for item in node:
            _check_url_whitelist(item, result)


def _check_env_bounds(node: Any, result: ValidationResult) -> None:
    env = node.get("env") if isinstance(node, dict) else None
    if isinstance(env, dict):
        if len(env) > _MAX_ENV_VARS:
            result.add_error(f"Too many env vars: {len(env)} > {_MAX_ENV_VARS}")
        for key, value in env.items():
            if not isinstance(key, str) or not isinstance(value, str):
                result.add_error(f"Env var '{key}' must be string-valued")
                continue
            if len(value) > _MAX_ENV_VALUE_LEN:
                result.add_error(f"Env var '{key}' value too long: {len(value)} > {_MAX_ENV_VALUE_LEN}")
    wine_settings = node.get("wine_settings") if isinstance(node, dict) else None
    if isinstance(wine_settings, dict):
        for key, value in wine_settings.items():
            if isinstance(value, str) and len(value) > _MAX_ENV_VALUE_LEN:
                result.add_error(f"Wine setting '{key}' too long: {len(value)} > {_MAX_ENV_VALUE_LEN}")


def _check_anticheat_policy(node: Any, result: ValidationResult) -> None:
    env = node.get("env") if isinstance(node, dict) else {}
    if isinstance(env, dict):
        for key in env:
            if key.lower() in _ANTI_CHEAT_BANNED:
                result.add_error(f"Anti-cheat bypass env var not allowed: {key}")
    anti_cheat = node.get("anti_cheat") if isinstance(node, dict) else {}
    if isinstance(anti_cheat, dict):
        mode = anti_cheat.get("mode")
        if mode == "disable":
            result.add_error("anti_cheat.mode='disable' is not allowed in community profiles")
        if mode == "circumvent":
            result.add_error("anti_cheat.mode='circumvent' is not allowed in community profiles")


def _check_pii(node: Any, result: ValidationResult) -> None:
    if isinstance(node, dict):
        violations = has_pii(node)
        for v in violations:
            result.add_error(f"PII detected at path: {v}")


def _check_dangerous_wine_flags(node: Any, result: ValidationResult) -> None:
    env = node.get("env") if isinstance(node, dict) else {}
    if isinstance(env, dict):
        for key, value in env.items():
            combined = f"{key}={value}".lower()
            for banned in _DANGEROUS_WINE_VARS:
                if banned.lower() in combined:
                    result.add_error(f"Dangerous Wine flag detected: {banned}")
    args = node.get("args") if isinstance(node, dict) else []
    if isinstance(args, (list, tuple)):
        for arg in args:
            if isinstance(arg, str) and "wine" in arg.lower() and "debug" in arg.lower():
                result.add_warning(f"Potential debug flag in args: {arg}")
