# HB Differential Correctness Fuzzing - 2026-06-01

Base: `18be487 checkpoint(Lane A): gate broken - past NtWaitForSingleObject deadlock, now on CRT/entry init`

## Oracle

Unicorn 2.1.4 is installed and is now wired as the primary x86-64 oracle:

- `tools/hb_oracle/unicorn_adapter.py`
- deterministic seed-compatible state generator matching `hb_diff_case_runner`
- GPR, modeled EFLAGS, XMM/YMM lanes, RIP, and data/stack memory hashes compared
- SDE remains unavailable and scaffold-only

Harness updates:

- `hb_diff_case_runner.c` now emits all XMM/YMM lanes, raw RFLAGS, defined-flag mask, and data/stack hashes.
- Raw RFLAGS fuzzing is sanitized to status flags plus DF/IF/reserved bit, avoiding Unicorn TF/VM/AC/system-bit traps.
- REP string instructions run to the instruction end address in Unicorn rather than one micro-step.
- Undefined x86 flags are filtered per template where required.

## Bugs Found And Fixed

- `IMUL r64,r/m64` now sets CF/OF on signed truncation overflow.
- `POPCNT r64,r/m64` now decodes, lifts, and executes with Intel flag semantics.
- `RCL/RCR` now lift and execute in the interpreter.
- `AESDEC/AESDECLAST` inverse AES round order corrected and regression expectations updated to Unicorn/Intel behavior.

Permanent regression:

- `interp_x64_unicorn_diff_regressions_scalar_flags` in `hb_test_runner.c`.

## Fuzz Runs

Final all-family command:

`python3 tests/hb_fuzz_diff.py --cases 100000 --batch 1024`

Result:

- cases run: 100,000
- Unicorn checked: 91,650
- Unicorn oracle mismatches: 0
- families covered: int arith/logic, mul/div, shifts, RCL/RCR, CMOV/SETcc, BT*, bit scan, BMI, XCHG/XADD/CMPXCHG, LEA, SSE/SSE2/SSSE3/SSE4, AES/PCLMUL, AVX templates, string/REP

Focused confirmations:

- `python3 tests/hb_fuzz_diff.py --families aes_pclmul --cases 700 --batch 128`: 700 checked, 0 mismatches
- `python3 tests/hb_fuzz_diff.py --families string_ops --cases 1000 --batch 128`: 1000 checked, 0 mismatches

## Known Oracle Limits

Unicorn 2.1.4 is not reliable for the AVX subset here:

- rejects several 256-bit VEX instructions as invalid
- mishandles 128-bit VEX three-operand source semantics / upper-zero behavior

Those are recorded under `oracle_unsupported` / `oracle_quirks`, not counted as interpreter failures.

DIV overflow/divide-by-zero cases are counted as Unicorn exceptions; both sides fault for the random trap cases seen.

## Verification

- `make tests/hb_test_runner && ./tests/hb_test_runner`: 417 passed, 0 failed
- `./tests/hb_test_runner --fast-family phase1_core`: 41 passed, 0 failed
- `python3 tests/hb_fuzz_diff.py --cases 100000 --batch 1024`: 0 Unicorn interpreter mismatches

## Changed Files

- `engine/hyperbridge/include/hb_ir.h`
- `engine/hyperbridge/src/hb_decode_x64.c`
- `engine/hyperbridge/src/hb_lift_x64.c`
- `engine/hyperbridge/src/hb_interpreter.c`
- `engine/hyperbridge/tests/hb_test_runner.c`
- `engine/hyperbridge/tests/hb_diff_case_runner.c`
- `engine/hyperbridge/tests/hb_fuzz_diff.py`
- `engine/hyperbridge/reports/hb_fuzz_diff_last.json`
- `tools/hb_oracle/unicorn_adapter.py`
- `reports/HB-FUZZ-CORRECTNESS-20260601.md`

No JIT codegen was edited. No local main tree was touched. No merge was performed.

## Addendum - base e256a3f MAX Unicorn pass

Base: `e256a3f`.

Harness hardening:

- `hb_fuzz_diff.py` now defaults to interpreter-vs-Unicorn only; JIT backend diff is opt-in.
- Added multi-seed sharding, `--no-build`, and per-shard `--report-json`.
- Classified Unicorn CPU exceptions as shared traps when HyperBridge also traps.
- Tightened undefined-flag masks for DIV, BT/BTS/BTR/BTC, COMISS, and PCMPxSTR.
- Fixed the VPCLMULQDQ fuzz template byte sequence and added PCMPxSTR template coverage.

Correctness fixes:

- SSE/AVX scalar and packed ADD/SUB/MUL/DIV now choose NaN payloads by Intel/Unicorn behavior: select the NaN operand with the larger significand payload, quiet signaling NaNs, and use the x86 default negative quiet NaN for invalid non-NaN operations such as `0/0`, `inf/inf`, `inf + -inf`, and `inf * 0`.
- Added permanent regression coverage for the exact `DIVPS` and `MULPS` Unicorn mismatches.

Final fuzz evidence:

- `reports/fuzz-max-20260601-post-fp2/shard*.json`
- total cases: 10,000,000
- Unicorn checked: 8,613,888
- Unicorn oracle mismatches: 0
- seeds: `0x90000001`, `0x90000002`, `0xa0000001`, `0xa0000002`, `0xb0000001`, `0xb0000002`, `0xc0000001`, `0xc0000002`
- shared DIV traps: 49,357
- Unicorn unsupported AVX/FMA/F16C cases: 1,386,112
- Unicorn VEX semantic quirks: 495,040

Focused confirmations:

- `python3 tests/hb_fuzz_diff.py --no-build --families sse,sse2 --cases 300000 --batch 4096 --seed-list 0xa7387e3aacb72dd2,0xe256a3f --report-json reports/hb_fuzz_diff_sse_post_fp_nan.json --stop-on-mismatch`: 300,000 checked, 0 mismatches
- `python3 tests/hb_fuzz_diff.py --no-build --families sse,sse2 --cases 300000 --batch 4096 --seed-list 0xa7387e3aacb72dd2,0xe256a3f,0x8d1d00a2880b523d --report-json reports/hb_fuzz_diff_sse_post_fp_nan2.json --stop-on-mismatch`: 300,000 checked, 0 mismatches

Verification:

- `make && ./tests/hb_test_runner`: 419 passed, 0 failed
- `./tests/hb_test_runner --fast-family phase1_core`: 42 passed, 0 failed
- `python3 tests/x64_isa_coverage.py --random 8000`: random valid capstone 6112/6112 decoded, top_missing empty

Changed files in this addendum:

- `engine/hyperbridge/src/hb_interpreter.c`
- `engine/hyperbridge/tests/hb_test_runner.c`
- `engine/hyperbridge/tests/hb_diff_case_runner.c`
- `engine/hyperbridge/tests/hb_fuzz_diff.py`
- `tools/hb_oracle/unicorn_adapter.py`
- `reports/HB-FUZZ-CORRECTNESS-20260601.md`
- `FILES.md`

No JIT codegen was edited. No local main tree was touched. No merge was performed.
