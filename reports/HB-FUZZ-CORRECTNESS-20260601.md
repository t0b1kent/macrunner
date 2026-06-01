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
