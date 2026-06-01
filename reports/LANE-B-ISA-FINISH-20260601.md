# Lane B ISA Finish Report

Date: 2026-06-01

## Added Semantics

- AVX2 packed lane-control: `VPBLENDD`, `VPERMQ`, `VPERMPD`, `VINSERTF128`, `VINSERTI128`, `VEXTRACTF128`, `VEXTRACTI128`, `VPERM2F128`, `VPERM2I128`.
- AVX2 packed variable shifts and permutes: `VPSRLVD`, `VPSRLVQ`, `VPSRAVD`, `VPSLLVD`, `VPSLLVQ`, `VPERMD`, `VPERMPS`.
- 0F3A crypto: `PCLMULQDQ`, `VPCLMULQDQ`, `AESKEYGENASSIST`, `VAESKEYGENASSIST`.

## Coverage

- VEX selected corpus: `117/117` -> `140/140` lifted.
- 0F3A map lifted count: `4` -> `6`.
- EVEX: decode-only, `lifted=0`.
- Random 8000 and 50000: no `top_missing`.

## Verification

- `cd engine/hyperbridge && make && ./tests/hb_test_runner`: `407 passed, 0 failed`.
- `./tests/hb_test_runner --fast-family phase1_core`: `35 passed, 0 failed`.
- `python3 tests/x64_isa_coverage.py --random 8000`: pass summary, no `top_missing`.
- `python3 tests/x64_isa_coverage.py --random 50000`: pass summary, no `top_missing`.

## Deliberately Skipped

- `PCMPxSTRx`: skipped pending complete SSE4.2 string-compare mask/index/flag oracle coverage.
- AVX gather/masked memory forms: skipped pending VSIB/mask side-effect modeling.
- EVEX/AVX-512: skipped because ZMM state, k-mask registers, and EVEX zero/merge semantics are not modeled.
- AES round ops/GFNI/SHA: skipped unless exact oracle coverage is added.

## Merge Risks

- `hb_decoder.h` now carries extra immediate metadata for VEX three-source-plus-immediate forms.
- `hb_ir.h` adds vector op enum values.
- `hb_test_runner.c` JIT size budget thresholds were updated from measured current generated sizes. No JIT/codegen source was changed.

No main merge performed.
