# AUTONOMOUS LANE (OFFLINE): Hollow Knight — does the translated shader emit colour?

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-SHADER-PROGRESS.md`. Work continuously; do not wait for a human
between steps.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Your question

Hollow Knight draws (`DrawIndexed≈41807`, `Present≈857`, `Clear≈2270`, faults=0,
`Performing automatic level start.` reached) and the render target is black. Two other
lanes own the other halves: one is proving whether the *presented surface* is black, one
built a probe for degenerate *camera transforms*. **Your lane owns the third candidate:
DXBC→Metal shader translation. If `airconv` produces a fragment shader that writes zero /
discards / never binds its texture, you get exactly this symptom — many draws, no colour.**

Read first, do not re-derive:
- `reports/phase4-hollow-knight/C1-CAUSAL-TARGET-READBACK-CONTROL-20260726.md` — the magenta
  control that proved our readback transport honest. A C1-injected `rt0=1,0,1,1` reaches the
  target, so the attachment and the encoder are fine; whatever is black is black *before*
  the readback.
- `reports/phase4-hollow-knight/HK-TRANSFORM-RUN-OWNER-HANDOFF.md` — what the transform lane
  already ruled out (translator SSE matrix families MATCH x86; MXCSR/FPCR neutral).

## Territory — hard boundaries, two other agents are editing this repo right now

- **YOURS:** `engine/dxmt/src/airconv/**`, plus `tools/**` and `reports/**`.
- **FORBIDDEN, another lane is mid-edit:** `engine/dxmt/src/winemetal/**`,
  `engine/dxmt/src/d3d11/**`.
- **FORBIDDEN, holds 400+ lines of uncommitted work preserved only in a checkpoint:**
  `engine/wine/dlls/ntdll/unix/macrunner_hb.c` and the rest of `engine/wine/**`.
- **NEVER run Hollow Knight or any game, never launch `scripts/mr-run.sh`.** Another lane
  owns the single JIT run slot; a second title under JIT invalidates both runs. Your whole
  question is answerable offline — that is why this lane exists.
- **Never `pkill`/`killall`,** not even scoped: a live run belonging to another lane is in
  flight.
- **No commits, no `git add`.** Leave work dirty and write a report instead.

## Loop

Each iteration: pick ONE question → smallest instrument or offline experiment that answers
it → **prove the instrument honest** → analyse → write verdict → next question.

Immediate queue (reorder only with a reason recorded in the report):
1. **Get real shaders.** Find Hollow Knight's actual DXBC shader bytecode — from a captured
   run under `reports/phase4-hollow-knight/**`, from the game's own assets, or by any
   offline means. If none is captured anywhere, say so explicitly and design the smallest
   capture hook for the run lane to add later; do not fabricate a substitute and call it HK.
2. **Translate offline and inspect the output.** Run those shaders through `airconv` outside
   the game and read the generated Metal/AIR. For each fragment shader, answer concretely:
   does it write a non-zero value to colour attachment 0 on any path? Is the write ever
   dominated by a `discard` or a constant-false branch? Are texture/sampler bindings
   resolved, or dropped to a default that samples black?
3. **Prove your reading with a known-good control.** Translate a shader whose correct output
   you can state in advance — one that must emit constant magenta — and confirm the
   generated code emits it. A translator you have never fed a known input is not evidence.
4. **Differential check.** Where the generated code is suspicious, compare against the DXBC
   semantics instruction by instruction and name the exact opcode or binding rule that
   diverges. "Looks wrong" is not a finding; `opcode X translated as Y, should be Z` is.

## Hard gates — every one cost this project a day

- **Verify the artifact actually landed.** If you build anything, check the SHA of the file
  that ends up in the dist path, not merely that the build exited 0. We have shipped a stale
  binary while believing it was fresh.
- **Build for the architecture the game loads** (x86_64-windows for the game's DLLs). We
  once instrumented the unused aarch64 copy and lost a day of measurements.
- **`not logged` ≠ `did not happen`.** Never conclude absence from a trace or a dump you have
  not proven active and unlimited.
- **Unity's own lines go to `launch.stdout`; ours to `launch.stderr`. Grep BOTH,
  case-SENSITIVE.** `grep -i present` matches `ddraw` and paths, and has already produced two
  false findings here.

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to `reports/research/LANE-HK-SHADER-PROGRESS.md`.
- State evidence, not status: paste the actual generated code, SHA, or opcode. "Blocked at X"
  with the exact blocker named is a valid result. Never fake forward progress.
- Mark every unproven statement as `[HYPOTHESIS]`.

## Termination

- `LOOP-STATUS: GOAL` — shader translation is confirmed or refuted as the cause of the black
  frame, with evidence a skeptical reviewer cannot dismiss.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide (a commit, the
  game-run slot, a destructive action, quota). Name it in one sentence.
- Otherwise keep going: next question, next experiment, next verdict.
