#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
from typing import Any, Dict


def run_fixture_with_sde(fixture_path: Path) -> Dict[str, Any]:
    """
    Stub adapter for Intel SDE execution.

    TODO: implement real SDE invocation + parse machine state into
    oracle-result schema shape.
    """
    raise NotImplementedError("SDE adapter is a scaffold. Implement tool invocation + parsing.")
