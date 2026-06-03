# HyperBridge Lane B — x87 FXAM/FPREM correctness

Date: 2026-06-03

Scope: Lane B x87 helper semantics, x87 oracle harness, and regression tests.
Lane A JIT/codegen files were not edited.

## Family Audit

Family: x87 condition-code semantics.

Members:
- `FXAM` (`D9 E5`)
- `FPREM1` (`D9 F5`)
- `FPREM` (`D9 F8`)

Trigger:
- `hb_fuzz_x87.py --cases 50 --seed 20260603` reported deterministic
  `FXAM` C-bit mismatches (`HB c03=0x0000`, Unicorn `0x0400`) and
  `FPREM/FPREM1` condition-bit mismatches.

Coverage in this fix:
- `[x] FXAM` — fixed `C3:C2:C0` class encoding and `C1` sign.
- `[x] FPREM` — fixed complete quotient condition bits.
- `[x] FPREM1` — fixed complete quotient condition bits.
- `[x] Oracle harness` — separated preamble memory from target memory and
  fixed malformed signed/ext80 fixtures.
- `[x] Invalid zero-divisor oracle limit` — documented and skipped
  `FPREM/FPREM1` C-bit comparison for ST(1)=+/-0.

Regression tests:
- `x87_fxam_class_condition_codes`
- `x87_fprem_condition_codes`

Audit completed: yes.

## Evidence

Build:
- `make` passed.
- `make tests/hb_test_runner` passed.
- manual `tests/hb_diff_case_runner` rebuild with project `-Werror` flags passed.

Runner:
- `./tests/hb_test_runner` -> `444 passed, 2 failed`.
- Remaining failures are current baseline JIT/mprotect lines:
  `tests/hb_test_runner.c:7088` and `tests/hb_test_runner.c:14724`.

Oracle:
- `python3 tools/hb_oracle/hb_fuzz_x87.py --template fxam --cases 500 --seed 20260603`
  -> `mismatch_count: 0`.
- `python3 tools/hb_oracle/hb_fuzz_x87.py --template fprem --cases 500 --seed 20260603`
  -> `mismatch_count: 0`.
- `python3 tools/hb_oracle/hb_fuzz_x87.py --template fprem1 --cases 500 --seed 20260603`
  -> `mismatch_count: 0`.

## Remaining Lane B x87 Work

The broad x87 smoke still has residual non-FXAM/non-FPREM mismatches, mostly:
- Unicorn tag-word classification noise for special values.
- ST7 empty-stack interpreter errors in templates that intentionally address
  `ST(7)` after a four-push preamble.
- `FXTRACT` tag/result edge cases.
- `FCMOV*` tag-word differences.

Next pass should classify those into engine bugs vs oracle limits before
changing code.
