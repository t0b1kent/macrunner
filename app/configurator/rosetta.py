#!/usr/bin/env python3
"""Rosetta 2 preflight helpers."""

from __future__ import annotations

import subprocess


def is_rosetta_available() -> bool:
    """Check whether Rosetta can execute x86_64 binaries on this host."""

    try:
        result = subprocess.run(
            ["/usr/bin/arch", "-x86_64", "/usr/bin/true"],
            check=False,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError:
        return False
    return result.returncode == 0


def rosetta_install_hint() -> str:
    return "Rosetta 2 is not available. Install it with: softwareupdate --install-rosetta --agree-to-license"

