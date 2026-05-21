#!/usr/bin/env python3
"""
hb_oracle compare skeleton.

Parses fixture expected JSON and actual backend output (JSON adapter format),
then emits structured diffs for:
- registers
- flags
- memory
- rip/pc
- fault
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Any


@dataclass
class Mismatch:
    kind: str
    key: str
    expected: Any
    actual: Any


@dataclass
class CompareResult:
    fixture: str
    backend: str
    ok: bool
    mismatches: List[Mismatch] = field(default_factory=list)


def load_json(path: Path) -> Dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def normalize_hex(v: Any) -> Any:
    if isinstance(v, str) and v.startswith("0x"):
        try:
            return hex(int(v, 16))
        except ValueError:
            return v
    return v


def compare_maps(kind: str, expected: Dict[str, Any], actual: Dict[str, Any], out: List[Mismatch]) -> None:
    keys = sorted(set(expected.keys()) | set(actual.keys()))
    for k in keys:
        ev = normalize_hex(expected.get(k))
        av = normalize_hex(actual.get(k))
        if ev != av:
            out.append(Mismatch(kind=kind, key=k, expected=ev, actual=av))


def memory_to_map(mem_list: List[Dict[str, Any]]) -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    for row in mem_list:
        addr = row.get("address")
        data = row.get("bytes_hex")
        if addr is not None:
            out[str(addr).lower()] = (str(data).lower() if data is not None else data)
    return out


def compare_fixture(fixture: Dict[str, Any], actual: Dict[str, Any], backend: str) -> CompareResult:
    expected = fixture.get("expected", {})
    mismatches: List[Mismatch] = []

    compare_maps("register", expected.get("registers", {}), actual.get("registers", {}), mismatches)
    compare_maps("flag", expected.get("flags", {}), actual.get("flags", {}), mismatches)

    exp_mem = memory_to_map(expected.get("memory", []))
    act_mem = memory_to_map(actual.get("memory", []))
    compare_maps("memory", exp_mem, act_mem, mismatches)

    exp_rip = expected.get("rip")
    act_rip = actual.get("rip")
    if exp_rip not in (None, "next"):
        if normalize_hex(exp_rip) != normalize_hex(act_rip):
            mismatches.append(Mismatch(kind="rip", key="rip", expected=exp_rip, actual=act_rip))

    exp_fault = expected.get("fault")
    act_fault = actual.get("fault")
    if exp_fault != act_fault:
        mismatches.append(Mismatch(kind="fault", key="fault", expected=exp_fault, actual=act_fault))

    return CompareResult(
        fixture=fixture.get("name", "unknown"),
        backend=backend,
        ok=(len(mismatches) == 0),
        mismatches=mismatches,
    )


def print_result(res: CompareResult) -> None:
    status = "PASS" if res.ok else "FAIL"
    print(f"[{status}] fixture={res.fixture} backend={res.backend}")
    for m in res.mismatches:
        print(f"  - {m.kind}:{m.key} expected={m.expected} actual={m.actual}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare HyperBridge actual output vs oracle fixture expected")
    ap.add_argument("--fixture", required=True, help="Path to fixture JSON")
    ap.add_argument("--actual", required=True, help="Path to actual-output JSON (adapter format)")
    ap.add_argument("--backend", default="interp", choices=["interp", "jit", "jit_fallback"], help="Backend label")
    ap.add_argument("--out", default="", help="Optional JSON result output path")
    args = ap.parse_args()

    fixture = load_json(Path(args.fixture))
    actual = load_json(Path(args.actual))
    res = compare_fixture(fixture, actual, args.backend)
    print_result(res)

    if args.out:
        out_path = Path(args.out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        payload = {
            "fixture": res.fixture,
            "backend": res.backend,
            "ok": res.ok,
            "mismatches": [m.__dict__ for m in res.mismatches],
        }
        out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    return 0 if res.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
