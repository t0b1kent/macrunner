# HB x86-64 ISA Coverage Matrix

Date: 2026-06-07
Scope: HyperBridge `hb_decode_x64` and `hb_lift_x64` vs Capstone semantic comparison.

## Executive Summary

- **Total test cases generated & analyzed:** 8644
- **Capstone valid instructions:** 8178
- **HyperBridge decoded:** 8178 (100.00% of valid)
- **Length matches:** 8178 (100.00% of decoded)
- **Mnemonic/Opcode matches:** 7247 (88.62% of decoded)
- **Operands structurally matches:** 4495 (54.96% of decoded)
- **Semantic validation pass:** 175 / 1294 checked (13.52%)

## Coverage Gap Matrix by Opcode Group

| Priority | Opcode Family | Decoded / Valid | Length Match | Mnemonic Match | Operands Match | Semantic Pass | Status / Remaining Gaps |
|---|---|---|---|---|---|---|---|
| P1 | MOVZX / MOVSX Zero/Sign-Extend | 118 / 118 | 118 / 118 | 118 / 118 | 112 / 118 | 19 / 112 | 🟡 SEMANTIC GAP |
| P2 | MUL / IMUL / DIV / IDIV Arithmetic | 162 / 162 | 162 / 162 | 162 / 162 | 159 / 162 | 48 / 159 | 🟡 SEMANTIC GAP |
| P3 | ADC / SBB Carry Arithmetic | 348 / 348 | 348 / 348 | 324 / 348 | 292 / 348 | 0 / 222 | 🟡 MISMATCH |
| P4 | ROL / ROR / RCL / RCR Shift-Rotates | 432 / 432 | 432 / 432 | 432 / 432 | 182 / 432 | 0 / 182 | 🟡 SEMANTIC GAP |
| P5 | SHLD / SHRD Double-Precision Shifts | 72 / 72 | 72 / 72 | 72 / 72 | 72 / 72 | 0 / 72 | 🟡 SEMANTIC GAP |
| P6 | BT / BTS / BTR / BTC / BSF / BSR / BSWAP Bit-Ops | 238 / 238 | 238 / 238 | 226 / 238 | 224 / 238 | 40 / 212 | 🟡 MISMATCH |
| P7 | MOVS / CMPS / LODS / STOS / SCAS String Operations | 60 / 60 | 60 / 60 | 41 / 60 | 0 / 60 | N/A | 🟡 MISMATCH |
| P8 | x87 FPU Floating-Point | 424 / 424 | 424 / 424 | 359 / 424 | 91 / 424 | 0 / 38 | 🟡 MISMATCH |
| P9 | SSE / SSE2 SIMD Vector | 1169 / 1169 | 1169 / 1169 | 397 / 1169 | 123 / 1169 | 26 / 75 | 🟡 MISMATCH |
| P10 | Legacy i386-only (PUSHA, POPA, BCD, BOUND, ARPL, LDS/LES/LFS/LGS) | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | N/A | No cases |
| P11 | Other Base Integer / Control-Flow | 5155 / 5155 | 5155 / 5155 | 5116 / 5155 | 3240 / 5155 | 42 / 222 | 🟡 MISMATCH |

## Prioritized Implementation Action List

Action list for PE32/Lane A bring-up:

1. **P1 - MOVZX / MOVSX Zero/Sign-Extend**: Semantic execution mismatch against Unicorn for 93 checked cases.
1. **P2 - MUL / IMUL / DIV / IDIV Arithmetic**: Semantic execution mismatch against Unicorn for 111 checked cases.
1. **P3 - ADC / SBB Carry Arithmetic**: Mnemonic/Opcode mismatch for 24 decoded instructions.
1. **P3 - ADC / SBB Carry Arithmetic**: Semantic execution mismatch against Unicorn for 222 checked cases.
1. **P4 - ROL / ROR / RCL / RCR Shift-Rotates**: Semantic execution mismatch against Unicorn for 182 checked cases.
1. **P5 - SHLD / SHRD Double-Precision Shifts**: Semantic execution mismatch against Unicorn for 72 checked cases.
1. **P6 - BT / BTS / BTR / BTC / BSF / BSR / BSWAP Bit-Ops**: Mnemonic/Opcode mismatch for 12 decoded instructions.
1. **P6 - BT / BTS / BTR / BTC / BSF / BSR / BSWAP Bit-Ops**: Semantic execution mismatch against Unicorn for 172 checked cases.
1. **P7 - MOVS / CMPS / LODS / STOS / SCAS String Operations**: Mnemonic/Opcode mismatch for 19 decoded instructions.
1. **P8 - x87 FPU Floating-Point**: Mnemonic/Opcode mismatch for 65 decoded instructions.
1. **P8 - x87 FPU Floating-Point**: Semantic execution mismatch against Unicorn for 38 checked cases.
1. **P9 - SSE / SSE2 SIMD Vector**: Mnemonic/Opcode mismatch for 772 decoded instructions.
1. **P9 - SSE / SSE2 SIMD Vector**: Semantic execution mismatch against Unicorn for 49 checked cases.
1. **P11 - Other Base Integer / Control-Flow**: Mnemonic/Opcode mismatch for 39 decoded instructions.
1. **P11 - Other Base Integer / Control-Flow**: Semantic execution mismatch against Unicorn for 180 checked cases.

## Concrete Mismatch and Gap Examples (Shrunken)

Below are shrunken mismatch details (bytes, Capstone expected disassembly, and MacRunner decoder actual output) for debugging and implementation:

### MOVZX / MOVSX Zero/Sign-Extend (P1) Gaps
- **Case:** `real/ntdll-x86_64.dll/0000da8e`
  - **Bytes (Minimal Repro):** `0fb60593960900c07f34128877665544332211909090`
  - **Capstone Expected:** `movzx eax, byte ptr [rip + 0x99693]` (len: 7)
  - **HyperBridge Actual:** `MOVZX (op match: True)` (len: 7)
  - **Reason:** *Operands mismatch: Mem base mismatch: HB=16, CS=41*
- **Case:** `real/ntdll-x86_64.dll/0000daad`
  - **Bytes (Minimal Repro):** `0fb60574960900c07f34128877665544332211909090`
  - **Capstone Expected:** `movzx eax, byte ptr [rip + 0x99674]` (len: 7)
  - **HyperBridge Actual:** `MOVZX (op match: True)` (len: 7)
  - **Reason:** *Operands mismatch: Mem base mismatch: HB=16, CS=41*
- **Case:** `real/ntdll-x86_64.dll/0000dcf4`
  - **Bytes (Minimal Repro):** `0fb6052d940900c07f34128877665544332211909090`
  - **Capstone Expected:** `movzx eax, byte ptr [rip + 0x9942d]` (len: 7)
  - **HyperBridge Actual:** `MOVZX (op match: True)` (len: 7)
  - **Reason:** *Operands mismatch: Mem base mismatch: HB=16, CS=41*
- **Case:** `real/ntdll-x86_64.dll/0000dd40`
  - **Bytes (Minimal Repro):** `0fb605e1930900c07f34128877665544332211909090`
  - **Capstone Expected:** `movzx eax, byte ptr [rip + 0x993e1]` (len: 7)
  - **HyperBridge Actual:** `MOVZX (op match: True)` (len: 7)
  - **Reason:** *Operands mismatch: Mem base mismatch: HB=16, CS=41*

### MUL / IMUL / DIV / IDIV Arithmetic (P2) Gaps
- **Case:** `sys/imul3/66/reg`
  - **Bytes (Minimal Repro):** `660fafc0c07f34128877665544332211909090`
  - **Capstone Expected:** `imul ax, ax` (len: 4)
  - **HyperBridge Actual:** `IMUL (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Op 0 size mismatch: HB=4, CS=2*
- **Case:** `sys/imul3/66/mem`
  - **Bytes (Minimal Repro):** `660faf00c07f34128877665544332211909090`
  - **Capstone Expected:** `imul ax, word ptr [rax]` (len: 4)
  - **HyperBridge Actual:** `IMUL (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Op 0 size mismatch: HB=4, CS=2*
- **Case:** `sys/imul3/66/sib`
  - **Bytes (Minimal Repro):** `660faf0424c07f34128877665544332211909090`
  - **Capstone Expected:** `imul ax, word ptr [rsp]` (len: 5)
  - **HyperBridge Actual:** `IMUL (op match: True)` (len: 5)
  - **Reason:** *Operands mismatch: Op 0 size mismatch: HB=4, CS=2*
- **Case:** `sys/muldiv/rep/f6/6/mem`
  - **Bytes (Minimal Repro):** `f3f630c07f34128877665544332211909090`
  - **Capstone Expected:** `div byte ptr [rax]` (len: 3)
  - **HyperBridge Actual:** `JMP` (len: 3)
  - **Reason:** *Semantic failure: Interpreter crashed/failed to lift instruction (result=-8)*

### ADC / SBB Carry Arithmetic (P3) Gaps
- **Case:** `sys/adcsbb/base/lock/10/mem`
  - **Bytes (Minimal Repro):** `1000`
  - **Capstone Expected:** `lock adc byte ptr [rax], al` (len: 3)
  - **HyperBridge Actual:** `ADC` (len: 3)
  - **Reason:** *op mismatch (HB=adc, CS=lock adc)*
- **Case:** `sys/adcsbb/base/lock/10/sib`
  - **Bytes (Minimal Repro):** `100424`
  - **Capstone Expected:** `lock adc byte ptr [rsp], al` (len: 4)
  - **HyperBridge Actual:** `ADC` (len: 4)
  - **Reason:** *op mismatch (HB=adc, CS=lock adc)*
- **Case:** `sys/adcsbb/base/lock/11/mem`
  - **Bytes (Minimal Repro):** `1100`
  - **Capstone Expected:** `lock adc dword ptr [rax], eax` (len: 3)
  - **HyperBridge Actual:** `ADC` (len: 3)
  - **Reason:** *op mismatch (HB=adc, CS=lock adc)*
- **Case:** `sys/adcsbb/base/lock/11/sib`
  - **Bytes (Minimal Repro):** `110424`
  - **Capstone Expected:** `lock adc dword ptr [rsp], eax` (len: 4)
  - **HyperBridge Actual:** `ADC` (len: 4)
  - **Reason:** *op mismatch (HB=adc, CS=lock adc)*

### ROL / ROR / RCL / RCR Shift-Rotates (P4) Gaps
- **Case:** `sys/rotate/none/c0/0/reg`
  - **Bytes (Minimal Repro):** `c0c0c07f34128877665544332211909090`
  - **Capstone Expected:** `rol al, 0xc0` (len: 3)
  - **HyperBridge Actual:** `ROL (op match: True)` (len: 3)
  - **Reason:** *Operands mismatch: Op 1 immediate mismatch: HB=0, CS=192*
- **Case:** `sys/rotate/66/c0/0/reg`
  - **Bytes (Minimal Repro):** `66c0c0c07f34128877665544332211909090`
  - **Capstone Expected:** `rol al, 0xc0` (len: 4)
  - **HyperBridge Actual:** `ROL (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Op 1 immediate mismatch: HB=0, CS=192*
- **Case:** `sys/rotate/67/c0/0/reg`
  - **Bytes (Minimal Repro):** `67c0c0c07f34128877665544332211909090`
  - **Capstone Expected:** `rol al, 0xc0` (len: 4)
  - **HyperBridge Actual:** `ROL (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Op 1 immediate mismatch: HB=0, CS=192*
- **Case:** `sys/rotate/rep/c0/0/reg`
  - **Bytes (Minimal Repro):** `f3c0c0c07f34128877665544332211909090`
  - **Capstone Expected:** `rol al, 0xc0` (len: 4)
  - **HyperBridge Actual:** `ROL (op match: True)` (len: 4)
  - **Reason:** *Operands mismatch: Op 1 immediate mismatch: HB=0, CS=192*

### SHLD / SHRD Double-Precision Shifts (P5) Gaps
- **Case:** `sys/shldshrd/repne/ad/reg`
  - **Bytes (Minimal Repro):** `f20fadc0c07f34128877665544332211909090`
  - **Capstone Expected:** `shrd eax, eax, cl` (len: 4)
  - **HyperBridge Actual:** `JMP` (len: 4)
  - **Reason:** *Semantic failure: Interpreter crashed/failed to lift instruction (result=-8)*
- **Case:** `sys/shldshrd/rexw/a4/sib`
  - **Bytes (Minimal Repro):** `480fa40424c07f34128877665544332211909090`
  - **Capstone Expected:** `shld qword ptr [rsp], rax, 0xc0` (len: 6)
  - **HyperBridge Actual:** `JMP` (len: 6)
  - **Reason:** *Semantic mismatch against Unicorn: reg.rip expected=0x0000000000100006 actual=0x0000000000100008*
- **Case:** `sys/shldshrd/rexw/ad/reg`
  - **Bytes (Minimal Repro):** `480fadc0c07f34128877665544332211909090`
  - **Capstone Expected:** `shrd rax, rax, cl` (len: 4)
  - **HyperBridge Actual:** `JMP` (len: 4)
  - **Reason:** *Semantic failure: Interpreter crashed/failed to lift instruction (result=-8)*
- **Case:** `sys/shldshrd/repne/ac/mem`
  - **Bytes (Minimal Repro):** `f20fac00c07f34128877665544332211909090`
  - **Capstone Expected:** `shrd dword ptr [rax], eax, 0xc0` (len: 5)
  - **HyperBridge Actual:** `JMP` (len: 5)
  - **Reason:** *Semantic mismatch against Unicorn: reg.rip expected=0x0000000000100005 actual=0x0000000000100007*

### BT / BTS / BTR / BTC / BSF / BSR / BSWAP Bit-Ops (P6) Gaps
- **Case:** `sys/bit/lock/ab/mem`
  - **Bytes (Minimal Repro):** `0fab00`
  - **Capstone Expected:** `lock bts dword ptr [rax], eax` (len: 4)
  - **HyperBridge Actual:** `BTS` (len: 4)
  - **Reason:** *op mismatch (HB=bts, CS=lock bts)*
- **Case:** `sys/bit/lock/ab/sib`
  - **Bytes (Minimal Repro):** `0fab0424`
  - **Capstone Expected:** `lock bts dword ptr [rsp], eax` (len: 5)
  - **HyperBridge Actual:** `BTS` (len: 5)
  - **Reason:** *op mismatch (HB=bts, CS=lock bts)*
- **Case:** `sys/bit/lock/b3/mem`
  - **Bytes (Minimal Repro):** `0fb300`
  - **Capstone Expected:** `lock btr dword ptr [rax], eax` (len: 4)
  - **HyperBridge Actual:** `BTR` (len: 4)
  - **Reason:** *op mismatch (HB=btr, CS=lock btr)*
- **Case:** `sys/bit/lock/b3/sib`
  - **Bytes (Minimal Repro):** `0fb30424`
  - **Capstone Expected:** `lock btr dword ptr [rsp], eax` (len: 5)
  - **HyperBridge Actual:** `BTR` (len: 5)
  - **Reason:** *op mismatch (HB=btr, CS=lock btr)*

### MOVS / CMPS / LODS / STOS / SCAS String Operations (P7) Gaps
- **Case:** `sys/string/none/a4`
  - **Bytes (Minimal Repro):** `a4c07f34128877665544332211909090`
  - **Capstone Expected:** `movsb byte ptr [rdi], byte ptr [rsi]` (len: 1)
  - **HyperBridge Actual:** `MOVS (op match: True)` (len: 1)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=reg, CS=3*
- **Case:** `sys/string/66/a4`
  - **Bytes (Minimal Repro):** `66a4c07f34128877665544332211909090`
  - **Capstone Expected:** `movsb byte ptr [rdi], byte ptr [rsi]` (len: 2)
  - **HyperBridge Actual:** `MOVS (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=reg, CS=3*
- **Case:** `sys/string/67/a4`
  - **Bytes (Minimal Repro):** `67a4c07f34128877665544332211909090`
  - **Capstone Expected:** `movsb byte ptr [edi], byte ptr [esi]` (len: 2)
  - **HyperBridge Actual:** `MOVS (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=reg, CS=3*
- **Case:** `sys/string/rep/a4`
  - **Bytes (Minimal Repro):** `a4`
  - **Capstone Expected:** `rep movsb byte ptr [rdi], byte ptr [rsi]` (len: 2)
  - **HyperBridge Actual:** `MOVS (op match: False)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=reg, CS=3*

### x87 FPU Floating-Point (P8) Gaps
- **Case:** `sys/x87/reg/d8/c0`
  - **Bytes (Minimal Repro):** `d8c0c07f34128877665544332211909090`
  - **Capstone Expected:** `fadd st(0)` (len: 2)
  - **HyperBridge Actual:** `X87_FADD (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=imm, CS=1*
- **Case:** `sys/x87/reg/d8/c1`
  - **Bytes (Minimal Repro):** `d8c1c07f34128877665544332211909090`
  - **Capstone Expected:** `fadd st(1)` (len: 2)
  - **HyperBridge Actual:** `X87_FADD (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=imm, CS=1*
- **Case:** `sys/x87/reg/d8/c2`
  - **Bytes (Minimal Repro):** `d8c2c07f34128877665544332211909090`
  - **Capstone Expected:** `fadd st(2)` (len: 2)
  - **HyperBridge Actual:** `X87_FADD (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=imm, CS=1*
- **Case:** `sys/x87/reg/d8/c3`
  - **Bytes (Minimal Repro):** `d8c3c07f34128877665544332211909090`
  - **Capstone Expected:** `fadd st(3)` (len: 2)
  - **HyperBridge Actual:** `X87_FADD (op match: True)` (len: 2)
  - **Reason:** *Operands mismatch: Op 0 type mismatch: HB=imm, CS=1*

### SSE / SSE2 SIMD Vector (P9) Gaps
- **Case:** `sys/sse/sse_mov/none/10/reg`
  - **Bytes (Minimal Repro):** `0f10c0`
  - **Capstone Expected:** `movups xmm0, xmm0` (len: 3)
  - **HyperBridge Actual:** `SSE_MOV (op match: False)` (len: 3)
  - **Reason:** *Operands mismatch: Op 0 register mismatch: HB=17, CS=xmm0 (expected 0)*
- **Case:** `sys/sse/sse_mov/none/10/reg`
  - **Bytes (Minimal Repro):** `0f10c0`
  - **Capstone Expected:** `movups xmm0, xmm0` (len: 3)
  - **HyperBridge Actual:** `SSE_MOV` (len: 3)
  - **Reason:** *op mismatch (HB=sse_mov, CS=movups)*
- **Case:** `sys/sse/sse_mov/66/10/reg`
  - **Bytes (Minimal Repro):** `0f10c0`
  - **Capstone Expected:** `movupd xmm0, xmm0` (len: 4)
  - **HyperBridge Actual:** `SSE_MOV (op match: False)` (len: 4)
  - **Reason:** *Operands mismatch: Op 0 register mismatch: HB=17, CS=xmm0 (expected 0)*
- **Case:** `sys/sse/sse_mov/66/10/reg`
  - **Bytes (Minimal Repro):** `0f10c0`
  - **Capstone Expected:** `movupd xmm0, xmm0` (len: 4)
  - **HyperBridge Actual:** `SSE_MOV` (len: 4)
  - **Reason:** *op mismatch (HB=sse_mov, CS=movupd)*

### Other Base Integer / Control-Flow (P11) Gaps
- **Case:** `sys/base/lock/01/mem`
  - **Bytes (Minimal Repro):** `0100`
  - **Capstone Expected:** `lock add dword ptr [rax], eax` (len: 3)
  - **HyperBridge Actual:** `ADD` (len: 3)
  - **Reason:** *op mismatch (HB=add, CS=lock add)*
- **Case:** `sys/base/lock/01/sib`
  - **Bytes (Minimal Repro):** `010424`
  - **Capstone Expected:** `lock add dword ptr [rsp], eax` (len: 4)
  - **HyperBridge Actual:** `ADD` (len: 4)
  - **Reason:** *op mismatch (HB=add, CS=lock add)*
- **Case:** `sys/base/lock/03/mem`
  - **Bytes (Minimal Repro):** `0300`
  - **Capstone Expected:** `lock add eax, dword ptr [rax]` (len: 3)
  - **HyperBridge Actual:** `ADD` (len: 3)
  - **Reason:** *op mismatch (HB=add, CS=lock add)*
- **Case:** `sys/base/lock/03/sib`
  - **Bytes (Minimal Repro):** `030424`
  - **Capstone Expected:** `lock add eax, dword ptr [rsp]` (len: 4)
  - **HyperBridge Actual:** `ADD` (len: 4)
  - **Reason:** *op mismatch (HB=add, CS=lock add)*
