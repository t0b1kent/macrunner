# Lane B Phase 5 Random Tail + Scalar VEX Report

Scope: `/Volumes/MacOS 1/MacRunner-AIR-bulk-isa-kit` only. No main merge performed. EVEX remains decode-only.

## Before / After

| Metric | Before | After |
|---|---:|---:|
| Full runner | `391 passed, 0 failed` | `393 passed, 0 failed` |
| `phase1_core` | `28 passed, 0 failed` | `30 passed, 0 failed` |
| VEX selected corpus | `99/99` lifted | `117/117` lifted |
| `0F` map | `1101/1101` decoded, `672` lifted | unchanged |
| `0F38` map | `136/136` decoded, `45` lifted | unchanged |
| `0F3A` map | `35/35` decoded, `4` lifted | unchanged |

## Random Tail

| Probe | Before missing | After missing |
|---|---|---|
| Random 8000 | `0f208e` -> `mov rsi, cr1` | none |
| Random 20000 | `0f208e`, `0f231b` -> control/debug MOV; `c7f8d96b81fd` -> `xbegin` | none |
| Random 50000 | above plus `410f2152` -> `mov r10, dr2`, `4964bb59636e4a` -> REX ignored before segment `mov ebx, imm32`, `0f72e121` -> MMX `psrad`, `0fb529` -> `lgs` | none |

Final random counts:

| Probe | Capstone | Decoded | Length | Mnemonic | Lifted |
|---|---:|---:|---:|---:|---:|
| Random 8000 | 6112 | 6112 | 6112 | 5943 | 5050 |
| Random 20000 | 15190 | 15190 | 15190 | 14770 | 12488 |
| Random 50000 | 38189 | 38189 | 38189 | 37155 | 31308 |

## Families Added

- `VMOVSS/VMOVSD`: register merge plus memory load/store forms; VEX 128-bit upper YMM zero verified.
- `VADDSS/VADDSD`, `VSUBSS/VSUBSD`, `VMULSS/VMULSD`, `VDIVSS/VDIVSD`, `VMINSS/VMINSD`, `VMAXSS/VMAXSD`: scalar merge semantics, upper-zero, Inf/NaN, and signed-zero edge cases verified.
- Generic VEX `VCMP*` fallback now consumes the immediate byte for correct length, but execution remains unsupported.
- `xbegin` now decodes as controlled unsupported system semantics; TSX execution was not added.
- Privileged/control/debug MOV samples decode and lift to controlled unsupported/fault behavior, not `HB_OK` execution.

## Upper-Zero Audit

- Added focused regression for implemented 128-bit VEX destination families covering packed float, packed integer, unpack, AVX2 packed op, scalar MOV/arithmetic, and MOVUPS.
- Added scalar merge checks that preserve untouched XMM lanes from VEX source1 while zeroing destination YMM high halves.

## Skipped

- `VCMPSS/VCMPSD` predicate execution: skipped pending exact predicate oracle coverage.
- AES/PCLMUL/PCMPxSTRx/SHA: skipped because exact instruction oracles were not added in this batch.
- CRC/ADX/BMI/BMI2 tails: skipped pending focused flag/edge-case oracle coverage.
- EVEX execution: skipped because ZMM state, mask registers, and zeroing/merging semantics are not modeled.

## Commands

| Command | Result |
|---|---|
| `cd engine/hyperbridge && make && ./tests/hb_test_runner` | pass, `393 passed, 0 failed` |
| `./tests/hb_test_runner --fast-family phase1_core` | pass, `30 passed, 0 failed` |
| `python3 tests/x64_isa_coverage.py --random 8000` | pass, no `top_missing`, VEX `117/117` |
| `python3 tests/x64_isa_coverage.py --random 20000` | pass, no `top_missing` |
| `python3 tests/x64_isa_coverage.py --random 50000` | pass, no `top_missing` |

## Merge Risks

- `hb_ir.h` adds `HB_IR_XMM_SCALAR_MOV` and `zero_ymm_upper`; downstream IR producers/consumers must preserve zero-initialization.
- `hb_interpreter.c` now tracks the current IR instruction for all interpreter steps so vector writes can apply VEX upper-zero semantics.
- ARM64 JIT/codegen was not edited; VEX semantics here are interpreter-verified only.

No full AVX/AVX2 completion is claimed.
