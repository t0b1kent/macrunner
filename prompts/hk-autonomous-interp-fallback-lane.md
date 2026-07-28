# AUTONOMOUS LANE (OFFLINE): why does execution sit in the interpreter, and what does it cost us?

Auto-loop lane. Relaunched until `LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-INTERP-PROGRESS.md`.
Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## The measurement that opened this lane

A live Hollow Knight process that looked "quiet-stalled" (185% CPU, no log growth, static
screen for 20s) was sampled with `sample(1)` while it was still running. The profile is
**100% interpreter**: 2282 interpreting frames, **0** JIT/translation frames.

Hot leaves, in order: `exec_instr_unlocked` 697, `hb_memory_read` 532,
`hb_flags_read_operand_value` 285, `hb_jit_helper_exec_interp_ir` 282,
`hb_jit_helper_exec_ir_block_once` 279, `hb_jit_helper_exec_two_block_loop` 260,
`_tlv_get_addr` 232 (libdyld), `hb_jit_helper_exec_load_operand_lazy` 207,
`read_vec_reg_bytes` 154, `hb_memory_write` 145, `find_region_normalized` 132.

Raw sample preserved for you at `reports/phase4-hollow-knight/interp-stall-sample-20260728.txt`.

**So the "stall" was not a deadlock — the game was running, interpreted, at roughly
interpreter speed.** Mihocka (who built xtajit/xtabase at Microsoft) puts interpretation at
5-10% of native, which for a Unity title is indistinguishable from frozen. The operator
independently reported the game "stuttering, then hanging" — the same thing seen from outside.

That run carried the engine lane's observability profile:
`MACRUNNER_HB_JIT_DIRECT_MEM=0`, `..._SCALAR_MEM=0`, `..._STACK=0`, `..._XMM_MEM=0`.

## Your questions

1. **Is the interpreter path a deliberate consequence of those four knobs, or is it entered
   even with them on?** Read the dispatch logic and say exactly what selects interpreter vs
   translated execution, quoting the code. `_tlv_get_addr` at 232 samples suggests a
   thread-local lookup on a very hot path — worth naming.
2. **What is the actual cost split?** From the sample, attribute time: memory helpers vs flag
   emulation vs vector-register access vs region lookup (`find_region_normalized`). Which one
   would repay optimisation first, with numbers, not intuition.
3. **`find_region_normalized` at 132 samples is suspicious** — a region lookup on every guest
   memory access. Is it O(n) over a region list? If so, that is a concrete algorithmic fix.
4. **What would a hybrid look like here?** Mihocka's own regret about xtabase was that
   interpreter and JIT shipped as separate components rather than a hotspot-style hybrid
   (interpret everything, promote hot blocks). We appear to have both mechanisms already.
   Sketch what promoting hot interpreted blocks would take in this codebase — read-only
   design note, no edits.

## Territory — READ-ONLY on the engine, three other agents are live in this repo

- **YOURS to write:** `reports/**` and `tools/**` only.
- **READ-ONLY:** `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/hyperbridge/**` — the
  engine lane is editing them and they carry uncommitted work. Do not edit, do not revert.
- **FORBIDDEN:** `engine/wine/dlls/winemac.drv/**` (input lane), `engine/dxmt/**`.
- **NEVER run Hollow Knight, never launch `scripts/mr-run.sh`, never `pkill`/`killall`.**
- **No commits, no `git add`.**

## Hard gates

- **Quote the code** behind every claim about dispatch. A claim about control flow with no
  function body behind it is a guess.
- **Numbers over adjectives.** "Slow" is not a finding; "N% of samples in X" is.
- **`not found` != `not there`.** Say what you searched.
- Mark every unproven statement `[HYPOTHESIS]`.

## Termination

- `LOOP-STATUS: GOAL` — you can say what selects the interpreter, what it costs by component
  with numbers, and which single change would buy the most speed.
- `LOOP-STATUS: BLOCKED` — name in one sentence what only the operator can decide.
