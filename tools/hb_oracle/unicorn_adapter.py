#!/usr/bin/env python3
"""Unicorn-backed x86/x86-64 single-instruction oracle for HyperBridge fuzzing.

Supports both 32-bit (i386) and 64-bit modes via the optional `arch` argument
("x86" -> UC_MODE_32, "x64" -> UC_MODE_64). Default is "x64" for back-compat.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from typing import Any

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_MODE_64
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RSP, UC_X86_REG_RBP,
    UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_R10, UC_X86_REG_R11,
    UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15,
    UC_X86_REG_RIP, UC_X86_REG_EFLAGS,
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_ESP, UC_X86_REG_EBP,
    UC_X86_REG_EIP,
)
import unicorn.x86_const as x86c


CODE_BASE = 0x100000
DATA_BASE = 0x70000000
STACK_BASE = 0x71000000
DATA_SIZE = 0x2000
STACK_SIZE = 0x2000
FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1
RFLAGS_FUZZ_MASK = 0xCD5

GPR_NAMES_64 = [
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rsp", "rbp",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
]

GPR_REGS_64 = [
    UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
    UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RSP, UC_X86_REG_RBP,
    UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_R10, UC_X86_REG_R11,
    UC_X86_REG_R12, UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15,
]

# 32-bit register name + ID lists. Keep the 64-bit name as the public key in
# `snapshot()` so the diff_case_runner (which keys by "rax","rsp",...) matches
# transparently — the C side stores both names via `print_regs_json`.
GPR_NAMES_32 = [
    "eax", "ebx", "ecx", "edx", "esi", "edi", "esp", "ebp",
]
GPR_REGS_32 = [
    UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
    UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_ESP, UC_X86_REG_EBP,
]
# Map the 64-bit key ("rax" etc.) onto the 32-bit register constant for 32-bit
# runs. The diff_case_runner uses 64-bit names even in 32-bit mode for keys.
GPR_REGS_32_BY_64KEY = {
    "rax": UC_X86_REG_EAX, "rbx": UC_X86_REG_EBX, "rcx": UC_X86_REG_ECX,
    "rdx": UC_X86_REG_EDX, "rsi": UC_X86_REG_ESI, "rdi": UC_X86_REG_EDI,
    "rsp": UC_X86_REG_ESP, "rbp": UC_X86_REG_EBP,
}

# Back-compat aliases for direct callers.
GPR_NAMES = GPR_NAMES_64
GPR_REGS = GPR_REGS_64

FLAG_BITS = {
    "cf": 0x001,
    "pf": 0x004,
    "af": 0x010,
    "zf": 0x040,
    "sf": 0x080,
    "of": 0x800,
}

HB_FLAG_MASKS = {
    "zf": 1 << 0,
    "sf": 1 << 1,
    "cf": 1 << 2,
    "of": 1 << 3,
    "pf": 1 << 4,
    "af": 1 << 5,
}

PRESERVED_RFLAGS_MASK = 0x600


@dataclass
class InitialState:
    regs: dict[str, int]
    rflags: int
    xmm: list[bytes]
    ymm_hi: list[bytes]
    data: bytes
    stack: bytes


def splitmix64_next(state: list[int]) -> int:
    state[0] = (state[0] + 0x9E3779B97F4A7C15) & MASK64
    z = state[0]
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return (z ^ (z >> 31)) & MASK64


def random_bytes(state: list[int], size: int) -> bytes:
    out = bytearray()
    while len(out) < size:
        out.extend(splitmix64_next(state).to_bytes(8, "little"))
    return bytes(out[:size])


def fnv1a64(data: bytes) -> int:
    h = FNV64_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV64_PRIME) & MASK64
    return h


def bytes_to_hex(data: bytes) -> str:
    return data.hex()


def hex64(v: int) -> str:
    return f"0x{v & MASK64:016x}"


def flags_from_eflags(eflags: int) -> dict[str, int]:
    return {name: 1 if (eflags & bit) else 0 for name, bit in FLAG_BITS.items()}


def build_initial_state(seed: int, arch: str = "x64") -> InitialState:
    """Build deterministic state matching hb_diff_case_runner init_context().

    64-bit mirrors the C-side init_context (R* are random, RSP/RBP into stack,
    RSI/RDI into data). 32-bit mirrors the i386 init_context (EAX=EBP+0x1000
    data pointer, EBP=stack+0x1100, ESP=stack+0x1000, ESI=0x800, EDI=0x1000,
    EBX/ECX random, EDX=0).
    """
    rng = [seed & MASK64]
    data = random_bytes(rng, DATA_SIZE)
    stack = random_bytes(rng, STACK_SIZE)
    regs: dict[str, int] = {}
    if arch == "x86":
        regs["eax"] = (DATA_BASE + 0x1000) & 0xFFFFFFFF
        regs["ebx"] = splitmix64_next(rng) & 0xFFFFFFFF
        regs["ecx"] = (splitmix64_next(rng) & 0x0F) + 1
        regs["edx"] = 0
        regs["esi"] = (DATA_BASE + 0x0800) & 0xFFFFFFFF
        regs["edi"] = (DATA_BASE + 0x1000) & 0xFFFFFFFF
        regs["esp"] = (STACK_BASE + 0x1000) & 0xFFFFFFFF
        regs["ebp"] = (STACK_BASE + 0x1100) & 0xFFFFFFFF
    else:
        regs = {
            "rax": DATA_BASE + 0x1000,
            "rbx": splitmix64_next(rng),
            "rcx": (splitmix64_next(rng) & 0x3F) + 1,
            "rdx": splitmix64_next(rng),
            "rsi": DATA_BASE + 0x0400,
            "rdi": DATA_BASE + 0x1000,
            "rsp": STACK_BASE + 0x1000,
            "rbp": STACK_BASE + 0x1100,
            "r8": splitmix64_next(rng),
            "r9": splitmix64_next(rng),
            "r10": splitmix64_next(rng),
            "r11": splitmix64_next(rng),
            "r12": splitmix64_next(rng),
            "r13": splitmix64_next(rng),
            "r14": splitmix64_next(rng),
            "r15": splitmix64_next(rng),
        }
    rflags = (splitmix64_next(rng) & RFLAGS_FUZZ_MASK) | 0x202
    xmm: list[bytes] = []
    ymm_hi: list[bytes] = []
    for _ in range(16):
        xmm.append(random_bytes(rng, 16))
        ymm_hi.append(random_bytes(rng, 16))
    return InitialState(regs=regs, rflags=rflags, xmm=xmm, ymm_hi=ymm_hi, data=data, stack=stack)


def reg_const(prefix: str, index: int) -> int:
    return getattr(x86c, f"UC_X86_REG_{prefix}{index}")


def set_initial_state(uc: Uc, state: InitialState, code: bytes, arch: str = "x64") -> None:
    uc.mem_map(CODE_BASE, 0x1000)
    uc.mem_write(CODE_BASE, code)
    uc.mem_map(DATA_BASE, DATA_SIZE)
    uc.mem_write(DATA_BASE, state.data)
    uc.mem_map(STACK_BASE, STACK_SIZE)
    uc.mem_write(STACK_BASE, state.stack)
    if arch == "x86":
        for name in GPR_NAMES_32:
            uc.reg_write(GPR_REGS_32[GPR_NAMES_32.index(name)], state.regs[name])
        uc.reg_write(UC_X86_REG_EIP, CODE_BASE)
    else:
        for name, reg in zip(GPR_NAMES_64, GPR_REGS_64):
            uc.reg_write(reg, state.regs[name])
        uc.reg_write(UC_X86_REG_RIP, CODE_BASE)
    uc.reg_write(UC_X86_REG_EFLAGS, state.rflags)
    # Prime XMM state. In 64-bit mode we also prime YMM (Unicorn 2.1.4
    # rejects YMM writes in UC_MODE_32; for 32-bit we keep XMM-only).
    if arch == "x86":
        for i in range(8):
            uc.reg_write(reg_const("XMM", i), int.from_bytes(state.xmm[i], "little"))
    else:
        for i in range(16):
            ymm = state.xmm[i] + state.ymm_hi[i]
            uc.reg_write(reg_const("YMM", i), int.from_bytes(ymm, "little"))


def snapshot(uc: Uc, arch: str = "x64") -> dict[str, Any]:
    if arch == "x86":
        regs: dict[str, str] = {}
        for name, reg in zip(GPR_NAMES_32, GPR_REGS_32):
            regs[name] = f"0x{uc.reg_read(reg) & 0xFFFFFFFF:08x}"
        regs["eip"] = f"0x{uc.reg_read(UC_X86_REG_EIP) & 0xFFFFFFFF:08x}"
    else:
        regs = {name: hex64(uc.reg_read(reg)) for name, reg in zip(GPR_NAMES_64, GPR_REGS_64)}
        regs["rip"] = hex64(uc.reg_read(UC_X86_REG_RIP))
    eflags = uc.reg_read(UC_X86_REG_EFLAGS)
    xmm: list[str] = []
    ymm_hi: list[str] = []
    if arch == "x86":
        for i in range(8):
            raw = int(uc.reg_read(reg_const("XMM", i))).to_bytes(16, "little")
            xmm.append(bytes_to_hex(raw))
            ymm_hi.append("0" * 32)
    else:
        for i in range(16):
            raw = int(uc.reg_read(reg_const("YMM", i))).to_bytes(32, "little")
            xmm.append(bytes_to_hex(raw[:16]))
            ymm_hi.append(bytes_to_hex(raw[16:]))
    data = bytes(uc.mem_read(DATA_BASE, DATA_SIZE))
    stack = bytes(uc.mem_read(STACK_BASE, STACK_SIZE))
    # FPU state (Unicorn exposes CW via UC_X86_REG_FPCW/SW/TW since 2.0).
    # We capture these for x87-family comparisons; absence is non-fatal.
    fpu: dict[str, str] = {}
    try:
        fpu["cw"] = hex64(uc.reg_read(x86c.UC_X86_REG_FPCW) & 0xFFFF)
    except Exception:
        pass
    try:
        fpu["sw"] = hex64(uc.reg_read(x86c.UC_X86_REG_FPSW) & 0xFFFF)
    except Exception:
        pass
    try:
        fpu["tw"] = hex64(uc.reg_read(x86c.UC_X86_REG_FPTW) & 0xFFFF)
    except Exception:
        pass
    out = {
        "api": 0,
        "result": 0,
        "regs": regs,
        "rflags": hex64(eflags),
        "flags": flags_from_eflags(eflags),
        "xmm": xmm,
        "ymm_hi": ymm_hi,
        "data_hash": hex64(fnv1a64(data)),
        "stack_hash": hex64(fnv1a64(stack)),
    }
    if fpu:
        out["fpu"] = fpu
    return out


def run_case(seed: int, code_hex: str, arch: str = "x64") -> dict[str, Any]:
    code = bytes.fromhex(code_hex)
    state = build_initial_state(seed, arch)
    mode = UC_MODE_32 if arch == "x86" else UC_MODE_64
    uc = Uc(UC_ARCH_X86, mode)
    try:
        set_initial_state(uc, state, code, arch)
        uc.emu_start(CODE_BASE, CODE_BASE + len(code))
        out = snapshot(uc, arch)
        out["ok"] = True
        return out
    except UcError as exc:
        return {
            "ok": False,
            "api": -1,
            "result": -1,
            "error": exc.__class__.__name__,
            "message": str(exc),
        }


def parse_hex_int(v: str) -> int:
    return int(v, 16) if isinstance(v, str) and v.startswith("0x") else int(v)


def diff_interpreter(row: dict[str, Any], oracle: dict[str, Any],
                     defined_flags: set[str] | frozenset[str] | None = None,
                     arch: str = "x64") -> list[str]:
    if not oracle.get("ok"):
        return [f"oracle_error:{oracle.get('message', oracle.get('error', 'unknown'))}"]
    interp = row["interp"]
    mismatches: list[str] = []
    if interp.get("api") != 0 or interp.get("result") != 0:
        mismatches.append(f"interp_error:{interp.get('api')}/{interp.get('result')}")
        return mismatches
    if arch == "x86":
        gpr_names = GPR_NAMES_32 + ["eip"]
    else:
        gpr_names = GPR_NAMES_64 + ["rip"]
    for name in gpr_names:
        if interp["regs"].get(name) != oracle["regs"].get(name):
            mismatches.append(f"reg.{name} expected={oracle['regs'].get(name)} actual={interp['regs'].get(name)}")
    mask = parse_hex_int(interp.get("flag_mask", "0x3f"))
    for name, hb_mask in HB_FLAG_MASKS.items():
        if defined_flags is not None:
            should_compare = name in defined_flags
        else:
            should_compare = (mask & hb_mask) != 0
        if should_compare:
            if int(interp["flags"].get(name, 0)) != int(oracle["flags"].get(name, 0)):
                mismatches.append(f"flag.{name} expected={oracle['flags'].get(name)} actual={interp['flags'].get(name)}")
    interp_rflags = parse_hex_int(interp.get("rflags", "0"))
    oracle_rflags = parse_hex_int(oracle.get("rflags", "0"))
    if (interp_rflags ^ oracle_rflags) & PRESERVED_RFLAGS_MASK:
        mismatches.append(f"rflags.preserved expected={hex64(oracle_rflags & PRESERVED_RFLAGS_MASK)} actual={hex64(interp_rflags & PRESERVED_RFLAGS_MASK)}")
    # 32-bit fpu cases: only first 8 xmm regs are populated by the C snapshot,
    # but the Unicorn side returns 16. Compare the overlap; ignore ymm_hi for
    # 32-bit (x86 SSE only has 8 XMM registers and the C side doesn't report
    # ymm_hi separately in 32-bit).
    if arch == "x86":
        for i, (actual, expected) in enumerate(zip(interp.get("xmm", [])[:8],
                                                    oracle.get("xmm", [])[:8])):
            if actual != expected:
                mismatches.append(f"xmm{i} expected={expected} actual={actual}")
    else:
        for i, (actual, expected) in enumerate(zip(interp.get("xmm", []), oracle.get("xmm", []))):
            if actual != expected:
                mismatches.append(f"xmm{i} expected={expected} actual={actual}")
        for i, (actual, expected) in enumerate(zip(interp.get("ymm_hi", []), oracle.get("ymm_hi", []))):
            if actual != expected:
                mismatches.append(f"ymm_hi{i} expected={expected} actual={actual}")
    for key in ("data_hash", "stack_hash"):
        if interp.get(key) != oracle.get(key):
            mismatches.append(f"{key} expected={oracle.get(key)} actual={interp.get(key)}")
    return mismatches


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("seed")
    ap.add_argument("code_hex")
    args = ap.parse_args()
    result = run_case(parse_hex_int(args.seed), args.code_hex)
    print(json.dumps(result, indent=2))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
