# AUTONOMOUS LANE: Hollow Knight — unblock the managed scene bootstrap (Mono/JIT)

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-MONO-PROGRESS.md`. Work continuously; do not wait for a human
between steps.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Goal

Make Hollow Knight's managed scene bootstrap advance past
`Performing automatic level start.` — which is what stands between us and the first
visible pixel. Either (a) get the bootstrap to advance, or (b) prove the exact mechanism
that stops it, with evidence that survives adversarial checking.

## Established — do NOT re-derive, do NOT reopen

The black frame is **not a graphics defect**. Full verdict:
`reports/phase4-hollow-knight/HK-BLACK-FRAME-ROOT-CAUSE-VERDICT-20260727.md`.

- The managed sequence halts right after `Performing automatic level start.` No scene is
  created, so every presented frame is the cleared backbuffer `(0,0,0,255)`.
- Proof is **differential against the Windows-Prism oracle**: the shared marker
  `Game controller set to None.` is present in both logs (so the trace is live), and every
  oracle marker after it is **0** in both of our runs, while
  `Screen position out of view frustum` is 0 on the oracle and 19817 / 17927 on ours.
- Every graphics layer is independently exonerated: causal ladder C2/C3 both full-viewport
  MAGENTA at 655360/655360; 152/152 HK DXBC blobs translate clean (rt0 82/82, discard parity
  28/28); composition binds the backbuffer RTV 210/210 last-bound-before-Present1; Unity
  itself writes the black vertex colours `(0,0,0,5/255)`; translator SSE matrix families
  match x86, MXCSR `0x1f80`, host `FPCR=0x0`. **Do not spend a run re-testing any of these.**

## The open question — this is your lane

The divergence point coincides exactly with the producer thread entering the Mono
`jit_code_hash` spin, stack-captured 2026-07-26:

```
hb_jit_runtime_run → generated ARM64 → hb_jit_helper_exec_two_block_loop → exec_instr → mem_read
```

The spin is reported as **finite (~15 min in both runs)**, and presents resume afterwards —
yet the managed sequence never advances. **So "the spin is the cause" is a
[HYPOTHESIS] built on coincidence of location, not a proven mechanism.** Your first job is
to settle exactly that:

1. **Does the managed thread survive the spin?** After the spin ends, is the same thread
   running managed code, blocked, or looping elsewhere? A finite spin followed by
   permanent non-advancement needs a mechanism — name it.
2. **What is being looked up, and does it ever succeed?** `jit_code_hash` /
   `mono_internal_hash_table_lookup`: instrument the lookup — key, hit/miss, and whether the
   miss path re-enters compilation. A hash lookup that never hits because our translation
   corrupts the key would explain a permanent stall with a live thread.
3. **Compare against the oracle's timing.** The oracle reaches a fully rendered main menu at
   +33.9 s. Ours spends ~15 min in one spin. Where does the oracle's equivalent work go?
4. **Then attempt a fix** and validate it with the regression detector below.

## Prior attempts were invalidated by ORCHESTRATION, not by findings — do not repeat them

- `PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`:
  `UNKNOWN_MONO_HASH_CYCLE_VALIDATION_FAILED_WRONG_HELPER_STATE` — the helper state under
  test was not the one actually live.
- `MONO-HASH-CYCLE-LOG-GATE-INVALID-20260726.md`: `INVALID_PRE_PRODUCT_ORCHESTRATION` — the
  log gate itself was invalid, so no JIT/chain/pixel conclusion from that run counts.

Before trusting any measurement, prove the instrumented state is the state the game is
actually executing.

## Regression detector — you now have a real one, use it

Ordinal-200 presented-surface readback, expected-black pixel hash
`0xc770038f717d0383`, `black=786432 nonblack=0`, byte-identical across two runs.
**A working fix turns that readback non-black.** Acceptance checklist is the oracle's
post-level marker sequence: `Loaded saved language code 'EN'` → `Making UI menu lean.` →
`Opening_Sequence` tilemap fallbacks → `Levels are ready before cinematics…` → first visible
pixel. Any candidate fix is judged against those markers, not against a feeling.

## Territory and safety

- **YOURS:** `engine/wine/dlls/ntdll/unix/macrunner_hb.c` and the JIT/Mono side, plus
  `tools/**` and `reports/**`. The operator confirmed nobody else is working there now.
- **`macrunner_hb.c` currently holds 400+ lines of UNCOMMITTED work** (ledger V4), preserved
  only in `reports/phase4-hollow-knight/checkpoints/`. **Never discard or `git checkout` it.**
  If you must restructure, preserve the existing diff into a checkpoint first.
- **You own the JIT run slot** — no other lane is running a title. Still: **one title at a
  time**, and never touch another lane's dist or prefix.
- **Never `pkill`/`killall`** globally; scope by verified PID or prefix. `mr-clean --prune`
  has previously destroyed the HK prefix-template's save snapshot — never delete
  `artifacts/hk-windows-oracle-prefix-template*`.
- **No commits, no `git add`.** Preserve work in a checkpoint instead.

## Hard gates — each cost this project a day

- **Verify the artifact actually landed.** After prefix-sync, check the SHA of the file in
  `system32` — not just that the build exited 0. We have shipped a stale `ntdll.so` while
  believing it was fresh; the root cause was a `makedep.c` dependency bug.
- **Prove the tool before trusting the number.** A control with a known-in-advance result
  runs FIRST. "It compiled" is not evidence.
- **`not logged` ≠ `did not happen`.** Never conclude absence from a trace you have not
  proven active and unlimited. Sampling caps (first-N) make counts meaningless — emit
  aggregate totals.
- **Unity's own lines go to `launch.stdout`; ours to `launch.stderr`. Grep BOTH,
  case-SENSITIVE.** `grep -i present` matches `ddraw` and paths and has produced two false
  findings here. A verdict was retracted this week for grepping only `launch.stderr`.
- **Redirect and verify logs**: at +2 minutes confirm both `launch.stdout` and
  `launch.stderr` exist and are growing. If not, stop — do not burn the budget.
- **Gate on progress, not on a timer.** Under emulation the boot takes tens of minutes.
- **Liveness ≠ progress.** A thread that is `R(running)` with a tiny distinct-PC set is
  busy-spinning, not advancing. Require a boundary advance or a two-sample diff.

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to `reports/research/LANE-HK-MONO-PROGRESS.md`, and
  write your reports as files — both count as work.
- State evidence, not status: paste the actual log line, SHA, address or number. "Blocked at
  X" with the exact blocker named is a valid result. Never fake forward progress.
- Mark every unproven statement as `[HYPOTHESIS]`.

## Termination

- `LOOP-STATUS: GOAL` — the managed bootstrap advances past the oracle markers (ideally the
  ordinal-200 readback goes non-black), or the stopping mechanism is proven beyond
  adversarial dispute.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide (a commit, a
  destructive action, hardware/quota). Name it in one sentence.
- Otherwise keep going: next question, next instrument, next run.
