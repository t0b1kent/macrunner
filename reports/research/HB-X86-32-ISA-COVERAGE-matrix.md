# HB x86-32 ISA Coverage Matrix

Date: 2026-06-07
Scope: HyperBridge `hb_decode_x86` and `hb_lift_x86` vs Capstone semantic comparison.

## Executive Summary

- **Total test cases generated & analyzed:** 11077
- **Capstone valid instructions:** 10613
- **HyperBridge decoded:** 10322 (97.26% of valid)
- **Length matches:** 10267 (99.47% of decoded)
- **Mnemonic/Opcode matches:** 10080 (97.66% of decoded)
- **Operands structurally matches:** 9031 (87.49% of decoded)
- **Semantic validation pass:** 258 / 1340 checked (19.25%)

## Coverage Gap Matrix by Opcode Group

| Priority | Opcode Family | Decoded / Valid | Length Match | Mnemonic Match | Operands Match | Semantic Pass | Status / Remaining Gaps |
|---|---|---|---|---|---|---|---|
| P1 | MOVZX / MOVSX Zero/Sign-Extend | 173 / 197 | 169 / 173 | 173 / 173 | 164 / 173 | 59 / 164 | 🟡 SEMANTIC GAP |
| P2 | MUL / IMUL / DIV / IDIV Arithmetic | 252 / 275 | 251 / 252 | 252 / 252 | 237 / 252 | 38 / 200 | 🟡 SEMANTIC GAP |
| P3 | ADC / SBB Carry Arithmetic | 334 / 374 | 334 / 334 | 314 / 334 | 286 / 334 | 7 / 200 | 🟡 MISMATCH |
| P4 | ROL / ROR / RCL / RCR Shift-Rotates | 373 / 373 | 349 / 373 | 373 / 373 | 152 / 373 | 0 / 152 | 🟡 SEMANTIC GAP |
| P5 | SHLD / SHRD Double-Precision Shifts | 42 / 66 | 38 / 42 | 42 / 42 | 34 / 42 | 0 / 34 | 🟡 SEMANTIC GAP |
| P6 | BT / BTS / BTR / BTC / BSF / BSR / BSWAP Bit-Ops | 128 / 204 | 118 / 128 | 128 / 128 | 100 / 128 | 10 / 100 | 🟡 SEMANTIC GAP |
| P7 | MOVS / CMPS / LODS / STOS / SCAS String Operations | 56 / 56 | 56 / 56 | 37 / 56 | 0 / 56 | N/A | 🟡 MISMATCH |
| P8 | x87 FPU Floating-Point | 446 / 458 | 446 / 446 | 412 / 446 | 97 / 446 | 13 / 81 | 🟡 MISMATCH |
| P9 | SSE / SSE2 SIMD Vector | 676 / 706 | 665 / 676 | 579 / 676 | 505 / 676 | 17 / 200 | 🟡 MISMATCH |
| P10 | Legacy i386-only (PUSHA, POPA, BCD, BOUND, ARPL, LDS/LES/LFS/LGS) | 71 / 71 | 71 / 71 | 69 / 71 | 10 / 71 | 0 / 9 | 🟡 MISMATCH |
| P11 | Other Base Integer / Control-Flow | 7771 / 7833 | 7770 / 7771 | 7701 / 7771 | 7446 / 7771 | 114 / 200 | 🟡 MISMATCH |

## Prioritized Implementation Action List

Action list for PE32 Lane CPU bring-up (Phase 3):

1. **P1 - MOVZX / MOVSX Zero/Sign-Extend**: Missing decode for 24 instructions.
1. **P1 - MOVZX / MOVSX Zero/Sign-Extend**: Semantic execution mismatch against Unicorn for 105 checked cases.
1. **P2 - MUL / IMUL / DIV / IDIV Arithmetic**: Missing decode for 23 instructions.
1. **P2 - MUL / IMUL / DIV / IDIV Arithmetic**: Semantic execution mismatch against Unicorn for 162 checked cases.
1. **P3 - ADC / SBB Carry Arithmetic**: Missing decode for 40 instructions.
1. **P3 - ADC / SBB Carry Arithmetic**: Mnemonic/Opcode mismatch for 20 decoded instructions.
1. **P3 - ADC / SBB Carry Arithmetic**: Semantic execution mismatch against Unicorn for 193 checked cases.
1. **P4 - ROL / ROR / RCL / RCR Shift-Rotates**: Semantic execution mismatch against Unicorn for 152 checked cases.
1. **P5 - SHLD / SHRD Double-Precision Shifts**: Missing decode for 24 instructions.
1. **P5 - SHLD / SHRD Double-Precision Shifts**: Semantic execution mismatch against Unicorn for 34 checked cases.
1. **P6 - BT / BTS / BTR / BTC / BSF / BSR / BSWAP Bit-Ops**: Missing decode for 76 instructions.
1. **P6 - BT / BTS / BTR / BTC / BSF / BSR / BSWAP Bit-Ops**: Semantic execution mismatch against Unicorn for 90 checked cases.
1. **P7 - MOVS / CMPS / LODS / STOS / SCAS String Operations**: Mnemonic/Opcode mismatch for 19 decoded instructions.
1. **P8 - x87 FPU Floating-Point**: Missing decode for 12 instructions.
1. **P8 - x87 FPU Floating-Point**: Mnemonic/Opcode mismatch for 34 decoded instructions.
1. **P8 - x87 FPU Floating-Point**: Semantic execution mismatch against Unicorn for 68 checked cases.
1. **P9 - SSE / SSE2 SIMD Vector**: Missing decode for 30 instructions.
1. **P9 - SSE / SSE2 SIMD Vector**: Mnemonic/Opcode mismatch for 97 decoded instructions.
1. **P9 - SSE / SSE2 SIMD Vector**: Semantic execution mismatch against Unicorn for 183 checked cases.
1. **P10 - Legacy i386-only (PUSHA, POPA, BCD, BOUND, ARPL, LDS/LES/LFS/LGS)**: Mnemonic/Opcode mismatch for 2 decoded instructions.
1. **P10 - Legacy i386-only (PUSHA, POPA, BCD, BOUND, ARPL, LDS/LES/LFS/LGS)**: Semantic execution mismatch against Unicorn for 9 checked cases.
1. **P11 - Other Base Integer / Control-Flow**: Missing decode for 62 instructions.
1. **P11 - Other Base Integer / Control-Flow**: Mnemonic/Opcode mismatch for 70 decoded instructions.
1. **P11 - Other Base Integer / Control-Flow**: Semantic execution mismatch against Unicorn for 86 checked cases.

## Concrete Mismatch and Gap Examples

Below are detailed mismatch details (bytes, Capstone expected disassembly, and MacRunner decoder actual output) for debugging and implementation:

### MOVZX / MOVSX Zero/Sign-Extend (P1) Gaps
- **Case:** `sys/movx/rep/b6/reg`
  - **Bytes:** `f30fb6c0`
  - **Capstone Expected:** `movzx eax, al` (len: 4)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/movx/repne/b6/reg`
  - **Bytes:** `f20fb6c0`
  - **Capstone Expected:** `movzx eax, al` (len: 4)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/movx/67/b6/mem`
  - **Bytes:** `670fb600`
  - **Capstone Expected:** `movzx eax, byte ptr [bx + si]` (len: 4)
  - **HyperBridge Actual:** `MOVZX (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Mem base mismatch: HB=0, CS=3*
- **Case:** `sys/movx/rep/b6/mem`
  - **Bytes:** `f30fb600`
  - **Capstone Expected:** `movzx eax, byte ptr [eax]` (len: 4)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*

### MUL / IMUL / DIV / IDIV Arithmetic (P2) Gaps
- **Case:** `sys/muldiv/67/f6/4/mem`
  - **Bytes:** `67f620`
  - **Capstone Expected:** `mul byte ptr [bx + si]` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/muldiv/67/f6/4/sib`
  - **Bytes:** `67f624`
  - **Capstone Expected:** `mul byte ptr [si]` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/muldiv/67/f6/5/mem`
  - **Bytes:** `67f628`
  - **Capstone Expected:** `imul byte ptr [bx + si]` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/muldiv/67/f6/5/sib`
  - **Bytes:** `67f62c`
  - **Capstone Expected:** `imul byte ptr [si]` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*

### ADC / SBB Carry Arithmetic (P3) Gaps
- **Case:** `sys/adcsbb/base/67/10/mem`
  - **Bytes:** `671000`
  - **Capstone Expected:** `adc byte ptr [bx + si], al` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/adcsbb/base/lock/10/mem`
  - **Bytes:** `f01000`
  - **Capstone Expected:** `lock adc byte ptr [eax], al` (len: 3)
  - **HyperBridge Actual:** `ADC` (len: 3)
  - **Reason:** *op mismatch (HB=adc, CS=lock adc)*
- **Case:** `sys/adcsbb/base/67/10/sib`
  - **Bytes:** `671004`
  - **Capstone Expected:** `adc byte ptr [si], al` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/adcsbb/base/lock/10/sib`
  - **Bytes:** `f0100424`
  - **Capstone Expected:** `lock adc byte ptr [esp], al` (len: 4)
  - **HyperBridge Actual:** `ADC` (len: 4)
  - **Reason:** *op mismatch (HB=adc, CS=lock adc)*

### ROL / ROR / RCL / RCR Shift-Rotates (P4) Gaps
- **Case:** `sys/rotate/none/c0/0/reg`
  - **Bytes:** `c0c0c0`
  - **Capstone Expected:** `rol al, 0xc0` (len: 3)
  - **HyperBridge Actual:** `ROL (op match: True)` (len: 3)
  - **Reason:** *Operands mismatch: Op 1 immediate mismatch: HB=0, CS=192*
- **Case:** `sys/rotate/66/c0/0/reg`
  - **Bytes:** `66c0c0c0`
  - **Capstone Expected:** `rol al, 0xc0` (len: 4)
  - **HyperBridge Actual:** `ROL (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Op 1 immediate mismatch: HB=0, CS=192*
- **Case:** `sys/rotate/67/c0/0/reg`
  - **Bytes:** `67c0c0c0`
  - **Capstone Expected:** `rol al, 0xc0` (len: 4)
  - **HyperBridge Actual:** `ROL (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Op 1 immediate mismatch: HB=0, CS=192*
- **Case:** `sys/rotate/rep/c0/0/reg`
  - **Bytes:** `f3c0c0c0`
  - **Capstone Expected:** `rol al, 0xc0` (len: 4)
  - **HyperBridge Actual:** `ROL (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Op 1 immediate mismatch: HB=0, CS=192*

### SHLD / SHRD Double-Precision Shifts (P5) Gaps
- **Case:** `sys/shldshrd/rep/a4/reg`
  - **Bytes:** `f30fa4c0c0`
  - **Capstone Expected:** `shld eax, eax, 0xc0` (len: 5)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/shldshrd/repne/a4/reg`
  - **Bytes:** `f20fa4c0c0`
  - **Capstone Expected:** `shld eax, eax, 0xc0` (len: 5)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/shldshrd/67/a4/mem`
  - **Bytes:** `670fa400c0`
  - **Capstone Expected:** `shld dword ptr [bx + si], eax, 0xc0` (len: 5)
  - **HyperBridge Actual:** `SHLD (op match: True)` (len: 5)
  - **Reason:** *Operands mismatch: Mem base mismatch: HB=0, CS=3*
- **Case:** `sys/shldshrd/rep/a4/mem`
  - **Bytes:** `f30fa400c0`
  - **Capstone Expected:** `shld dword ptr [eax], eax, 0xc0` (len: 5)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*

### BT / BTS / BTR / BTC / BSF / BSR / BSWAP Bit-Ops (P6) Gaps
- **Case:** `sys/bit/rep/a3/reg`
  - **Bytes:** `f30fa3c0`
  - **Capstone Expected:** `bt eax, eax` (len: 4)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/bit/repne/a3/reg`
  - **Bytes:** `f20fa3c0`
  - **Capstone Expected:** `bt eax, eax` (len: 4)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/bit/67/a3/mem`
  - **Bytes:** `670fa300`
  - **Capstone Expected:** `bt dword ptr [bx + si], eax` (len: 4)
  - **HyperBridge Actual:** `BT (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Mem base mismatch: HB=0, CS=3*
- **Case:** `sys/bit/rep/a3/mem`
  - **Bytes:** `f30fa300`
  - **Capstone Expected:** `bt dword ptr [eax], eax` (len: 4)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*

### MOVS / CMPS / LODS / STOS / SCAS String Operations (P7) Gaps
- **Case:** `sys/string/none/a4`
  - **Bytes:** `a4`
  - **Capstone Expected:** `movsb byte ptr es:[edi], byte ptr [esi]` (len: 1)
  - **HyperBridge Actual:** `MOVS (op match: True)` (len: 1)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=reg, CS=3*
- **Case:** `sys/string/66/a4`
  - **Bytes:** `66a4`
  - **Capstone Expected:** `movsb byte ptr es:[edi], byte ptr [esi]` (len: 2)
  - **HyperBridge Actual:** `MOVS (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=reg, CS=3*
- **Case:** `sys/string/67/a4`
  - **Bytes:** `67a4`
  - **Capstone Expected:** `movsb byte ptr es:[di], byte ptr [si]` (len: 2)
  - **HyperBridge Actual:** `MOVS (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=reg, CS=3*
- **Case:** `sys/string/rep/a4`
  - **Bytes:** `f3a4`
  - **Capstone Expected:** `rep movsb byte ptr es:[edi], byte ptr [esi]` (len: 2)
  - **HyperBridge Actual:** `MOVS (op match: False)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=reg, CS=3*

### x87 FPU Floating-Point (P8) Gaps
- **Case:** `sys/x87/reg/d8/c0`
  - **Bytes:** `d8c0`
  - **Capstone Expected:** `fadd st(0)` (len: 2)
  - **HyperBridge Actual:** `X87_FADD (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=imm, CS=1*
- **Case:** `sys/x87/reg/d8/c1`
  - **Bytes:** `d8c1`
  - **Capstone Expected:** `fadd st(1)` (len: 2)
  - **HyperBridge Actual:** `X87_FADD (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=imm, CS=1*
- **Case:** `sys/x87/reg/d8/c2`
  - **Bytes:** `d8c2`
  - **Capstone Expected:** `fadd st(2)` (len: 2)
  - **HyperBridge Actual:** `X87_FADD (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=imm, CS=1*
- **Case:** `sys/x87/reg/d8/c3`
  - **Bytes:** `d8c3`
  - **Capstone Expected:** `fadd st(3)` (len: 2)
  - **HyperBridge Actual:** `X87_FADD (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=imm, CS=1*

### SSE / SSE2 SIMD Vector (P9) Gaps
- **Case:** `sys/sse/sse_mov/none/10/reg`
  - **Bytes:** `0f10c0`
  - **Capstone Expected:** `movups xmm0, xmm0` (len: 3)
  - **HyperBridge Actual:** `SSE_MOV (op match: False)` (len: 3)
  - **Reason:** *Operands mismatch: Op 0 register mismatch: HB=17, CS=xmm0 (expected 0)*
- **Case:** `sys/sse/sse_mov/none/10/reg`
  - **Bytes:** `0f10c0`
  - **Capstone Expected:** `movups xmm0, xmm0` (len: 3)
  - **HyperBridge Actual:** `SSE_MOV` (len: 3)
  - **Reason:** *op mismatch (HB=sse_mov, CS=movups)*
- **Case:** `sys/sse/sse_mov/66/10/reg`
  - **Bytes:** `660f10c0`
  - **Capstone Expected:** `movupd xmm0, xmm0` (len: 4)
  - **HyperBridge Actual:** `SSE_MOV (op match: False)` (len: 4)
  - **Reason:** *Operands mismatch: Op 0 register mismatch: HB=17, CS=xmm0 (expected 0)*
- **Case:** `sys/sse/sse_mov/66/10/reg`
  - **Bytes:** `660f10c0`
  - **Capstone Expected:** `movupd xmm0, xmm0` (len: 4)
  - **HyperBridge Actual:** `SSE_MOV` (len: 4)
  - **Reason:** *op mismatch (HB=sse_mov, CS=movupd)*

### Legacy i386-only (PUSHA, POPA, BCD, BOUND, ARPL, LDS/LES/LFS/LGS) (P10) Gaps
- **Case:** `legacy/pusha`
  - **Bytes:** `60`
  - **Capstone Expected:** `pushal` (len: 1)
  - **HyperBridge Actual:** `PUSHA (op match: False)` (len: 1)
  - **Reason:** *Operands mismatch: Operand count mismatch: HB has 1, CS has 0*
- **Case:** `legacy/pusha`
  - **Bytes:** `60`
  - **Capstone Expected:** `pushal` (len: 1)
  - **HyperBridge Actual:** `PUSHA` (len: 1)
  - **Reason:** *op mismatch (HB=pusha, CS=pushal)*
- **Case:** `legacy/popa`
  - **Bytes:** `61`
  - **Capstone Expected:** `popal` (len: 1)
  - **HyperBridge Actual:** `POPA` (len: 1)
  - **Reason:** *op mismatch (HB=popa, CS=popal)*
- **Case:** `legacy/bound_eax_mem`
  - **Bytes:** `6200`
  - **Capstone Expected:** `bound eax, qword ptr [eax]` (len: 2)
  - **HyperBridge Actual:** `BOUND (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 1 size mismatch: HB=4, CS=8*

### Other Base Integer / Control-Flow (P11) Gaps
- **Case:** `sys/base/67/88/mem`
  - **Bytes:** `678800`
  - **Capstone Expected:** `mov byte ptr [bx + si], al` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/base/67/88/sib`
  - **Bytes:** `678804`
  - **Capstone Expected:** `mov byte ptr [si], al` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/base/67/89/mem`
  - **Bytes:** `678900`
  - **Capstone Expected:** `mov dword ptr [bx + si], eax` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
- **Case:** `sys/base/67/89/sib`
  - **Bytes:** `678904`
  - **Capstone Expected:** `mov dword ptr [si], eax` (len: 3)
  - **HyperBridge Actual:** `DECODE_FAIL (-5)` (len: 0)
  - **Reason:** *Opcode not supported in decoder (HB_ERR=-5)*
