# HB x64 ISA Coverage Matrix

Date: 2026-06-01

Scope: Lane B decode/lift/interpreter only. No main merge performed. No ARM64 JIT/codegen source edits were made. EVEX is no longer decode-only for the listed unmasked ZMM smoke families, but masked/zeroing EVEX remains unsupported.

## Current Summary

- Full runner is now `419 passed, 0 failed`.
- `phase1_core` is now `42 passed, 0 failed`.
- Selected VEX corpus moved from `157/157` lifted to `185/185` lifted in this kit.
- Added AVX2 packed lane-control semantics for `VPBLENDD`, `VPERMQ`, `VPERMPD`, `VINSERTF128`, `VINSERTI128`, `VEXTRACTF128`, `VEXTRACTI128`, `VPERM2F128`, and `VPERM2I128`.
- Added AVX2 packed variable shift and permute semantics for `VPSRLVD`, `VPSRLVQ`, `VPSRAVD`, `VPSLLVD`, `VPSLLVQ`, `VPERMD`, and `VPERMPS`.
- Added `PCLMULQDQ`, `VPCLMULQDQ`, and `AESKEYGENASSIST`/`VAESKEYGENASSIST` semantics with exact regression vectors.
- Added AVX mask/masked-load-store and gather coverage for `VMASKMOVPS/PD`, `VPMASKMOVD/Q`, `VGATHER*`, and `VPGATHER*`.
- Added SSE4.2 `PCMPESTRM/PCMPESTRI/PCMPISTRM/PCMPISTRI` interpreter semantics with flag/index/mask regressions.
- Added AES round-family semantics for `AESIMC`, `AESENC`, `AESENCLAST`, `AESDEC`, `AESDECLAST`, plus VEX/EVEX `VAES*` coverage.
- Added GFNI `GF2P8MULB`/`VGF2P8MULB` semantics for XMM/YMM/ZMM lanes.
- Appended AVX-512 `zmm_hi[16][4]` and `k[8]` state to `hb_context_t`; implemented unmasked EVEX ZMM `VADDPS`, `VMOVDQA32`, `VAESENC`, and `VGF2P8MULB` smoke semantics.
- Added FMA3 `VFMADD/VFMSUB` 132/213/231 semantics for packed PS/PD and scalar SS/SD.
- Added F16C `VCVTPH2PS` and `VCVTPS2PH` XMM/YMM reg/mem semantics with nearest-even and directed immediate rounding support; MXCSR-selected rounding falls back to nearest-even because MXCSR is not modeled.
- Updated interpreter CPUID/XGETBV policy to expose tested SSE/AVX/FMA/F16C/AES/PCLMUL feature bits while leaving SHA, GFNI, AVX2, and AVX-512 feature bits unadvertised.

## Final Matrix

| Opcode group | Capstone valid | Decoded | Length matches capstone | Mnemonic/family matches | Lifted | Oracle/regression evidence | Remaining gaps |
|---|---:|---:|---:|---:|---:|---|---|
| Legacy one-byte | 1375 | 1375 | 1375 | 1302 | 1183 | Existing runner/oracle families pass | Alias/family naming and generic privileged/system placeholders |
| `0F` map | 1101 | 1101 | 1101 | 1054 | 673 | Existing scalar/SSE/MMX decode probes plus runtime tests pass | Remaining non-modeled MMX/system forms |
| `0F38` map | 136 | 136 | 136 | 87 | 51 | Focused SSSE3/SSE4/AVX2 packed, AES round, and GF2P8MULB tests pass | SHA/CRC/ADX/BMI, GFNI affine forms, and remaining decode-only vector tails |
| `0F3A` map | 35 | 35 | 35 | 23 | 10 | `PBLENDW`, `PALIGNR`, `PCLMULQDQ`, `AESKEYGENASSIST`, and `PCMPxSTRx` pass focused interpreter regressions | `DPPS/DPPD`, `ROUND*`, `INSERTPS/EXTRACTPS`, `PINSR*/PEXTR*`, GFNI affine/SHA tails |
| x87 register forms | 364 | 364 | 364 | 364 | 210 | x87 fast-family subset and tbyte `FLD` memory regression pass | True 80-bit precision is not modeled; value is converted into the existing double x87 state |
| VEX selected corpus | 185 | 185 | 185 | 185 | 185 | Scalar, packed AVX/AVX2, FMA3, F16C, upper-zero, maskmov/gather, AES, GFNI, XMM/YMM low/high, reg-reg, memory, and legacy-equivalence tests | Full AVX/AVX2 is still not claimed complete; Unicorn 2.1.4 rejects FMA/F16C VEX opcodes as invalid |
| EVEX smoke | 4 | 4 | 4 | 4 | 4 | Unmasked ZMM `VADDPS`, `VMOVDQA32`, `VAESENC`, and `VGF2P8MULB` interpreter regressions pass | Mask registers are stored, but masked merge/zeroing and broad AVX-512 semantics remain unsupported |
| Random 8000 capstone-valid | 6112 | 6112 | 6112 | 5943 | 5103 | Same decode/lift probe; no `top_missing` | Random tail closed for this seed/size |
| Random 50000 capstone-valid | 38189 | 38189 | 38189 | 37155 | 31593 | Same decode/lift probe; no `top_missing` | Random tail closed for this seed/size |

## Verification

| Command | Result |
|---|---|
| `cd engine/hyperbridge && make && ./tests/hb_test_runner` | `419 passed, 0 failed` |
| `./tests/hb_test_runner --fast-family phase1_core` | `42 passed, 0 failed` |
| `python3 tests/x64_isa_coverage.py --random 8000` | VEX `185/185`; EVEX `4/4`; `0F38 lifted=51`; `0F3A lifted=10`; no `top_missing` |
| `python3 tests/x64_isa_coverage.py --random 50000` | VEX `185/185`; EVEX `4/4`; `0F38 lifted=51`; `0F3A lifted=10`; no `top_missing` |
| `python3 tests/hb_fuzz_diff.py --cases 20000 --batch 512` | 20,000 cases; 16,839 Unicorn-checked; 0 interpreter oracle mismatches; FMA/F16C recorded as Unicorn unsupported |

## Remaining Intentionally Unsupported Semantics

- Full AVX/AVX2 is not claimed complete. Remaining exact-testable VEX gaps include `VPERMIL*`, VEX `*BLENDV*`, and less common predicate/control tails outside the selected corpus.
- `PCMPxSTRx` now has focused byte/word mask/index/flag coverage, but broad string-compare mode coverage should be expanded before claiming exhaustive SSE4.2.
- EVEX/AVX-512 is only modeled for unmasked smoke families listed above. Masked merge/zeroing, opmask writes, SAE/rounding, broadcast, 32-register ZMM addressing, and broad AVX-512 semantics remain unsupported.
- SHA and GFNI affine forms were deliberately skipped because no independent exact oracle was established in this pass.
- `VCMPSS/VCMPSD` decode the immediate length in generic VEX fallback, but execution remains unsupported pending predicate oracle coverage.
- FMA3 and F16C have focused interpreter regression coverage, but this Unicorn 2.1.4 build returns `UC_ERR_INSN_INVALID` for those VEX opcodes, so they are not claimed as Unicorn-executed oracle cases.
- `xbegin` decodes to controlled unsupported system semantics; TSX execution is not modeled.
- Privileged/control/debug register MOV samples decode and lift to controlled unsupported/fault behavior; they are not executed as user-mode success.

## Merge Risks

- `hb_decoder.h` appends VSIB metadata plus `has_imm8`/`imm8`; gather and VEX/EVEX three-source forms depend on this.
- `hb_ir.h` adds `HB_SIZE_512` and new vector op enum values for maskmov/gather/AES/GFNI/PCMPxSTR semantics.
- `hb_context_t` now appends `ymm_hi[16][2]`, `zmm_hi[16][4]`, and `k[8]`; main Mac reconciliation needs care.
- No JIT/codegen source edits were made in this pass.
