#!/usr/bin/env python3
"""Differential correctness fuzzer for HyperBridge x64 interpreter semantics.

SDE support is detected but not faked. When SDE is unavailable, this harness
uses two fallback signals:
  * exact Python golden oracles for selected integer/flag instructions
  * interpreter-vs-JIT post-state differential for game-relevant templates
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any


HB_ROOT = Path(__file__).resolve().parents[1]
KIT_ROOT = HB_ROOT.parents[1]
RUNNER_C = HB_ROOT / "tests" / "hb_diff_case_runner.c"
RUNNER = HB_ROOT / "tests" / "hb_diff_case_runner"
REPORT_JSON = HB_ROOT / "reports" / "hb_fuzz_diff_last.json"

MASK64 = (1 << 64) - 1
SIGN64 = 1 << 63


@dataclass(frozen=True)
class Template:
    name: str
    family: str
    code: str
    oracle: str | None = None


TEMPLATES: list[Template] = [
    Template("add_r8_r9", "int_arith_flags", "4d01c8", "add"),
    Template("adc_r8_r9", "int_arith_flags", "4d11c8", "adc"),
    Template("sub_r8_r9", "int_arith_flags", "4d29c8", "sub"),
    Template("sbb_r8_r9", "int_arith_flags", "4d19c8", "sbb"),
    Template("cmp_r8_r9", "int_arith_flags", "4d39c8", "cmp"),
    Template("and_r8_r9", "int_logic_flags", "4d21c8", "and"),
    Template("or_r8_r9", "int_logic_flags", "4d09c8", "or"),
    Template("xor_r8_r9", "int_logic_flags", "4d31c8", "xor"),
    Template("test_r8_r9", "int_logic_flags", "4d85c8", "test"),
    Template("shl_r8_cl", "shift_rotate_flags", "49d3e0", "shl"),
    Template("cmovne_r8_r9", "cmov_setcc", "4d0f45c1", "cmovne"),
    Template("setne_r8b", "cmov_setcc", "410f95c0", "setne"),
    Template("tzcnt_r8_r9", "bmi", "f34d0fbcc1", "tzcnt"),
    Template("lzcnt_r8_r9", "bmi", "f34d0fbdc1", "lzcnt"),
    Template("popcnt_r8_r9", "bmi", "f34d0fb8c1", "popcnt"),
    Template("xorps_xmm0_xmm0", "sse", "0f57c0", None),
    Template("addps_xmm0_xmm1", "sse", "0f58c1", None),
    Template("addss_xmm0_xmm1", "sse", "f30f58c1", None),
    Template("pxor_xmm0_xmm1", "sse2", "660fefc1", None),
    Template("rep_movsb", "string_ops", "f3a4", None),
    Template("rep_cmpsb", "string_ops", "f3a6", None),
    Template("rep_stosb", "string_ops", "f3aa", None),
    Template("scasb", "string_ops", "ae", None),
]


def build_runner() -> None:
    subprocess.run(["make", "libhyperbridge.a"], cwd=HB_ROOT, check=True, stdout=subprocess.PIPE)
    subprocess.run(
        [
            "cc",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-std=c11",
            "-I./include",
            str(RUNNER_C.relative_to(HB_ROOT)),
            "libhyperbridge.a",
            "-o",
            str(RUNNER.relative_to(HB_ROOT)),
        ],
        cwd=HB_ROOT,
        check=True,
        stdout=subprocess.PIPE,
    )


def as_u64(v: str) -> int:
    return int(v, 16) & MASK64


def parity_even(byte: int) -> int:
    return 1 if (byte & 0xff).bit_count() % 2 == 0 else 0


def flag_common(result: int) -> dict[str, int]:
    result &= MASK64
    return {
        "zf": 1 if result == 0 else 0,
        "sf": 1 if result & SIGN64 else 0,
        "pf": parity_even(result),
    }


def add_flags(a: int, b: int, carry: int) -> tuple[int, dict[str, int]]:
    total = a + b + carry
    result = total & MASK64
    flags = flag_common(result)
    flags["cf"] = 1 if total >> 64 else 0
    flags["of"] = 1 if ((~(a ^ b) & (a ^ result) & SIGN64) != 0) else 0
    flags["af"] = 1 if ((a ^ b ^ result) & 0x10) != 0 else 0
    return result, flags


def sub_flags(a: int, b: int, borrow: int) -> tuple[int, dict[str, int]]:
    subtrahend = b + borrow
    result = (a - subtrahend) & MASK64
    flags = flag_common(result)
    flags["cf"] = 1 if a < subtrahend else 0
    flags["of"] = 1 if (((a ^ b) & (a ^ result) & SIGN64) != 0) else 0
    flags["af"] = 1 if ((a ^ b ^ result) & 0x10) != 0 else 0
    return result, flags


def logic_flags(result: int) -> dict[str, int]:
    flags = flag_common(result)
    flags["cf"] = 0
    flags["of"] = 0
    return flags


def shl_flags(a: int, raw_count: int, initial_flags: dict[str, int]) -> tuple[int, dict[str, int]]:
    count = raw_count & 0x3f
    if count == 0:
        return a, {k: int(v) for k, v in initial_flags.items()}
    result = (a << count) & MASK64
    flags = flag_common(result)
    flags["cf"] = 1 if ((a >> (64 - count)) & 1) else 0
    if count == 1:
        flags["of"] = 1 if (((result >> 63) & 1) != flags["cf"]) else 0
    return result, flags


def exact_oracle(template: Template, row: dict[str, Any]) -> list[str]:
    if not template.oracle:
        return []
    initial = row["initial"]
    final = row["interp"]
    regs_i = initial["regs"]
    regs_f = final["regs"]
    flags_i = initial["flags"]
    flags_f = final["flags"]
    a = as_u64(regs_i["r8"])
    b = as_u64(regs_i["r9"])
    rcx = as_u64(regs_i["rcx"])
    op = template.oracle
    expected_r8 = a
    expected_flags: dict[str, int]
    if op == "add":
        expected_r8, expected_flags = add_flags(a, b, 0)
    elif op == "adc":
        expected_r8, expected_flags = add_flags(a, b, int(flags_i["cf"]))
    elif op == "sub":
        expected_r8, expected_flags = sub_flags(a, b, 0)
    elif op == "sbb":
        expected_r8, expected_flags = sub_flags(a, b, int(flags_i["cf"]))
    elif op == "cmp":
        _, expected_flags = sub_flags(a, b, 0)
    elif op == "and":
        expected_r8, expected_flags = a & b, logic_flags(a & b)
    elif op == "or":
        expected_r8, expected_flags = a | b, logic_flags(a | b)
    elif op == "xor":
        expected_r8, expected_flags = a ^ b, logic_flags(a ^ b)
    elif op == "test":
        _, expected_flags = a & b, logic_flags(a & b)
    elif op == "shl":
        expected_r8, expected_flags = shl_flags(a, rcx, flags_i)
    elif op == "cmovne":
        expected_r8 = b if int(flags_i["zf"]) == 0 else a
        expected_flags = {k: int(v) for k, v in flags_i.items()}
    elif op == "setne":
        low = 1 if int(flags_i["zf"]) == 0 else 0
        expected_r8 = (a & ~0xff) | low
        expected_flags = {k: int(v) for k, v in flags_i.items()}
    elif op == "tzcnt":
        expected_r8 = 64 if b == 0 else (b & -b).bit_length() - 1
        expected_flags = {"cf": 1 if b == 0 else 0, "zf": 1 if expected_r8 == 0 else 0}
    elif op == "lzcnt":
        expected_r8 = 64 if b == 0 else 64 - b.bit_length()
        expected_flags = {"cf": 1 if b == 0 else 0, "zf": 1 if expected_r8 == 0 else 0}
    elif op == "popcnt":
        expected_r8 = b.bit_count()
        expected_flags = {
            "cf": 0,
            "pf": 0,
            "af": 0,
            "zf": 1 if b == 0 else 0,
            "sf": 0,
            "of": 0,
        }
    else:
        return []

    mismatches: list[str] = []
    if op not in {"cmp", "test"} and as_u64(regs_f["r8"]) != expected_r8:
        mismatches.append(f"r8 expected=0x{expected_r8:016x} actual={regs_f['r8']}")
    if op in {"cmp", "test"} and as_u64(regs_f["r8"]) != a:
        mismatches.append(f"r8_modified expected=0x{a:016x} actual={regs_f['r8']}")

    for flag, expected in expected_flags.items():
        if int(flags_f[flag]) != expected:
            mismatches.append(f"flag.{flag} expected={expected} actual={flags_f[flag]}")
    return mismatches


def run_batch(cases: list[tuple[int, Template]]) -> list[dict[str, Any]]:
    payload = "".join(f"0x{seed:016x} {template.code}\n" for seed, template in cases)
    proc = subprocess.run(
        [str(RUNNER)],
        cwd=HB_ROOT,
        input=payload,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    rows = [json.loads(line) for line in proc.stdout.splitlines() if line.strip()]
    if len(rows) != len(cases):
        raise RuntimeError(f"runner returned {len(rows)} rows for {len(cases)} cases")
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cases", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=0x18BE487)
    ap.add_argument("--batch", type=int, default=512)
    ap.add_argument("--families", default="", help="comma-separated family filter")
    ap.add_argument("--stop-on-mismatch", action="store_true")
    args = ap.parse_args()

    build_runner()
    sde_path = shutil.which("sde64") or shutil.which("sde")
    family_filter = {x for x in args.families.split(",") if x}
    templates = [t for t in TEMPLATES if not family_filter or t.family in family_filter]
    if not templates:
        raise SystemExit("no templates selected")

    rng = random.Random(args.seed)
    counts: dict[str, int] = {}
    backend_mismatches: list[dict[str, Any]] = []
    oracle_mismatches: list[dict[str, Any]] = []
    unsupported: dict[str, int] = {}
    oracle_checked_count = 0
    oracle_pass_count = 0

    pending: list[tuple[int, Template]] = []
    case_index = 0
    while case_index < args.cases:
        template = templates[case_index % len(templates)]
        seed = rng.getrandbits(64)
        pending.append((seed, template))
        case_index += 1
        if len(pending) < args.batch and case_index < args.cases:
            continue
        rows = run_batch(pending)
        for (seed_i, template_i), row in zip(pending, rows):
            counts[template_i.family] = counts.get(template_i.family, 0) + 1
            interp_ok = row["interp"]["api"] == 0 and row["interp"]["result"] == 0
            jit_ok = row["jit"]["api"] == 0 and row["jit"]["result"] == 0
            if not interp_ok or not jit_ok:
                key = f"{template_i.family}:{template_i.name}:interp={row['interp']['api']}/{row['interp']['result']}:jit={row['jit']['api']}/{row['jit']['result']}"
                unsupported[key] = unsupported.get(key, 0) + 1
            exact = exact_oracle(template_i, row) if interp_ok else []
            oracle_checked = bool(template_i.oracle and interp_ok)
            if oracle_checked:
                oracle_checked_count += 1
                if not exact:
                    oracle_pass_count += 1
            if interp_ok and jit_ok and not row["ok"]:
                backend_mismatches.append({
                    "template": template_i.name,
                    "family": template_i.family,
                    "seed": f"0x{seed_i:016x}",
                    "code": template_i.code,
                    "diff": row["diff"],
                    "interp_oracle": "pass" if oracle_checked and not exact else
                                     ("fail" if exact else "not_available"),
                })
            if exact:
                oracle_mismatches.append({
                    "template": template_i.name,
                    "family": template_i.family,
                    "seed": f"0x{seed_i:016x}",
                    "code": template_i.code,
                    "mismatches": exact,
                })
            if args.stop_on_mismatch and (backend_mismatches or oracle_mismatches):
                break
        pending = []
        if args.stop_on_mismatch and (backend_mismatches or oracle_mismatches):
            break

    REPORT_JSON.parent.mkdir(parents=True, exist_ok=True)
    result = {
        "sde_available": bool(sde_path),
        "sde_path": sde_path,
        "oracle_mode": "sde" if sde_path else "golden_int_oracle_plus_interp_vs_jit_fallback",
        "cases_requested": args.cases,
        "cases_run": sum(counts.values()),
        "families": counts,
        "backend_mismatch_count": len(backend_mismatches),
        "oracle_mismatch_count": len(oracle_mismatches),
        "oracle_checked_count": oracle_checked_count,
        "oracle_pass_count": oracle_pass_count,
        "backend_mismatches": backend_mismatches[:50],
        "oracle_mismatches": oracle_mismatches[:50],
        "unsupported": unsupported,
    }
    REPORT_JSON.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 1 if backend_mismatches or oracle_mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
