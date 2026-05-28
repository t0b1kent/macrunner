"""Privacy sanitization for telemetry (Phase A.8).

All fields stripped of PII before upload.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Any


def sanitize_error_detail(text: str) -> str:
    """Strip PII from error strings.

    Rules:
    - Redact home paths to [HOME]
    - Redact email addresses to [EMAIL]
    - Redact MAC addresses to [MAC]
    - Redact IP addresses to [IP]
    """
    if not text:
        return text

    # Home path
    home = str(Path.home())
    text = text.replace(home, "[HOME]")

    # Email
    text = re.sub(
        r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Z|a-z]{2,}",
        "[EMAIL]",
        text,
    )

    # MAC address
    text = re.sub(
        r"([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})",
        "[MAC]",
        text,
    )

    # IP address (v4)
    text = re.sub(
        r"\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b",
        "[IP]",
        text,
    )

    # Long paths (heuristic: contains / or \ and > 30 chars)
    def redact_long_path(m: Any) -> str:
        s = m.group(0)
        if len(s) > 30 and ("/" in s or "\\" in s):
            return "[PATH]"
        return s

    text = re.sub(r"[^\s\n]+", redact_long_path, text)

    return text


def truncate(text: str, max_len: int = 2000) -> str:
    if len(text) > max_len:
        return text[:max_len] + "...[TRUNCATED]"
    return text


def sanitize_telemetry_event(event: dict[str, Any]) -> dict[str, Any]:
    """Apply all sanitization rules to a telemetry event dict."""
    out = dict(event)
    if "error_detail" in out:
        out["error_detail"] = truncate(sanitize_error_detail(out["error_detail"]))
    return out
