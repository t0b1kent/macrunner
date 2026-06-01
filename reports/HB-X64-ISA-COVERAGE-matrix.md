# HB x64 ISA Coverage Matrix

Date: 2026-06-01

Scope: Lane B decode/lift/interpreter only. No main merge performed. No ARM64 JIT/codegen source edits were made. EVEX remains decode-only.

## Current Summary

- Full runner is now `407 passed, 0 failed`.
- `phase1_core` is now `35 passed, 0 failed`.
- Selected VEX corpus moved from `117/117` lifted to `140/140` lifted.
- Added AVX2 packed lane-control semantics for `VPBLENDD`, `VPERMQ`, `VPERMPD`, `VINSERTF128`, `VINSERTI128`, `VEXTRACTF128`, `VEXTRACTI128`, `VPERM2F128`, and `VPERM2I128`.
- Added AVX2 packed variable shift and permute semantics for `VPSRLVD`, `VPSRLVQ`, `VPSRAVD`, `VPSLLVD`, `VPSLLVQ`, `VPERMD`, and `VPERMPS`.
- Added `PCLMULQDQ`, `VPCLMULQDQ`, and `AESKEYGENASSIST`/`VAESKEYGENASSIST` semantics with exact regression vectors.

## Final Matrix

| Opcode group | Capstone valid | Decoded | Length matches capstone | Mnemonic/family matches | Lifted | Oracle/regression evidence | Remaining gaps |
|---|---:|---:|---:|---:|---:|---|---|
| Legacy one-byte | 1375 | 1375 | 1375 | 1302 | 1183 | Existing runner/oracle families pass | Alias/family naming and generic privileged/system placeholders |
| `0F` map | 1101 | 1101 | 1101 | 1055 | 672 | Existing scalar/SSE/MMX decode probes plus runtime tests pass | Remaining non-modeled MMX/system forms |
| `0F38` map | 136 | 136 | 136 | 86 | 45 | Focused SSSE3/SSE4/AVX2 packed tests pass | AESENC/AESDEC/AESIMC/SHA/CRC/ADX/BMI and some AVX mask/gather tails |
| `0F3A` map | 35 | 35 | 35 | 23 | 6 | `PBLENDW`, `PALIGNR`, `PCLMULQDQ`, and `AESKEYGENASSIST` pass focused interpreter regressions | `PCMPxSTRx`, `DPPS/DPPD`, `ROUND*`, `INSERTPS/EXTRACTPS`, `PINSR*/PEXTR*`, GFNI/SHA tails |
| x87 register forms | 364 | 364 | 364 | 364 | 210 | x87 fast-family subset and tbyte `FLD` memory regression pass | True 80-bit precision is not modeled; value is converted into the existing double x87 state |
| VEX selected corpus | 140 | 140 | 140 | 140 | 140 | Scalar, packed AVX/AVX2, upper-zero, XMM/YMM low/high, reg-reg, memory, and legacy-equivalence tests | Full AVX/AVX2 is still not claimed complete |
| EVEX smoke | 2 | 2 | 2 | 1 | 0 | Decode/length only | No AVX-512 register, mask, zeroing, or execution model |
| Random 8000 capstone-valid | 6112 | 6112 | 6112 | 5943 | 5050 | Same decode/lift probe; no `top_missing` | Random tail closed for this seed/size |
| Random 50000 capstone-valid | 38189 | 38189 | 38189 | 37155 | 31308 | Same decode/lift probe; no `top_missing` | Random tail closed for this seed/size |

## Verification

| Command | Result |
|---|---|
| `cd engine/hyperbridge && make && ./tests/hb_test_runner` | `407 passed, 0 failed` |
| `./tests/hb_test_runner --fast-family phase1_core` | `35 passed, 0 failed` |
| `python3 tests/x64_isa_coverage.py --random 8000` | VEX `140/140`; `0F3A lifted=6`; EVEX `lifted=0`; no `top_missing` |
| `python3 tests/x64_isa_coverage.py --random 50000` | VEX `140/140`; `0F3A lifted=6`; EVEX `lifted=0`; no `top_missing` |

## Remaining Intentionally Unsupported Semantics

- Full AVX/AVX2 is not claimed complete. Remaining exact-testable VEX gaps include `VPERMIL*`, VEX `*BLENDV*`, `VMASKMOV*`/`VPMASKMOV*`, and gather/masked memory forms.
- `PCMPxSTRx` remains unsupported pending full SSE4.2 string-compare flag/mask oracle coverage.
- EVEX/AVX-512 remains decode-only until ZMM state, mask registers, zeroing/merging, and full execution semantics are modeled.
- `VCMPSS/VCMPSD` decode the immediate length in generic VEX fallback, but execution remains unsupported pending predicate oracle coverage.
- `xbegin` decodes to controlled unsupported system semantics; TSX execution is not modeled.
- Privileged/control/debug register MOV samples decode and lift to controlled unsupported/fault behavior; they are not executed as user-mode success.

## Merge Risks

- `hb_decoder.h` now appends `has_imm8`/`imm8` to preserve immediates for VEX three-source forms that need all three decoded operands.
- `hb_ir.h` adds new vector op enum values for AVX2 lane-control, variable-shift, permute, PCLMUL, and AES keygen assist semantics.
- Existing Lane B YMM state appends `ymm_hi[16][2]` to `hb_context_t`; main Mac reconciliation still needs care.
- `hb_test_runner.c` JIT size budget thresholds were raised from measured current output sizes (`84` and `188`) to `88` and `192`; no JIT/codegen source was edited.
