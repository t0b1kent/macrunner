# HB x64 ISA Coverage Matrix

Date: 2026-05-31

Scope: Lane B decode/lift/interpreter only. No JIT/codegen files were edited. References: Python capstone 5.0.7 for first-instruction decode/length/family checks, `hb_x64_probe` for HyperBridge decode/lift status, and existing `hb_test_runner --fast-family` oracle families.

## Batch Summary

- Added reusable bulk coverage harness: `engine/hyperbridge/tests/x64_isa_coverage.py` and `engine/hyperbridge/tests/hb_x64_probe.c`.
- Closed the requested numeric decode goals:
  - Legacy one-byte: `1131/1375` -> `1375/1375`.
  - Random capstone-valid: `2418/3081` -> `3081/3081`.
  - Structured `0F`: `649/1101` initial matrix, `1047/1101` pre-resume -> `1101/1101`.
- Added broad decode/length coverage for x87, 0F system/control, MMX/SSE fallback families, 0F38/0F3A, VEX, EVEX, mixed prefix scans, port/string I/O, interrupt/flag/control forms, CR/DR/segment forms, far returns, and operand-size immediate/Jcc length fixes.
- Current C runner after semantic lift pass: `384 passed, 0 failed`.

## Coverage Corpus

- Legacy one-byte map with prefixes: none, `66`, `67`, `48`, `F3`, `F2`.
- `0F`, `0F38`, and `0F3A` maps with the same prefix set.
- x87 `D8-DF` register forms.
- VEX and EVEX smoke samples.
- 4000 deterministic random byte streams; only capstone-valid first instructions are scored.

## Final Matrix

| Opcode group | Capstone valid | Decoded | Length matches capstone | Mnemonic/family matches | Lifted | Oracle verified | Leading remaining non-decode gaps |
|---|---:|---:|---:|---:|---:|---|---|
| Legacy one-byte | 1375 | 1375 | 1375 | 1302 | 1183 | Existing runner/oracle families pass | Alias/family naming: `wait->NOP`, `cwde->CDQE`, `cdq->CWD`, `cmpsd->CMPS`, `repz ret->RET` |
| `0F` map | 1101 | 1101 | 1101 | 1018 | 654 | Existing runner/oracle families pass for lifted scalar families | Generic/alias families: `movq->MMX`, `ucomiss->COMISS`, `bndmov->NOP`, `movd->MOVD`, `movq->MOVD` |
| `0F38` map | 136 | 136 | 136 | 82 | 47 | Oracle-smoke for CRC32/PABSB/PMOVSXBW/PMINSB/PTEST plus lifted SSSE3/SSE4.1 integer families | Remaining `VEC`: MMX encodings, SHA/AES/GFNI/ADX and unsupported crypto/string families |
| `0F3A` map | 35 | 35 | 35 | 20 | 18 | Oracle-smoke for PBLENDW/PEXTRB/PINSRB plus lifted ROUND/BLEND/INSERTPS/DPPS/DPPD/MPSADBW forms | Remaining `VEC`: PCLMULQDQ, SSE4 string compare, AESKEYGENASSIST, GFNI |
| x87 register forms | 364 | 364 | 364 | 364 | 210 | Existing x87 fast-family subset passes | Non-core x87 stack/control forms remain unsupported in lifter |
| VEX smoke | 11 | 11 | 11 | 11 | 11 | Oracle-smoke for VZEROUPPER/VPXOR/VADDPS/VSUBPS/VXORPS/VMOVQ with x64 YMM upper-half state; decode/lift smoke also covers VMULPS/VDIVPS/VANDPS/VANDNPS/VORPS | Broader AVX/AVX2 VEX tables remain unsupported |
| EVEX smoke | 2 | 2 | 2 | 2 | 0 | Decode/length only | Generic `VEC` placeholder; no AVX-512 semantics in lifter |
| Random capstone-valid | 3081 | 3081 | 3081 | 2995 | 2525 | Same decode/lift probe; oracle families pass | Alias/family naming and generic unsupported vectors/system ops |

Note: the harness normalizes several late-vector capstone mnemonics to the generic `vec` family, so exact opcode names can lower the mnemonic/family-match column while increasing real lifted coverage.

## Cycle Log

| Cycle | Largest gap attacked | Result |
|---|---|---|
| 1 | Legacy one-byte gaps | Added moffs, XCHG, segment MOV, LEAVE, port/string I/O, interrupts, flags, POP r/m, HLT, operand-size immediates |
| 2 | x87 and system/control gaps | Added x87 decode/family lifting, CR/DR, segment push/pop, system/UD/MMX generic decoding |
| 3 | 0F38/0F3A/VEX/EVEX gaps | Added generic vector decode/length coverage and focused tests |
| 4 | Remaining prefixed 0F vector/system gaps | Closed EXTRQ/INSERTQ, HADDPD/HADDPS, POPCNT, RDFSBASE, SSE2 packed variants |
| 5 | Remaining capstone length mismatches | Closed 0F A6/A7, UD1, operand-size PUSH/IMUL/TEST, capstone-compatible 66 Jcc lengths |
| 6 | 0F38/0F3A semantic placeholders | Lifted SSSE3/SSE4.1/SSE4.2 XMM integer/immediate families into interpreter IR and added fixed oracle-smoke tests |
| 7 | VEX smoke semantics | Modeled x64 YMM upper halves, lifted/interpreted common packed VEX PS/logical smoke forms plus VMOVQ/VZEROUPPER, and added AVX oracle-smoke tests |

## Remaining Intentionally Unsupported Semantics

- Shared struct note: `hb_context_t` now appends `x64_ymm_hi[16][2]` for AVX YMM upper halves. Existing context field offsets were preserved by appending the state at the end; reconcile this shared layout change carefully on merge.
- Generic `HB_INS_VEC`: broad AVX/AVX2 VEX coverage beyond the smoke set, EVEX, MMX-encoded SSSE3 forms, SHA/AES/GFNI/ADX/PCLMUL/string-compare families, and the remaining crypto/control vector forms are decoded and length-matched, but not lifted to real vector semantics.
- Generic `HB_INS_MMX`: MMX/3DNow and shared packed-integer forms are decoded and length-matched; unsupported lifter emission is intentional until MMX state is modeled.
- Generic `HB_INS_SYS`/`HB_INS_UD`: privileged/system/virtualization/FSGSBASE-like forms are decoded for length/family coverage and intentionally not executed as real privileged operations.
- MPX `BNDMOV` is currently decoded as a NOP-family placeholder because bound registers are not modeled.

## Verification

| Command | Result |
|---|---|
| `make tests/hb_test_runner` | pass |
| `./tests/hb_test_runner` | `382 passed, 0 failed` |
| `python3 tests/x64_isa_coverage.py --random 4000` | all capstone-valid generated cases decoded and length-matched |
| `./tests/hb_test_runner --fast-family rep_movs` | `14 passed, 0 failed` |
| `./tests/hb_test_runner --fast-family string_ops` | `14 passed, 0 failed` |
| `./tests/hb_test_runner --fast-family phase1_core` | `14 passed, 0 failed` |

`git status` is unavailable in this kit because this directory is not a git repository; transfer artifacts contain the changed files instead of a generated git diff.
