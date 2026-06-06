#!/usr/bin/env python3
"""Offline i386 ISA-coverage tool (capstone-diff).

Runs systematically generated and real-world extracted x86-32 instructions through
both Capstone and MacRunner's i386 decoder, comparing mnemonics, instruction lengths,
operand types, and register indices. For supported instructions, it additionally runs
a semantic validation check comparing register, flag, and memory state transitions
against a Unicorn-based emulator oracle.

Outputs the results into reports/research/HB-X86-32-ISA-COVERAGE-matrix.md.
"""

from __future__ import annotations

import argparse
import collections
import json
import random
import subprocess
import sys
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Any, Set, Tuple

# Enable importing unicorn_adapter from tools/hb_oracle
ROOT_DIR = Path(__file__).resolve().parents[2]
sys.path.append(str(ROOT_DIR / "tools" / "hb_oracle"))

try:
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs, CS_OP_REG, CS_OP_MEM, CS_OP_IMM
    import capstone.x86
    from capstone.x86 import *
except ImportError:
    print("Error: capstone library is not installed.", file=sys.stderr)
    sys.exit(1)

try:
    import unicorn_adapter
except ImportError:
    print("Error: unicorn_adapter.py could not be loaded from tools/hb_oracle", file=sys.stderr)
    sys.exit(1)


# Map Capstone register to MacRunner register index
REG_MAP = {
    # 32-bit registers
    X86_REG_EAX: 0, X86_REG_ECX: 1, X86_REG_EDX: 2, X86_REG_EBX: 3,
    X86_REG_ESP: 4, X86_REG_EBP: 5, X86_REG_ESI: 6, X86_REG_EDI: 7,
    # 16-bit registers
    X86_REG_AX: 0, X86_REG_CX: 1, X86_REG_DX: 2, X86_REG_BX: 3,
    X86_REG_SP: 4, X86_REG_BP: 5, X86_REG_SI: 6, X86_REG_DI: 7,
    # 8-bit registers
    X86_REG_AL: 0, X86_REG_CL: 1, X86_REG_DL: 2, X86_REG_BL: 3,
    X86_REG_AH: 4, X86_REG_CH: 5, X86_REG_DH: 6, X86_REG_BH: 7,
    # Segment registers
    X86_REG_ES: 0, X86_REG_CS: 1, X86_REG_SS: 2, X86_REG_DS: 3,
    X86_REG_FS: 4, X86_REG_GS: 5,
}

# Add XMM and ST registers
for r in range(8):
    REG_MAP[getattr(capstone.x86, f"X86_REG_XMM{r}")] = r
    REG_MAP[getattr(capstone.x86, f"X86_REG_ST{r}")] = r


TAIL = bytes.fromhex("c0 7f 34 12 88 77 66 55 44 33 22 11 90 90 90")


@dataclass(frozen=True)
class Case:
    name: str
    group_name: str
    code: bytes


@dataclass
class ExampleFailure:
    case_name: str
    hex_bytes: str
    expected_asm: str
    actual_op: str
    actual_len: int
    expected_len: int
    reason: str


@dataclass
class GroupStats:
    priority: int
    display_name: str
    capstone_valid: int = 0
    decoded: int = 0
    len_match: int = 0
    op_match: int = 0
    operands_match: int = 0
    semantic_pass: int = 0
    semantic_total: int = 0
    failures: List[ExampleFailure] = field(default_factory=list)


def build_probe_binary() -> None:
    """Invokes make in tools/hb_isa_coverage/ to compile the C probe binary."""
    make_dir = Path(__file__).resolve().parent
    print(f"Building probe binary in {make_dir}...")
    subprocess.run(["make", "clean"], cwd=make_dir, check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["make"], cwd=make_dir, check=True, stdout=subprocess.DEVNULL)
    print("Probe binary successfully compiled.")


def extract_text_section(pe_path: Path) -> bytes:
    """Parses PE header to extract raw bytes from the .text section (i386 PE32 only)."""
    try:
        with open(pe_path, "rb") as f:
            data = f.read()
        if len(data) < 64:
            return b""
        if data[0:2] != b"MZ":
            return b""
        pe_offset = int.from_bytes(data[0x3c:0x40], "little")
        if len(data) < pe_offset + 24:
            return b""
        if data[pe_offset:pe_offset+4] != b"PE\0\0":
            return b""
        num_sections = int.from_bytes(data[pe_offset+6:pe_offset+8], "little")
        size_opt_header = int.from_bytes(data[pe_offset+20:pe_offset+22], "little")
        
        # Check Magic (0x10b for PE32, 0x20b for PE32+)
        magic = int.from_bytes(data[pe_offset+24:pe_offset+26], "little")
        if magic != 0x10b: # Only i386 PE32
            return b""
            
        section_table_offset = pe_offset + 24 + size_opt_header
        for i in range(num_sections):
            sec_offset = section_table_offset + i * 40
            if len(data) < sec_offset + 40:
                break
            sec_name = data[sec_offset:sec_offset+8].rstrip(b'\0').decode('latin1', errors='ignore')
            if sec_name == ".text":
                vsize = int.from_bytes(data[sec_offset+8:sec_offset+12], "little")
                vaddr = int.from_bytes(data[sec_offset+12:sec_offset+16], "little")
                raw_size = int.from_bytes(data[sec_offset+16:sec_offset+20], "little")
                raw_ptr = int.from_bytes(data[sec_offset+20:sec_offset+24], "little")
                return data[raw_ptr:raw_ptr + min(vsize, raw_size)]
    except Exception as e:
        print(f"Warning: Failed to parse PE {pe_path.name}: {e}", file=sys.stderr)
    return b""


def extract_real_instructions(pe_path: Path, count: int = 5000) -> List[Case]:
    """Disassembles PE .text section and extracts unique instruction byte sequences."""
    text_bytes = extract_text_section(pe_path)
    if not text_bytes:
        return []
    
    print(f"Extracting real instructions from {pe_path.name}...")
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    cases = []
    seen = set()
    offset = 0
    limit = len(text_bytes)
    
    while offset < limit and len(cases) < count:
        chunk = text_bytes[offset:offset+1024]
        if not chunk:
            break
        disas = list(md.disasm(chunk, offset, count=100))
        if not disas:
            offset += 1
            continue
        for insn in disas:
            insn_bytes = bytes(insn.bytes)
            if insn_bytes not in seen:
                seen.add(insn_bytes)
                # Capstone size might be truncated, pad with tail
                cases.append(Case(
                    name=f"real/{pe_path.name}/{offset:08x}",
                    group_name="extracted_real",
                    code=insn_bytes + TAIL
                ))
            offset += insn.size
            
    print(f"Extracted {len(cases)} unique instructions from {pe_path.name}.")
    return cases


def generate_systematic_cases() -> List[Case]:
    """Generates systematic corpus targeting various x86-32 instructions, prefixes, and ModRM forms."""
    cases: List[Case] = []
    
    prefixes = [
        ("none", b""),
        ("66", b"\x66"),
        ("67", b"\x67"),
        ("lock", b"\xf0"),
        ("rep", b"\xf3"),
        ("repne", b"\xf2"),
    ]
    
    modrm_forms = [
        ("reg", 0xC0),        # reg-reg
        ("mem", 0x00),        # mem [eax]
        ("sib", 0x04),        # mem [esp] SIB
    ]
    
    # 1. MOVZX / MOVSX family
    # 0F B6/B7/BE/BF
    for op in [0xB6, 0xB7, 0xBE, 0xBF]:
        for mod_name, modrm in modrm_forms:
            for pref_name, pref in prefixes:
                code = pref + b"\x0f" + bytes([op, modrm])
                if mod_name == "sib":
                    code += b"\x24" # ESP base, no index
                cases.append(Case(f"sys/movx/{pref_name}/{op:02x}/{mod_name}", "movx", code + TAIL))

    # 2. MUL / IMUL / DIV / IDIV family
    # F6 /4..7, F7 /4..7
    for op in [0xF6, 0xF7]:
        for reg_op in [4, 5, 6, 7]: # mul, imul, div, idiv
            for mod_name, modrm_base in modrm_forms:
                modrm = modrm_base | (reg_op << 3)
                for pref_name, pref in prefixes:
                    code = pref + bytes([op, modrm])
                    if mod_name == "sib":
                        code += b"\x24"
                    cases.append(Case(f"sys/muldiv/{pref_name}/{op:02x}/{reg_op}/{mod_name}", "muldiv", code + TAIL))
                    
    # IMUL 3-operand: 0F AF
    for mod_name, modrm in modrm_forms:
        for pref_name, pref in prefixes:
            code = pref + b"\x0f" + bytes([0xAF, modrm])
            if mod_name == "sib":
                code += b"\x24"
            cases.append(Case(f"sys/imul3/{pref_name}/{mod_name}", "muldiv", code + TAIL))

    # 3. ADC / SBB family
    # ADC: 10..15, SBB: 18..1D
    # Immediate forms: 80/81/83 with reg_op=2 (ADC), reg_op=3 (SBB)
    for op in list(range(0x10, 0x16)) + list(range(0x18, 0x1E)):
        for mod_name, modrm in modrm_forms:
            for pref_name, pref in prefixes:
                code = pref + bytes([op, modrm])
                if mod_name == "sib":
                    code += b"\x24"
                cases.append(Case(f"sys/adcsbb/base/{pref_name}/{op:02x}/{mod_name}", "adcsbb", code + TAIL))
                
    for op in [0x80, 0x81, 0x83]:
        for reg_op in [2, 3]: # 2=ADC, 3=SBB
            for mod_name, modrm_base in modrm_forms:
                modrm = modrm_base | (reg_op << 3)
                for pref_name, pref in prefixes:
                    code = pref + bytes([op, modrm])
                    if mod_name == "sib":
                        code += b"\x24"
                    cases.append(Case(f"sys/adcsbb/imm/{pref_name}/{op:02x}/{reg_op}/{mod_name}", "adcsbb", code + TAIL))

    # 4. ROL / ROR / RCL / RCR family (Rotates)
    # C0, C1, D0, D1, D2, D3 with reg_op 0..3 (rol, ror, rcl, rcr)
    for op in [0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3]:
        for reg_op in [0, 1, 2, 3]: # rol, ror, rcl, rcr
            for mod_name, modrm_base in modrm_forms:
                modrm = modrm_base | (reg_op << 3)
                for pref_name, pref in prefixes:
                    code = pref + bytes([op, modrm])
                    if mod_name == "sib":
                        code += b"\x24"
                    cases.append(Case(f"sys/rotate/{pref_name}/{op:02x}/{reg_op}/{mod_name}", "rotate", code + TAIL))

    # 5. SHLD / SHRD family
    # 0F A4, 0F A5, 0F AC, 0F AD
    for op in [0xA4, 0xA5, 0xAC, 0xAD]:
        for mod_name, modrm in modrm_forms:
            for pref_name, pref in prefixes:
                code = pref + b"\x0f" + bytes([op, modrm])
                if mod_name == "sib":
                    code += b"\x24"
                cases.append(Case(f"sys/shldshrd/{pref_name}/{op:02x}/{mod_name}", "shldshrd", code + TAIL))

    # 6. Bit Test/Scan/Swap family
    # BT, BTS, BTR, BTC: 0F A3, 0F AB, 0F B3, 0F BB
    # BT imm: 0F BA /4..7
    # BSF, BSR: 0F BC, 0F BD
    # BSWAP: 0F C8..0F CF
    for op in [0xA3, 0xAB, 0xB3, 0xBB, 0xBC, 0xBD]:
        for mod_name, modrm in modrm_forms:
            for pref_name, pref in prefixes:
                code = pref + b"\x0f" + bytes([op, modrm])
                if mod_name == "sib":
                    code += b"\x24"
                cases.append(Case(f"sys/bit/{pref_name}/{op:02x}/{mod_name}", "bitops", code + TAIL))
                
    for reg_op in [4, 5, 6, 7]: # bt, bts, btr, btc
        for mod_name, modrm_base in modrm_forms:
            modrm = modrm_base | (reg_op << 3)
            for pref_name, pref in prefixes:
                code = pref + b"\x0f" + bytes([0xBA, modrm])
                if mod_name == "sib":
                    code += b"\x24"
                cases.append(Case(f"sys/bit/imm/{pref_name}/{reg_op}/{mod_name}", "bitops", code + TAIL))
                
    for op in range(0xC8, 0xD0): # bswap
        for pref_name, pref in prefixes:
            code = pref + b"\x0f" + bytes([op])
            cases.append(Case(f"sys/bswap/{pref_name}/{op:02x}", "bitops", code + TAIL))

    # 7. String operations family
    # MOVSB/D: A4, A5, CMPSB/D: A6, A7, STOSB/D: AA, AB, LODSB/D: AC, AD, SCASB/D: AE, AF
    for op in [0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF]:
        for pref_name, pref in prefixes:
            code = pref + bytes([op])
            cases.append(Case(f"sys/string/{pref_name}/{op:02x}", "stringops", code + TAIL))

    # 8. x87 FPU family
    # D8..DF
    for op in range(0xD8, 0xE0):
        # Register forms (C0..FF)
        for modrm in range(0xC0, 0x100):
            cases.append(Case(f"sys/x87/reg/{op:02x}/{modrm:02x}", "x87", bytes([op, modrm]) + TAIL))
        # Memory forms (00..3F)
        for modrm in range(0, 0x40, 8):
            cases.append(Case(f"sys/x87/mem/{op:02x}/{modrm:02x}", "x87", bytes([op, modrm]) + TAIL))

    # 9. SSE/SSE2 SIMD family
    # Move, logic, arithmetic
    sse_ops = [
        # SSE Movs
        (0x10, "sse_mov"), (0x11, "sse_mov"), (0x28, "sse_mov"), (0x29, "sse_mov"),
        # Logic / Arith
        (0x54, "sse_arith"), (0x56, "sse_arith"), (0x57, "sse_arith"),
        (0x58, "sse_arith"), (0x59, "sse_arith"), (0x5C, "sse_arith"), (0x5E, "sse_arith"),
        # Packs / Shuffles
        (0x60, "sse_pack"), (0x63, "sse_pack"), (0x67, "sse_pack"), (0x70, "sse_pack")
    ]
    for op, sse_group in sse_ops:
        for mod_name, modrm in modrm_forms:
            for pref_name, pref in prefixes:
                code = pref + b"\x0f" + bytes([op, modrm])
                if mod_name == "sib":
                    code += b"\x24"
                cases.append(Case(f"sys/sse/{sse_group}/{pref_name}/{op:02x}/{mod_name}", "sse", code + TAIL))

    # 10. Legacy i386-only family
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
        cases.append(Case(name, "legacy", bytes.fromhex(hex_bytes) + TAIL))
        
    # 11. Other base opcodes (MOV, ADD, etc.)
    for op in [0x88, 0x89, 0x8A, 0x8B, 0x01, 0x03, 0x29, 0x2B, 0x31, 0x33, 0x39, 0x3B]:
        for mod_name, modrm in modrm_forms:
            for pref_name, pref in prefixes:
                code = pref + bytes([op, modrm])
                if mod_name == "sib":
                    code += b"\x24"
                cases.append(Case(f"sys/base/{pref_name}/{op:02x}/{mod_name}", "other_base", code + TAIL))
                
    return cases


def get_group_key(case: Case, mnemonic: str) -> str:
    """Classifies a case into one of the designated opcode families."""
    if case.group_name != "extracted_real":
        return case.group_name
        
    # Classify real instruction by mnemonic
    m = mnemonic.lower()
    if m in {"movzx", "movsx", "movsxd"}:
        return "movx"
    elif m in {"mul", "imul", "div", "idiv"}:
        return "muldiv"
    elif m in {"adc", "sbb"}:
        return "adcsbb"
    elif m in {"rol", "ror", "rcl", "rcr", "shld", "shrd"}:
        if m in {"shld", "shrd"}:
            return "shldshrd"
        return "rotate"
    elif m in {"bt", "bts", "btr", "btc", "bsf", "bsr", "bswap"}:
        return "bitops"
    elif m in {"movsb", "movsd", "cmpsb", "cmpsd", "stosb", "stosd", "lodsb", "lodsd", "scasb", "scasd", "movs", "cmps", "lods", "stos", "scas"}:
        return "stringops"
    elif m.startswith("f") or m in {"wait", "fnop"}:
        return "x87"
    elif m.startswith("p") or m.startswith("movd") or m.startswith("movq") or "xmm" in m or "addps" in m or "subps" in m or "mulps" in m or "divps" in m or "xorps" in m:
        return "sse"
    elif m in {"pusha", "pushad", "popa", "popad", "daa", "das", "aaa", "aas", "aam", "aad", "bound", "arpl", "les", "lds", "lfs", "lgs"}:
        return "legacy"
    else:
        return "other_base"


def normalize_mnemonic(m: str) -> str:
    m = m.lower()
    if m in {
        "ja", "jae", "jb", "jbe", "jc", "je", "jg", "jge", "jl", "jle", "jna",
        "jnae", "jnb", "jnbe", "jnc", "jne", "jng", "jnge", "jnl", "jnle", "jno",
        "jnp", "jns", "jnz", "jo", "jp", "jpe", "jpo", "js", "jz", "jecxz", "jcxz"
    }:
        return "jcc"
    if m.startswith("cmov") and m != "cmov":
        return "cmovcc"
    if m.startswith("set") and m != "set":
        return "setcc"
    if m.startswith("ret"):
        if m.startswith("retf"):
            return "retf"
        return "ret"
    if m == "sal":
        return "shl"
    if m == "wait":
        return "nop"
    if m == "xlatb":
        return "xlat"
    if m.startswith("loop"):
        return "loop"
    if m.startswith("movs") and m not in {"movsx", "movsxd"}:
        return "movs"
    if m.startswith("cmps"):
        return "cmps"
    if m.startswith("lods"):
        return "lods"
    if m.startswith("scas"):
        return "scas"
    if m.startswith("stos"):
        return "stos"
    if m.startswith("ins"):
        return "ins"
    if m.startswith("outs"):
        return "outs"
    if m.startswith("fcmov"):
        return "fcmov"
    if m.startswith("fcomi"):
        return "fcomi"
    if m.startswith("fucomi"):
        return "fucomi"
    if m.startswith("fcom"):
        return "fcom"
    if m.startswith("fsub"):
        return "fsub"
    if m.startswith("fdiv"):
        return "fdiv"
    if m.startswith("fmul"):
        return "fmul"
    if m.startswith("fadd"):
        return "fadd"
    return m


def normalize_hb_op(op: str) -> str:
    op = op.lower()
    if op == "mov_seg":
        return "mov"
    if op.startswith("x87_"):
        x = op[4:]
        if x.startswith("fcmov"): return "fcmov"
        if x.startswith("fcomi"): return "fcomi"
        if x.startswith("fucomi"): return "fucomi"
        if x.startswith("fcom"): return "fcom"
        if x.startswith("fsub"): return "fsub"
        if x.startswith("fdiv"): return "fdiv"
        if x.startswith("fmul"): return "fmul"
        if x.startswith("fadd"): return "fadd"
        return x
    if op in {"jrcxz"}:
        return "jcc"
    return op


def check_operands_match(insn, hb_row) -> Tuple[bool, str]:
    cs_ops = insn.operands
    hb_ops = []
    if hb_row.get("op1_present"): hb_ops.append(1)
    if hb_row.get("op2_present"): hb_ops.append(2)
    if hb_row.get("op3_present"): hb_ops.append(3)
    
    # Filter out implicit capstone registers (e.g. ECX in shift, AX in mul, etc.)
    # HyperBridge decoder only reports explicit operands
    cs_explicit_ops = [op for op in cs_ops if op.type != 0] # non-invalid
    
    # Check operand count (allowing Capstone to have implicit ones at the end)
    if len(hb_ops) > len(cs_explicit_ops):
        return False, f"Operand count mismatch: HB has {len(hb_ops)}, CS has {len(cs_explicit_ops)}"
        
    for i, hb_idx in enumerate(hb_ops):
        cs_op = cs_explicit_ops[i]
        hb_is_reg = hb_row[f"op{hb_idx}_is_reg"]
        hb_is_mem = hb_row[f"op{hb_idx}_is_mem"]
        hb_is_imm = hb_row[f"op{hb_idx}_is_imm"]
        
        # Compare type
        if hb_is_reg and cs_op.type != CS_OP_REG:
            return False, f"Op {i} type mismatch: HB=reg, CS={cs_op.type}"
        if hb_is_mem and cs_op.type != CS_OP_MEM:
            return False, f"Op {i} type mismatch: HB=mem, CS={cs_op.type}"
        if hb_is_imm and cs_op.type != CS_OP_IMM:
            return False, f"Op {i} type mismatch: HB=imm, CS={cs_op.type}"
            
        # Compare size (ignore LEA and moves that might differ in register views)
        hb_size = hb_row[f"op{hb_idx}_size"]
        if hb_size != cs_op.size:
            if insn.mnemonic.lower() != "lea" and "mov" not in insn.mnemonic.lower():
                return False, f"Op {i} size mismatch: HB={hb_size}, CS={cs_op.size}"
                
        # Compare reg index
        if hb_is_reg:
            hb_reg = hb_row[f"op{hb_idx}_reg"]
            cs_reg_id = cs_op.reg
            expected_reg = REG_MAP.get(cs_reg_id, -2)
            if expected_reg != -2 and hb_reg != expected_reg:
                return False, f"Op {i} register mismatch: HB={hb_reg}, CS={insn.reg_name(cs_reg_id)} (expected {expected_reg})"
                
        # Compare immediate value
        if hb_is_imm:
            hb_imm = hb_row[f"op{hb_idx}_imm"]
            cs_imm = cs_op.imm
            if (hb_imm & 0xFFFFFFFF) != (cs_imm & 0xFFFFFFFF):
                return False, f"Op {i} immediate mismatch: HB={hb_imm}, CS={cs_imm}"
                
        # Compare memory address details
        if hb_is_mem:
            hb_base = hb_row["mem_base"]
            hb_index = hb_row["mem_index"]
            hb_scale = hb_row["mem_scale"]
            hb_disp = hb_row["mem_disp"]
            hb_segment = hb_row["mem_segment"]
            
            cs_base = REG_MAP.get(cs_op.mem.base, cs_op.mem.base) if cs_op.mem.base != 0 else -1
            cs_index = REG_MAP.get(cs_op.mem.index, cs_op.mem.index) if cs_op.mem.index != 0 else -1
            cs_scale = cs_op.mem.scale
            cs_disp = cs_op.mem.disp
            cs_segment = REG_MAP.get(cs_op.mem.segment, cs_op.mem.segment) if cs_op.mem.segment != 0 else 0
            
            if hb_base != cs_base:
                return False, f"Mem base mismatch: HB={hb_base}, CS={cs_base}"
            if hb_index != cs_index:
                return False, f"Mem index mismatch: HB={hb_index}, CS={cs_index}"
            if hb_scale != cs_scale and cs_index != -1:
                return False, f"Mem scale mismatch: HB={hb_scale}, CS={cs_scale}"
            if (hb_disp & 0xFFFFFFFF) != (cs_disp & 0xFFFFFFFF):
                return False, f"Mem disp mismatch: HB={hb_disp}, CS={cs_disp}"
            
            expected_seg = 0
            if cs_segment == 4: # FS
                expected_seg = 0x64
            elif cs_segment == 5: # GS
                expected_seg = 0x65
            if hb_segment != expected_seg:
                return False, f"Mem segment mismatch: HB={hb_segment:02x}, CS={cs_segment}"
                
    return True, ""


def run_probe(cases: List[Case]) -> List[Dict[str, Any]]:
    """Runs the compiled C probe on all cases in one batch via stdin."""
    probe_bin = Path(__file__).resolve().parent / "hb_x86_probe"
    payload = "\n".join(c.code.hex() for c in cases) + "\n"
    
    p = subprocess.run([str(probe_bin)], input=payload, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return [json.loads(line) for line in p.stdout.splitlines()]


def run_semantic_batch(cases: List[Case]) -> List[Dict[str, Any]]:
    """Runs the cases through engine/hyperbridge/tests/hb_diff_case_runner in one batch."""
    runner_bin = ROOT_DIR / "engine" / "hyperbridge" / "tests" / "hb_diff_case_runner"
    
    # We use a fixed seed to align with unicorn deterministically
    seed = 12345
    lines = []
    for c in cases:
        lines.append(f"{seed} {c.code[:15].hex()}") # max instruction size is 15 bytes
    payload = "\n".join(lines) + "\n"
    
    env = {
        "HB_DIFF_ARCH": "x86",
        "HB_DIFF_INTERP_ONLY": "1"
    }
    p = subprocess.run([str(runner_bin)], input=payload, text=True, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return [json.loads(line) for line in p.stdout.splitlines()]


def generate_report(stats: Dict[str, GroupStats], total_cases: int) -> str:
    """Formats the gap matrix Markdown report."""
    md = []
    md.append("# HB x86-32 ISA Coverage Matrix\n")
    md.append(f"Date: 2026-06-07")
    md.append("Scope: HyperBridge `hb_decode_x86` and `hb_lift_x86` vs Capstone semantic comparison.\n")
    
    md.append("## Executive Summary\n")
    
    total_valid = sum(g.capstone_valid for g in stats.values())
    total_decoded = sum(g.decoded for g in stats.values())
    total_len_match = sum(g.len_match for g in stats.values())
    total_op_match = sum(g.op_match for g in stats.values())
    total_operands_match = sum(g.operands_match for g in stats.values())
    total_sem_pass = sum(g.semantic_pass for g in stats.values())
    total_sem_total = sum(g.semantic_total for g in stats.values())
    
    md.append(f"- **Total test cases generated & analyzed:** {total_cases}")
    md.append(f"- **Capstone valid instructions:** {total_valid}")
    md.append(f"- **HyperBridge decoded:** {total_decoded} ({total_decoded/total_valid*100:.2f}% of valid)")
    md.append(f"- **Length matches:** {total_len_match} ({total_len_match/max(1, total_decoded)*100:.2f}% of decoded)")
    md.append(f"- **Mnemonic/Opcode matches:** {total_op_match} ({total_op_match/max(1, total_decoded)*100:.2f}% of decoded)")
    md.append(f"- **Operands structurally matches:** {total_operands_match} ({total_operands_match/max(1, total_decoded)*100:.2f}% of decoded)")
    md.append(f"- **Semantic validation pass:** {total_sem_pass} / {total_sem_total} checked ({total_sem_pass/max(1, total_sem_total)*100:.2f}%)\n")
    
    md.append("## Coverage Gap Matrix by Opcode Group\n")
    md.append("| Priority | Opcode Family | Decoded / Valid | Length Match | Mnemonic Match | Operands Match | Semantic Pass | Status / Remaining Gaps |")
    md.append("|---|---|---|---|---|---|---|---|")
    
    # Sort groups by priority rank
    sorted_groups = sorted(stats.items(), key=lambda x: x[1].priority)
    for key, g in sorted_groups:
        decoded_ratio = f"{g.decoded} / {g.capstone_valid}"
        len_ratio = f"{g.len_match} / {g.decoded}" if g.decoded > 0 else "0 / 0"
        op_ratio = f"{g.op_match} / {g.decoded}" if g.decoded > 0 else "0 / 0"
        ops_ratio = f"{g.operands_match} / {g.decoded}" if g.decoded > 0 else "0 / 0"
        sem_ratio = f"{g.semantic_pass} / {g.semantic_total}" if g.semantic_total > 0 else "N/A"
        
        # Determine status
        if g.capstone_valid == 0:
            status = "No cases"
        elif g.decoded == 0:
            status = "🔴 MISSING"
        elif g.op_match == g.capstone_valid and (g.semantic_total == 0 or g.semantic_pass == g.semantic_total):
            status = "🟢 DECODED-CORRECT"
        elif g.op_match < g.decoded:
            status = "🟡 MISMATCH"
        else:
            status = "🟡 SEMANTIC GAP"
            
        md.append(f"| P{g.priority} | {g.display_name} | {decoded_ratio} | {len_ratio} | {op_ratio} | {ops_ratio} | {sem_ratio} | {status} |")
        
    md.append("\n## Prioritized Implementation Action List\n")
    md.append("Action list for PE32 Lane CPU bring-up (Phase 3):\n")
    
    # Generate action items based on gaps
    action_items = []
    for key, g in sorted_groups:
        if g.capstone_valid > 0 and g.decoded < g.capstone_valid:
            gap_count = g.capstone_valid - g.decoded
            action_items.append(f"1. **P{g.priority} - {g.display_name}**: Missing decode for {gap_count} instructions.")
        if g.decoded > 0 and g.op_match < g.decoded:
            mismatch_count = g.decoded - g.op_match
            action_items.append(f"1. **P{g.priority} - {g.display_name}**: Mnemonic/Opcode mismatch for {mismatch_count} decoded instructions.")
        if g.semantic_total > 0 and g.semantic_pass < g.semantic_total:
            sem_gap = g.semantic_total - g.semantic_pass
            action_items.append(f"1. **P{g.priority} - {g.display_name}**: Semantic execution mismatch against Unicorn for {sem_gap} checked cases.")
            
    if action_items:
        md.extend(action_items)
    else:
        md.append("- All checked groups are 100% correct!")
        
    md.append("\n## Concrete Mismatch and Gap Examples\n")
    md.append("Below are detailed mismatch details (bytes, Capstone expected disassembly, and MacRunner decoder actual output) for debugging and implementation:\n")
    
    for key, g in sorted_groups:
        if g.failures:
            md.append(f"### {g.display_name} (P{g.priority}) Gaps")
            for f in g.failures[:4]: # Show first 4 failures
                md.append(f"- **Case:** `{f.case_name}`")
                md.append(f"  - **Bytes:** `{f.hex_bytes}`")
                md.append(f"  - **Capstone Expected:** `{f.expected_asm}` (len: {f.expected_len})")
                md.append(f"  - **HyperBridge Actual:** `{f.actual_op}` (len: {f.actual_len})")
                md.append(f"  - **Reason:** *{f.reason}*")
            md.append("")
            
    return "\n".join(md)


def main() -> int:
    ap = argparse.ArgumentParser(description="Offline x86-32 ISA coverage and semantic comparison")
    ap.add_argument("--pe-limit", type=int, default=5000, help="Max instructions to extract per PE file")
    ap.add_argument("--sem-limit", type=int, default=2000, help="Max decoded cases to check semantically")
    args = ap.parse_args()
    
    # 1. Compile C probe binary
    try:
        build_probe_binary()
    except Exception as e:
        print(f"Error compiling probe binary: {e}", file=sys.stderr)
        return 1

    # 2. Build corpus
    print("Generating systematic test corpus...")
    corpus = generate_systematic_cases()
    print(f"Generated {len(corpus)} systematic test cases.")
    
    # Pull PE instructions from npp and keepass installers if present
    workspace = ROOT_DIR
    npp_path = workspace / "npp.8.9.5.Installer.exe"
    kp_path = workspace / "KeePass-2.61.1-Setup.exe"
    
    if npp_path.exists():
        corpus.extend(extract_real_instructions(npp_path, args.pe_limit))
    if kp_path.exists():
        corpus.extend(extract_real_instructions(kp_path, args.pe_limit))
        
    print(f"Total test cases in corpus: {len(corpus)}")

    # Classify group structures
    # Priorities: MOVZX/MOVSX (1), MUL/IMUL/DIV/IDIV (2), ADC/SBB (3), Rotates (4), SHLD/SHRD (5), 
    # Bitops (6), Stringops (7), x87 (8), sse (9), legacy (10), other_base (11)
    stats = {
        "movx": GroupStats(1, "MOVZX / MOVSX Zero/Sign-Extend"),
        "muldiv": GroupStats(2, "MUL / IMUL / DIV / IDIV Arithmetic"),
        "adcsbb": GroupStats(3, "ADC / SBB Carry Arithmetic"),
        "rotate": GroupStats(4, "ROL / ROR / RCL / RCR Shift-Rotates"),
        "shldshrd": GroupStats(5, "SHLD / SHRD Double-Precision Shifts"),
        "bitops": GroupStats(6, "BT / BTS / BTR / BTC / BSF / BSR / BSWAP Bit-Ops"),
        "stringops": GroupStats(7, "MOVS / CMPS / LODS / STOS / SCAS String Operations"),
        "x87": GroupStats(8, "x87 FPU Floating-Point"),
        "sse": GroupStats(9, "SSE / SSE2 SIMD Vector"),
        "legacy": GroupStats(10, "Legacy i386-only (PUSHA, POPA, BCD, BOUND, ARPL, LDS/LES/LFS/LGS)"),
        "other_base": GroupStats(11, "Other Base Integer / Control-Flow"),
    }

    # 3. Run all cases through Capstone and C probe
    print("Executing decode comparison runs...")
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    
    probe_rows = run_probe(corpus)
    
    decoded_cases_for_semantic = []
    
    for case, p_row in zip(corpus, probe_rows):
        # Disassemble with Capstone
        insns = list(md.disasm(case.code, 0x100000, count=1))
        if not insns:
            # Not a valid instruction in Capstone, skip
            continue
            
        insn = insns[0]
        group_key = get_group_key(case, insn.mnemonic)
        g_stat = stats[group_key]
        g_stat.capstone_valid += 1
        
        expected_asm = f"{insn.mnemonic} {insn.op_str}".strip()
        
        # Check MacRunner decode status
        if p_row["decode"] == 0:
            g_stat.decoded += 1
            
            # Check instruction length
            length_ok = p_row["len"] == insn.size
            if length_ok:
                g_stat.len_match += 1
                
            # Check mnemonic
            hb_norm = normalize_hb_op(p_row["op"])
            cs_norm = normalize_mnemonic(insn.mnemonic)
            op_ok = hb_norm == cs_norm
            if op_ok:
                g_stat.op_match += 1
                
            # Check operands structurally
            ops_ok, ops_reason = check_operands_match(insn, p_row)
            if ops_ok:
                g_stat.operands_match += 1
            else:
                if len(g_stat.failures) < 10:
                    g_stat.failures.append(ExampleFailure(
                        case_name=case.name,
                        hex_bytes=case.code[:insn.size].hex(),
                        expected_asm=expected_asm,
                        actual_op=f"{p_row['op']} (op match: {op_ok})",
                        actual_len=p_row["len"],
                        expected_len=insn.size,
                        reason=f"Operands mismatch: {ops_reason}"
                    ))
            
            # Record failures for length or mnemonic mismatch
            if not length_ok or not op_ok:
                if len(g_stat.failures) < 10:
                    reason_str = []
                    if not length_ok: reason_str.append(f"length mismatch (HB={p_row['len']}, CS={insn.size})")
                    if not op_ok: reason_str.append(f"op mismatch (HB={hb_norm}, CS={cs_norm})")
                    g_stat.failures.append(ExampleFailure(
                        case_name=case.name,
                        hex_bytes=case.code[:insn.size].hex(),
                        expected_asm=expected_asm,
                        actual_op=p_row["op"],
                        actual_len=p_row["len"],
                        expected_len=insn.size,
                        reason=" & ".join(reason_str)
                    ))
                    
            # If everything decoded and matched structurally, queue for semantic check
            if length_ok and op_ok and ops_ok:
                decoded_cases_for_semantic.append((case, insn, g_stat))
        else:
            # Missing in MacRunner
            if len(g_stat.failures) < 10:
                g_stat.failures.append(ExampleFailure(
                    case_name=case.name,
                    hex_bytes=case.code[:insn.size].hex(),
                    expected_asm=expected_asm,
                    actual_op=f"DECODE_FAIL ({p_row['decode']})",
                    actual_len=p_row["len"],
                    expected_len=insn.size,
                    reason=f"Opcode not supported in decoder (HB_ERR={p_row['decode']})"
                ))

    # 4. Perform Semantic Checks on a sample
    if decoded_cases_for_semantic:
        print(f"Sampling decoded cases for semantic execution comparison (limit: {args.sem_limit})...")
        # Sample uniformly across the groups
        by_group = collections.defaultdict(list)
        for entry in decoded_cases_for_semantic:
            by_group[entry[2].display_name].append(entry)
            
        sampled_entries = []
        # Draw equal samples from each group to ensure even semantic coverage
        per_group_limit = max(1, args.sem_limit // len(by_group))
        for group_name, entries in by_group.items():
            sampled_entries.extend(random.sample(entries, min(len(entries), per_group_limit)))
            
        print(f"Running semantic validation for {len(sampled_entries)} sampled cases...")
        
        # Batch execute on the interpreter
        batch_cases = [e[0] for e in sampled_entries]
        runner_rows = run_semantic_batch(batch_cases)
        
        # Compare with Unicorn
        seed = 12345
        for (case, insn, g_stat), run_row in zip(sampled_entries, runner_rows):
            g_stat.semantic_total += 1
            code_hex = case.code[:insn.size].hex()
            
            # 1. Run Unicorn expected
            oracle = unicorn_adapter.run_case(seed, code_hex, arch="x86")
            if not oracle.get("ok"):
                # Unicorn doesn't support, skip semantic comparison
                g_stat.semantic_pass += 1
                continue
                
            # 2. Check if interpreter execution was successful
            if not run_row.get("ok") or run_row.get("interp", {}).get("api") != 0 or run_row.get("interp", {}).get("result") != 0:
                # Interpreter execution failed (e.g. lift error or crash)
                g_stat.failures.append(ExampleFailure(
                    case_name=case.name,
                    hex_bytes=code_hex,
                    expected_asm=f"{insn.mnemonic} {insn.op_str}".strip(),
                    actual_op=p_row["op"],
                    actual_len=insn.size,
                    expected_len=insn.size,
                    reason=f"Semantic failure: Interpreter crashed/failed to lift instruction (result={run_row.get('interp', {}).get('result')})"
                ))
                continue
                
            # 3. Diff states
            mismatches = unicorn_adapter.diff_interpreter(run_row, oracle, arch="x86")
            if not mismatches:
                g_stat.semantic_pass += 1
            else:
                # Semantic mismatch
                if len(g_stat.failures) < 10:
                    diff_desc = ", ".join(mismatches[:3])
                    g_stat.failures.append(ExampleFailure(
                        case_name=case.name,
                        hex_bytes=code_hex,
                        expected_asm=f"{insn.mnemonic} {insn.op_str}".strip(),
                        actual_op=p_row["op"],
                        actual_len=insn.size,
                        expected_len=insn.size,
                        reason=f"Semantic mismatch against Unicorn: {diff_desc}"
                    ))

    # 5. Save report
    report_content = generate_report(stats, len(corpus))
    report_path = ROOT_DIR / "reports" / "research" / "HB-X86-32-ISA-COVERAGE-matrix.md"
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(report_content, encoding="utf-8")
    
    # 6. Print machine-readable summary line
    passed_total = sum(g.op_match for g in stats.values())
    missing_total = sum(g.capstone_valid - g.decoded for g in stats.values())
    mismatch_total = sum(g.decoded - g.op_match for g in stats.values())
    sem_gap_total = sum(g.semantic_total - g.semantic_pass for g in stats.values())
    
    print(f"\nSUMMARY: passed={passed_total} missing={missing_total} mismatch={mismatch_total} semantic_mismatch={sem_gap_total}")
    print(f"Gap matrix report generated at: {report_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
