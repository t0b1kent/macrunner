# HB Differential Correctness Fuzzing - 2026-06-01

Base: `18be487 checkpoint(Lane A): gate broken - past NtWaitForSingleObject deadlock, now on CRT/entry init`

## Oracle Status

Intel SDE is not installed in this kit environment: `sde64`/`sde` not found.

`tools/hb_oracle/sde_adapter.py` and `tools/hb_oracle/hyperbridge_adapter.py` are still scaffolds and raise `NotImplementedError`, so no SDE result is claimed. The implemented fallback is:

- exact Python identities for selected integer flags, logic flags, shifts, CMOVNE, SETNE, TZCNT/LZCNT/POPCNT when the instruction executes
- interpreter-vs-JIT whole-state differential for GPRs, modeled EFLAGS bits, XMM/YMM state, and scratch memory

## Work Done

Added:

- `engine/hyperbridge/tests/hb_diff_case_runner.c`: single-instruction differential runner. It seeds random x64 state, maps code/data/stack memory, executes the same lifted instruction through interpreter and JIT, then emits JSON snapshots.
- `engine/hyperbridge/tests/hb_fuzz_diff.py`: seeded template fuzzer over game-relevant families with exact-oracle checks and fallback backend diffing.

Fixed in the new harness:

- Lazy flags are now materialized through each instruction's defined flag mask instead of forcing `HB_FLAG_BIT_ALL`. This avoids false mismatches for instructions with undefined flags such as AF after `AND/OR/XOR/TEST` and OF after multi-bit shifts.

No decode/lift/interpreter semantic fix was required by the exact-oracle failures in this run. No JIT codegen was edited.

## Fuzz Evidence

Command:

`python3 tests/hb_fuzz_diff.py --cases 1000000 --batch 4096`

Result:

- cases run: 1,000,000
- exact-oracle checked: 608,698
- exact-oracle passed: 608,698
- exact-oracle mismatches: 0
- backend mismatches: 43,478

Family distribution:

- int_arith_flags: 217,395
- int_logic_flags: 173,913
- shift_rotate_flags: 43,478
- cmov_setcc: 86,956
- bmi: 130,434
- sse: 130,434
- sse2: 43,478
- string_ops: 173,912

Backend mismatch minimized sample:

- instruction: `SETNE r8b`
- bytes: `41 0f 95 c0`
- seed: `0xee8b31b0ef0351eb`
- initial `r8`: `0x2aa9b3c6758e4412`
- initial `ZF`: 1
- expected/interpreter `r8`: `0x2aa9b3c6758e4400`
- JIT `r8`: `0x0000000000000000`

Classification: interpreter matches exact SETcc oracle; JIT zeroes the whole destination register. This is a JIT codegen bug and was not fixed because the task scope forbids JIT edits.

Unsupported counted, not hidden:

- `POPCNT r8,r9` (`f3 4d 0f b8 c1`): interpreter and JIT both return unsupported in 43,478 cases.

## Verification

- `make && ./tests/hb_test_runner`: pass, 416 passed, 0 failed
- `./tests/hb_test_runner --fast-family phase1_core`: pass, 41 passed, 0 failed
- `python3 tests/hb_fuzz_diff.py --cases 1000000 --batch 4096`: exit 1 due known JIT backend mismatch; exact interpreter oracle mismatches 0

## Remaining Gaps

- Real SDE differential mode still requires implementing `tools/hb_oracle/sde_adapter.py` and installing Intel SDE.
- JIT `SETNE r8b` partial-register behavior remains wrong but is out of Lane B scope.
- `POPCNT r8,r9` remains unsupported by both tested backends.
- SSE/string templates currently rely on backend differential only unless exact hardware/SDE oracle support is added.

No local main tree was touched. No merge was performed.
