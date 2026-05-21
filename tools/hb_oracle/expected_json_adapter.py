#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict


def load_expected_payload(path: Path) -> Dict[str, Any]:
    """
    Load expected-result JSON and return dictionary for strict comparator.

    TODO: map legacy fixture/expected formats into oracle-result schema shape.
    """
    return json.loads(path.read_text(encoding="utf-8"))
