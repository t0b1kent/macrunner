# HB x64 ISA Coverage Matrix

Date: 2026-06-05

Scope: Lane B decode/lift/interpreter only. No main merge performed. No ARM64 JIT/codegen source edits were made. EVEX remains decode-only.

## Phase 5 Summary

- Full runner moved from `391 passed, 0 failed` to `393 passed, 0 failed`.
- `phase1_core` moved from `28 passed, 0 failed` to `30 passed, 0 failed`.
- Selected VEX corpus moved from `99/99` lifted to `117/117` lifted.
- Closed the random decode tail for exact `mov` control/debug-register bytes, `xbegin`, REX-before-segment `mov imm`, MMX immediate shift, `lgs`, and VEX `VCMP*` immediate length.
- Added scalar VEX MOV and arithmetic semantics/tests for `VMOVSS/VMOVSD`, `VADDSS/VADDSD`, `VSUBSS/VSUBSD`, `VMULSS/VMULSD`, `VDIVSS/VDIVSD`, `VMINSS/VMINSD`, and `VMAXSS/VMAXSD`.
- Added focused upper-zero regressions for implemented 128-bit VEX destinations and scalar merge/load/store behavior.

## Final Matrix

| Opcode group | Capstone valid | Decoded | Length matches capstone | Mnemonic/family matches | Lifted | Oracle/regression evidence | Remaining gaps |
|---|---:|---:|---:|---:|---:|---|---|
| Legacy one-byte | 1375 | 1375 | 1375 | 1302 | 1183 | Existing runner/oracle families pass | Alias/family naming and generic privileged/system placeholders |
| `0F` map | 1101 | 1101 | 1101 | 1055 | 672 | Existing scalar/SSE/MMX decode probes plus runtime tests pass | Remaining non-modeled MMX/system forms |
| `0F38` map | 136 | 136 | 136 | 86 | 45 | Focused SSSE3/SSE4/AVX2 packed tests pass | AES/SHA/CRC/ADX/BMI/string-compare tails need exact oracle coverage |
| `0F3A` map | 35 | 35 | 35 | 23 | 4 | Focused `pblendw`/`palignr` tests pass | Remaining immediate SSE4/AES/string-compare forms |
| x87 register forms | 364 | 364 | 364 | 364 | 210 | x87 fast-family subset and tbyte `FLD` memory regression pass | True 80-bit precision is not modeled; value is converted into the existing double x87 state |
| VEX selected corpus | 117 | 117 | 117 | 117 | 117 | Scalar MOV/arithmetic, packed VEX/AVX2, upper-zero, XMM/YMM low/high, reg-reg, memory, and legacy-equivalence tests | Full AVX/AVX2 is not complete; selected corpus only |
| EVEX smoke | 2 | 2 | 2 | 1 | 0 | Decode/length only | No AVX-512 register, mask, zeroing, or execution model |
| Random 8000 capstone-valid | 6112 | 6112 | 6112 | 5943 | 5050 | Same decode/lift probe; no `top_missing` | Random tail closed for this seed/size |
| Random 20000 capstone-valid | 15190 | 15190 | 15190 | 14770 | 12488 | Same decode/lift probe; no `top_missing` | Random tail closed for this seed/size |
| Random 50000 capstone-valid | 38189 | 38189 | 38189 | 37155 | 31308 | Same decode/lift probe; no `top_missing` | Random tail closed for this seed/size |

## Verification

| Command | Result |
|---|---|
| `cd engine/hyperbridge && make && ./tests/hb_test_runner` | `393 passed, 0 failed` |
| `./tests/hb_test_runner --fast-family phase1_core` | `30 passed, 0 failed` |
| `python3 tests/x64_isa_coverage.py --random 8000` | VEX `117/117`; EVEX `lifted=0`; no `top_missing` |
| `python3 tests/x64_isa_coverage.py --random 20000` | VEX `117/117`; EVEX `lifted=0`; no `top_missing` |
| `python3 tests/x64_isa_coverage.py --random 50000` | VEX `117/117`; EVEX `lifted=0`; no `top_missing` |

## Remaining Intentionally Unsupported Semantics

- Full AVX/AVX2 is not claimed complete. Coverage is proven only for selected corpus entries and explicit regression families.
- EVEX/AVX-512 remains decode-only until ZMM state, mask registers, zeroing/merging, and full execution semantics are modeled.
- `VCMPSS/VCMPSD` now decode the immediate length in generic VEX fallback, but execution remains unsupported pending predicate oracle coverage.
- AES/PCLMUL/PCMPxSTRx/SHA/CRC/ADX/BMI tails remain skipped unless exact oracle tests are added.
- `xbegin` decodes to controlled unsupported system semantics; TSX execution is not modeled.
- Privileged/control/debug register MOV samples decode and lift to controlled unsupported/fault behavior; they are not executed as user-mode success.

## Merge Risks

- `hb_ir.h` adds `HB_IR_XMM_SCALAR_MOV` and `zero_ymm_upper`; downstream IR consumers should preserve the field initialization semantics.
- Existing Lane B YMM state appends `ymm_hi[16][2]` to `hb_context_t`; main Mac reconciliation still needs care.
- VEX scalar semantics are interpreter-verified only. ARM64 JIT/codegen was intentionally not edited.
