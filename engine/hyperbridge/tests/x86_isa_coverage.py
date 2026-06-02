#!/usr/bin/env python3
"""Bulk i386 (CS_MODE_32) decode/lift coverage against capstone.

Mirrors x64_isa_coverage.py for 32-bit mode. Drives hb_decode_x86 / hb_lift_x86
across:

  - one-byte opcodes (0x00..0xFF), 6 prefixes (none, 0x66, 0x67, 0xF0, 0xF2, 0xF3)
  - two-byte 0F xx map (256 opcodes × 6 prefixes)
  - 0F 38 xx, 0F 3A xx maps (3-byte opcodes × 6 prefixes)
  - x87 0xD8..0xDF (with all modrm bytes 0xC0..0xFF)
  - legacy i386-only opcodes (PUSHA/POPA, BCD, BOUND/ARPL, LDS/LES/LFS/LGS)
  - random valid streams (sanity)

The script compares the produced hb_decoded_t to capstone's first instruction;
if capstone decodes a valid instruction but HyperBridge returns a non-zero
status, that's a "missing" entry. The output summarises per-group decode /
length / mnemonic / lift ratios plus a list of the most common missing
mnemonics — same shape as the 64-bit report so they can be diffed.

Usage:
  python3 tests/x86_isa_coverage.py                # default 4000 random
  python3 tests/x86_isa_coverage.py --json         # emit JSON for diffing
  python3 tests/x86_isa_coverage.py --random 8000
"""

from __future__ import annotations

import argparse
import collections
import json
import random
import subprocess
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs


HB_ROOT = Path(__file__).resolve().parents[1]
PROBE_C = HB_ROOT / "tests" / "hb_x86_probe.c"
PROBE_BIN = HB_ROOT / "tests" / "hb_x86_probe"
LIB = HB_ROOT / "libhyperbridge.a"
TAIL = bytes.fromhex("c0 7f 34 12 88 77 66 55 44 33 22 11 90 90 90")


@dataclass(frozen=True)
class Case:
    name: str
    group: str
    code: bytes


def run(cmd: list[str], cwd: Path) -> None:
    subprocess.run(cmd, cwd=cwd, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def build_probe() -> None:
    run(["make", "libhyperbridge.a"], HB_ROOT)
    needs_build = not PROBE_BIN.exists()
    if not needs_build:
        newest_input = max(PROBE_C.stat().st_mtime, LIB.stat().st_mtime)
        needs_build = PROBE_BIN.stat().st_mtime < newest_input
    if needs_build:
        run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I./include",
             str(PROBE_C.relative_to(HB_ROOT)), "libhyperbridge.a", "-o",
             str(PROBE_BIN.relative_to(HB_ROOT))], HB_ROOT)


def prefixes() -> list[tuple[str, bytes]]:
    return [
        ("none", b""),
        ("66", b"\x66"),
        ("67", b"\x67"),
        ("lock", b"\xf0"),
        ("rep", b"\xf3"),
        ("repne", b"\xf2"),
    ]


def generate_cases(random_count: int) -> list[Case]:
    cases: list[Case] = []
    for prefix_name, prefix in prefixes():
        for opcode in range(256):
            if opcode in (0x0f,):
                continue
            cases.append(Case(f"one-byte/{prefix_name}/{opcode:02x}",
                              "legacy one-byte", prefix + bytes([opcode]) + TAIL))
    for prefix_name, prefix in prefixes():
        for opcode in range(256):
            cases.append(Case(f"0f/{prefix_name}/{opcode:02x}",
                              "0f map", prefix + b"\x0f" + bytes([opcode]) + TAIL))
    for prefix_name, prefix in prefixes():
        for opcode in range(256):
            cases.append(Case(f"0f38/{prefix_name}/{opcode:02x}",
                              "0f38 map", prefix + b"\x0f\x38" + bytes([opcode]) + TAIL))
            cases.append(Case(f"0f3a/{prefix_name}/{opcode:02x}",
                              "0f3a map", prefix + b"\x0f\x3a" + bytes([opcode]) + TAIL))
    for opcode in range(0xd8, 0xe0):
        for modrm in range(0xc0, 0x100):
            cases.append(Case(f"x87/{opcode:02x}/{modrm:02x}",
                              "x87", bytes([opcode, modrm]) + TAIL))

    # Legacy i386-only opcodes (removed in x64). These are the families the
    # AGENTS.md lane B-1 task added: PUSHA/POPA, BCD, BOUND/ARPL, LDS/LES/LFS/LGS.
    legacy = {
        "legacy/pusha":        "60",
        "legacy/popa":         "61",
        "legacy/daa":          "27",
        "legacy/das":          "2f",
        "legacy/aaa":          "37",
        "legacy/aas":          "3f",
        "legacy/aam_imm10":    "d4 0a",
        "legacy/aad_imm10":    "d5 0a",
        "legacy/bound_eax_mem":"62 00",
        "legacy/arpl_ax_cx":   "63 29 01",
        "legacy/les_eax_mem":  "c4 04 1e 00 08 00 70 00",
        "legacy/lds_eax_mem":  "c5 04 1e 00 08 00 70 00",
        "legacy/lfs_eax_mem":  "0f b4 04 1e 00 08 00 70 00",
        "legacy/lgs_eax_mem":  "0f b5 04 1e 00 08 00 70 00",
    }
    for name, hex_bytes in legacy.items():
        cases.append(Case(name, "legacy i386-only", bytes.fromhex(hex_bytes) + TAIL))

    rng = random.Random(0x4862)
    for i in range(random_count):
        n = rng.randint(1, 15)
        cases.append(Case(f"random/{i:04d}", "random valid capstone", bytes(rng.randrange(256) for _ in range(n))))
    return cases


COND_JUMPS = {
    "ja", "jae", "jb", "jbe", "jc", "je", "jg", "jge", "jl", "jle", "jna",
    "jnae", "jnb", "jnbe", "jnc", "jne", "jng", "jnge", "jnl", "jnle", "jno",
    "jnp", "jns", "jnz", "jo", "jp", "jpe", "jpo", "js", "jz",
}


def normalize_capstone(mnemonic: str) -> str:
    m = mnemonic.lower()
    if m in COND_JUMPS:
        return "jcc"
    if m.startswith("cmov") and m != "cmov":
        return "cmovcc"
    if m.startswith("set") and m != "set":
        return "setcc"
    if m.startswith("ret"):
        if m.startswith("retf"):
            return "retf"
        return "ret"
    if m.startswith("iret"):
        return "iret"
    if m in {"pushfq", "pushfd", "pushfw"}:
        return "pushf"
    if m in {"popfq", "popfd", "popfw"}:
        return "popf"
    if m == "fnop":
        return "nop"
    if m in {"feni8087_nop", "fdisi8087_nop", "fsetpm"}:
        return "nop"
    if m in {"sldt", "str", "lldt", "ltr", "verr", "verw", "sgdt", "sidt", "lgdt",
             "lidt", "smsw", "lmsw", "invlpg", "enclv", "lar", "lsl", "syscall",
             "clts", "sysret", "invd", "wbinvd", "wrmsr", "rdtsc", "rdmsr", "rdpmc",
             "sysenter", "sysexit", "getsec", "montmul", "xstore", "rsm",
             "vmread", "vmwrite", "rdfsbase", "rdgsbase", "wrfsbase", "wrgsbase",
             "popcnt"}:
        return "sys"
    if m in {"ud0", "ud1", "ud2"}:
        return "ud"
    if m in {"femms", "emms"}:
        return "mmx"
    if m.startswith("fcmov"):
        return "fcmov"
    if m == "fstpnce":
        return "fstp"
    if m == "fucompp":
        return "fcompp"
    if m in {"fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr"}:
        return "fi"
    if m in {"fchs", "fabs", "ftst", "fxam", "fld1", "fldl2t", "fldl2e", "fldpi",
             "fldlg2", "fldln2", "fldz", "f2xm1", "fyl2x", "fptan", "fpatan",
             "fxtract", "fprem1", "fdecstp", "fincstp", "fprem", "fyl2xp1",
             "fsqrt", "fsincos", "fscale", "fsin", "fcos", "fldenv", "fnstenv",
             "frstor", "fnsave", "fbld", "fbstp"}:
        return "misc"
    if m in {"insb", "insw", "insd"}:
        return "ins"
    if m in {"outsb", "outsw", "outsd"}:
        return "outs"
    if m == "xlatb":
        return "xlat"
    if m == "movabs":
        return "mov"
    if m.startswith("loop"):
        return "loop"
    if m.startswith("movs") and m != "movsx" and m != "movsxd":
        return "movs"
    if m.startswith("cmps"):
        return "cmps"
    if m.startswith("lods"):
        return "lods"
    if m.startswith("scas"):
        return "scas"
    if m.startswith("stos"):
        return "stos"
    if m in {"movaps", "movups", "movss", "movsd", "movapd", "movupd", "movdqa", "movdqu"}:
        return "sse_mov"
    if m in {"andps", "andpd", "pand"}:
        return "xmm_and"
    if m in {"andnps", "andnpd", "pandn"}:
        return "xmm_andn"
    if m in {"orps", "orpd", "por"}:
        return "xmm_or"
    # i386-specific normalizations
    if m in {"pusha", "pushad", "pushal"}:
        return "pusha"
    if m in {"popa", "popad", "popal"}:
        return "popa"
    if m in {"daa", "das", "aaa", "aas", "aam", "aad"}:
        return m
    if m == "bound":
        return "bound"
    if m == "arpl":
        return "arpl"
    if m in {"les", "lds", "lfs", "lgs"}:
        return m
    if m == "sal":
        return "shl"
    if m == "callf":
        return "call"
    if m == "jmpf":
        return "jmp"
    if m in {"enter", "leave"}:
        return m
    return m


def normalize_hb(op: str) -> str:
    low = op.lower()
    if low in {"mov_seg", "mov_cr", "mov_dr"}:
        return "mov"
    if low == "push_seg":
        return "push"
    if low == "pop_seg":
        return "pop"
    if low.startswith("x87_"):
        return low[4:]
    if low in {"pusha", "popa", "daa", "das", "aaa", "aas", "aam", "aad",
               "bound", "arpl", "les", "lds", "lfs", "lgs"}:
        return low
    # HyperBridge uses CWD as the umbrella for CWD/CDQ/CQO; size is the
    # discriminator. In 32-bit coverage it is reported as CDQ by Capstone.
    if low == "cwd":
        return "cdq"
    # JRCXZ in our 32-bit decoder is actually JECXZ (CX vs ECX is sized via op1).
    if low == "jrcxz":
        return "jecxz"
    return low


def capstone_first(md: Cs, code: bytes):
    insns = list(md.disasm(code, 0x100000, count=1))
    return insns[0] if insns else None


def probe(cases: list[Case]) -> list[dict]:
    payload = "\n".join(c.code.hex() for c in cases) + "\n"
    p = subprocess.run([str(PROBE_BIN)], input=payload, text=True, cwd=HB_ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return [json.loads(line) for line in p.stdout.splitlines()]


def summarize(cases: list[Case], rows: list[dict]) -> dict:
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    summary = collections.defaultdict(lambda: {
        "cases": 0,
        "capstone_valid": 0,
        "decoded": 0,
        "length_match": 0,
        "mnemonic_match": 0,
        "lifted": 0,
        "missing": collections.Counter(),
        "mismatch": collections.Counter(),
        "examples": [],
    })
    all_missing = collections.Counter()
    for case, row in zip(cases, rows):
        insn = capstone_first(md, case.code)
        group = summary[case.group]
        group["cases"] += 1
        if insn is None:
            continue
        group["capstone_valid"] += 1
        cap_norm = normalize_capstone(insn.mnemonic)
        if row["decode"] == 0:
            group["decoded"] += 1
            if row["len"] == insn.size:
                group["length_match"] += 1
            hb_norm = normalize_hb(row["op"])
            if hb_norm == cap_norm:
                group["mnemonic_match"] += 1
            else:
                group["mismatch"][f"{insn.mnemonic}->{row['op']}"] += 1
                if len(group["examples"]) < 6:
                    group["examples"].append({
                        "case": case.name,
                        "hex": case.code[:insn.size].hex(),
                        "capstone": f"{insn.mnemonic} {insn.op_str}".strip(),
                        "hb": row["op"],
                        "hb_len": row["len"],
                        "cs_len": insn.size,
                        "lift": row["lift"],
                    })
            if row["lift"] == 0:
                group["lifted"] += 1
        else:
            group["missing"][insn.mnemonic] += 1
            all_missing[insn.mnemonic] += 1
            if len(group["examples"]) < 6:
                group["examples"].append({
                    "case": case.name,
                    "hex": case.code[:insn.size].hex(),
                    "capstone": f"{insn.mnemonic} {insn.op_str}".strip(),
                    "hb": f"decode {row['decode']}",
                    "cs_len": insn.size,
                })
    result = {"groups": {}, "top_missing": all_missing.most_common(30)}
    for name, value in sorted(summary.items()):
        result["groups"][name] = {
            "cases": value["cases"],
            "capstone_valid": value["capstone_valid"],
            "decoded": value["decoded"],
            "length_match": value["length_match"],
            "mnemonic_match": value["mnemonic_match"],
            "lifted": value["lifted"],
            "top_missing": value["missing"].most_common(12),
            "top_mismatch": value["mismatch"].most_common(12),
            "examples": value["examples"],
        }
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--random", type=int, default=4000, help="random byte streams to include")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of compact text")
    args = ap.parse_args()

    build_probe()
    cases = generate_cases(args.random)
    rows = probe(cases)
    result = summarize(cases, rows)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for group, data in result["groups"].items():
            cv = data["capstone_valid"]
            print(f"{group}: capstone={cv} decoded={data['decoded']} "
                  f"len={data['length_match']} mnemonic={data['mnemonic_match']} "
                  f"lifted={data['lifted']}")
        print("top_missing:", ", ".join(f"{k}:{v}" for k, v in result["top_missing"][:12]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
