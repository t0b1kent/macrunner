#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
from typing import Any, Dict


def run_fixture_with_hyperbridge(fixture_path: Path, backend: str = "interp") -> Dict[str, Any]:
    """
    Stub adapter for HyperBridge fixture execution.

    TODO: wire to hb_test_runner hooks and emit normalized oracle-result schema payload.
    backend: interp | jit | jit_fallback
    """
    raise NotImplementedError("HyperBridge adapter is a scaffold. Implement hb_test_runner integration.")
