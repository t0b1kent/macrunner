#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any, Dict, List


CRITICAL_REGS = {"rcx", "rsi", "rdi", "ecx", "esi", "edi"}


@dataclass
class DiffRow:
    kind: str
    key: str
    expected: Any
    actual: Any
    severity: str = "error"


def _norm(v: Any) -> Any:
    if isinstance(v, str) and v.startswith("0x"):
        try:
            return int(v, 16)
        except ValueError:
            return v.lower()
    return v


def _to_mem_map(rows: List[Dict[str, Any]]) -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    for r in rows or []:
        addr = str(r.get("address", "")).lower()
        out[addr] = str(r.get("bytes_hex", "")).lower()
    return out


def _cmp_map(kind: str, exp: Dict[str, Any], act: Dict[str, Any], diffs: List[DiffRow]) -> None:
    for k in sorted(set(exp.keys()) | set(act.keys())):
        ev, av = _norm(exp.get(k)), _norm(act.get(k))
        if ev != av:
            diffs.append(DiffRow(kind=kind, key=str(k), expected=ev, actual=av))


def compare(payload: Dict[str, Any]) -> Dict[str, Any]:
    exp = payload.get("expected", {})
    act = payload.get("actual", {})
    diffs: List[DiffRow] = []
    hard_fail_reasons: List[str] = []

    _cmp_map("register", exp.get("registers", {}), act.get("registers", {}), diffs)
    _cmp_map("flag", exp.get("flags", {}), act.get("flags", {}), diffs)
    _cmp_map("memory", _to_mem_map(exp.get("memory_after", [])), _to_mem_map(act.get("memory_after", [])), diffs)

    if _norm(exp.get("rip")) != _norm(act.get("rip")):
        diffs.append(DiffRow(kind="pc", key="rip", expected=exp.get("rip"), actual=act.get("rip")))

    expected_fault = exp.get("fault")
    actual_fault = payload.get("actual_fault", act.get("fault"))
    if expected_fault != actual_fault:
        diffs.append(DiffRow(kind="fault", key="fault", expected=expected_fault, actual=actual_fault))

    for d in diffs:
        k = d.key.lower()
        if d.kind == "flag":
            hard_fail_reasons.append(f"wrong flag {d.key}")
        if d.kind == "register" and k in CRITICAL_REGS:
            hard_fail_reasons.append(f"wrong critical reg {d.key}")
        if d.kind in {"register", "memory", "pc"}:
            hard_fail_reasons.append("regs/mem/pc mismatch")

    if expected_fault in (None, "", "none") and actual_fault not in (None, "", "none"):
        hard_fail_reasons.append("silent memory fault")

    for d in diffs:
        if d.kind == "register" and isinstance(d.expected, int) and d.expected != 0 and d.actual == 0:
            hard_fail_reasons.append(f"unexpected zero in {d.key}")

    hard_fail_reasons = sorted(set(hard_fail_reasons))
    passed = len(hard_fail_reasons) == 0 and len(diffs) == 0
    result = {
        "fixture_name": payload.get("fixture_name", "unknown"),
        "family": payload.get("family", "unknown"),
        "backend": payload.get("backend", "interp"),
        "pass": passed,
        "hard_fail_reasons": hard_fail_reasons,
        "diff": [asdict(d) for d in diffs],
    }
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description="Strict oracle compare for HyperBridge fast validation")
    ap.add_argument("--input", required=True, help="Oracle compare payload JSON")
    ap.add_argument("--out", default="", help="Optional output JSON path")
    args = ap.parse_args()

    payload = json.loads(Path(args.input).read_text(encoding="utf-8"))
    res = compare(payload)
    print(f"{'PASS' if res['pass'] else 'FAIL'} fixture={res['fixture_name']} backend={res['backend']}")
    for row in res["diff"]:
        print(f"  - {row['kind']}:{row['key']} expected={row['expected']} actual={row['actual']}")
    if res["hard_fail_reasons"]:
        print("  hard-fail:", "; ".join(res["hard_fail_reasons"]))

    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(res, indent=2), encoding="utf-8")

    return 0 if res["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
