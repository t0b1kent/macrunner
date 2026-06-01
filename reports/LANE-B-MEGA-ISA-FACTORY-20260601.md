# Lane B Mega ISA Factory Report

Date: 2026-06-01

## Summary

- Worked only in `/Volumes/MacOS 1/MacRunner-AIR-bulk-isa-kit`.
- No main merge performed.
- No JIT/codegen source edits.
- Full AVX/AVX2 and full AVX-512 are not claimed complete.

## New Lifted Families

- AVX mask/maskmov: `VMASKMOVPS/PD`, `VPMASKMOVD/Q`.
- AVX2 gather: all `VGATHER*` and `VPGATHER*` selected forms with VSIB metadata.
- SSE4.2 PCMPxSTR: `PCMPESTRM`, `PCMPESTRI`, `PCMPISTRM`, `PCMPISTRI`.
- AES round family: `AESIMC`, `AESENC`, `AESENCLAST`, `AESDEC`, `AESDECLAST`, VEX/EVEX `VAES*`.
- GFNI exact subset: `GF2P8MULB`, `VGF2P8MULB`.
- EVEX unmasked ZMM smoke: `VADDPS`, `VMOVDQA32`, `VAESENC`, `VGF2P8MULB`.

## Coverage

- VEX selected corpus: `157/157` lifted.
- EVEX selected smoke: `4/4` lifted.
- `0F38`: lifted `51`.
- `0F3A`: lifted `10`.
- Random 8000 and random 50000: no `top_missing`.

## Verification

- `cd engine/hyperbridge && make && ./tests/hb_test_runner`: `416 passed, 0 failed`.
- `./tests/hb_test_runner --fast-family phase1_core`: `41 passed, 0 failed`.
- `python3 tests/x64_isa_coverage.py --random 8000`: pass, no `top_missing`.
- `python3 tests/x64_isa_coverage.py --random 50000`: pass, no `top_missing`.

## Deliberately Skipped

- SHA instructions: skipped pending independent exact oracle vectors.
- GFNI affine/inverse affine: skipped pending independent bit-matrix oracle vectors.
- Masked/zeroing EVEX, SAE/rounding, broadcast, opmask writes, and 32-register ZMM addressing: skipped because only appended state and unmasked smoke semantics are modeled.
- Broad MMX/x87 tails: not touched in this pass.

## Merge Risks

- `hb_context_t` append-only state now includes `zmm_hi[16][4]` and `k[8]`.
- `hb_ir.h` adds `HB_SIZE_512`; consumers assuming max `HB_SIZE_256` need audit.
- `hb_decoder.h` and IR memory operands carry VSIB metadata used by gather.
