#!/usr/bin/env python3
"""Back-compat launcher facade for MacRunner."""

from __future__ import annotations

from dataclasses import asdict
from pathlib import Path
from typing import Sequence

from app.configurator.compatibility import (
    CompatibilityPlan,
    build_compatibility_plan,
    main as compatibility_main,
)
from app.configurator.pe import PEMachine, detect_pe_machine


def normalize_arch(arch: str) -> str:
    if arch == "x86_64":
        return "x86_64"
    if arch == "i386":
        return "x86"
    if arch == "arm64":
        return "arm64"
    return "unknown"


def select_lane(arch: str) -> str:
    normalized = normalize_arch(arch)
    if normalized == "arm64":
        return "arm64-native"
    if normalized == "x86_64":
        return "x86_64-rosetta"
    if normalized == "x86":
        return "x86-rosetta-wow64"
    raise ValueError(f"unsupported executable architecture: {arch}")


def build_plan(target: Path, argv: Sequence[str], profile_ref: str | None = None) -> CompatibilityPlan:
    return build_compatibility_plan(
        exe_path=str(target),
        profile_id=profile_ref,
        extra_args=tuple(argv),
        materialize=False,
    )


def run_plan(plan: CompatibilityPlan) -> int:
    import subprocess

    if plan.errors:
        return 2
    result = subprocess.run(plan.command, env=plan.env, check=False)
    return result.returncode


def plan_as_dict(plan: CompatibilityPlan) -> dict[str, object]:
    return asdict(plan)


def main(argv: Sequence[str] | None = None) -> int:
    return compatibility_main(argv)
