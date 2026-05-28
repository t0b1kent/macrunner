"""Privacy and GDPR compliance for community profile sharing (Phase A.6).

Per ADR-A004 §5 and NETWORKING-ADR-006:
- Strict opt-in
- No PII in uploaded profiles
- Anonymized contributor IDs
- Automatic PII stripping before upload
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any


# Keys that must NEVER appear in a community-uploaded profile.
_PI_BLACKLIST: set[str] = {
    "user_name",
    "user_email",
    "serial_number",
    "machine_id",
    "device_uuid",
    "home_path",
    "file_path",
    "exe_path",
    "wineprefix",
    "bottle_root",
    "local_username",
    "computer_name",
    "network_hostname",
}

# Regex for paths that look like home directories or local paths.
_LOCAL_PATH_RE = re.compile(r"[Cc]:\\[Uu]sers\\[^\\]+|/Users/[^/]+|/home/[^/]+|~[/\\]")

# Whitelisted URL domains for fields that allow URLs.
_WHITELISTED_DOMAINS: set[str] = {
    "github.com",
    "raw.githubusercontent.com",
    "gitlab.com",
    "winehq.org",
    "codeweavers.com",
    "playonlinux.com",
    "macrunner.dev",
}


def strip_pii(profile_json: dict[str, Any]) -> dict[str, Any]:
    """Recursively remove PII from a profile dict before upload.

    Returns a deep-copied, sanitized dict.
    """
    out: dict[str, Any] = {}
    for key, value in profile_json.items():
        if key.lower() in _PI_BLACKLIST:
            continue
        if isinstance(value, dict):
            cleaned = strip_pii(value)
            if cleaned:
                out[key] = cleaned
        elif isinstance(value, list):
            out[key] = [
                strip_pii(item) if isinstance(item, dict) else _strip_string(item)
                for item in value
            ]
        elif isinstance(value, str):
            out[key] = _strip_string(value)
        else:
            out[key] = value
    return out


def _strip_string(value: str) -> str:
    """Remove local paths from a string value."""
    return _LOCAL_PATH_RE.sub("[REDACTED]", value)


def has_pii(profile_json: dict[str, Any]) -> list[str]:
    """Audit a profile for remaining PII. Returns list of violation paths."""
    violations: list[str] = []
    _scan_for_pii(profile_json, "", violations)
    return violations


def _scan_for_pii(node: Any, path: str, violations: list[str]) -> None:
    if isinstance(node, dict):
        for k, v in node.items():
            if k.lower() in _PI_BLACKLIST:
                violations.append(f"{path}.{k}" if path else k)
            _scan_for_pii(v, f"{path}.{k}" if path else k, violations)
    elif isinstance(node, list):
        for i, item in enumerate(node):
            _scan_for_pii(item, f"{path}[{i}]", violations)
    elif isinstance(node, str):
        if _LOCAL_PATH_RE.search(node):
            violations.append(path)


def validate_no_pii(profile_json: dict[str, Any]) -> bool:
    """Return True only if the profile contains zero PII."""
    return len(has_pii(profile_json)) == 0


# ---------------------------------------------------------------------------
# Profile JSON serialization helpers
# ---------------------------------------------------------------------------

def serialize_for_upload(profile_json: dict[str, Any]) -> str:
    """Sanitize and serialize a profile for HTTP upload."""
    cleaned = strip_pii(profile_json)
    return json.dumps(cleaned, separators=(",", ":"), ensure_ascii=False)


def compute_pe_hash(exe_path: str | Path) -> str:
    """Compute SHA-256 of a PE binary for community lookup."""
    import hashlib

    data = Path(exe_path).read_bytes()
    return hashlib.sha256(data).hexdigest()
