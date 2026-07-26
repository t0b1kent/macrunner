# AUTONOMOUS LANE (OFFLINE): Hollow Knight — is the transform degenerate?

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-TRANSFORM-PROGRESS.md`. Work continuously; do not wait for a
human between steps.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Your question

Hollow Knight renders (`DrawIndexed≈41807`, `Present≈857`, `Clear≈2270`, faults=0,
`Performing automatic level start.` reached) but the screen is black. Another lane is
proving whether the *presented surface* is black. **Your lane owns a different half:
are the draws being fed a degenerate camera transform, so that geometry is drawn
off-screen or collapsed to zero area?**

Unity emits `Screen position out of view frustum (screen pos 0.000000, 767.000000)
(Camera rect 0 0 1024 768)` and repeatedly `Couldn't find a UIManager`.

Read first, do not re-derive:
- `reports/phase4-hollow-knight/HK-VIEW-FRUSTUM-ANOMALY-ANALYSIS.md` — **section 5 is your
  spec** (capture points + matrix analysis rule). Its verdict is that a degenerate
  view/projection matrix remains a valid hypothesis that MUST be confirmed by capture.
- `reports/phase4-hollow-knight/C1-CAUSAL-TARGET-READBACK-CONTROL-20260726.md` — the
  magenta control that established our readback transport is honest.

## Territory — hard boundaries, another agent is editing this repo right now

- **YOURS:** `engine/dxmt/src/d3d11/**`, `tools/**` (analysis scripts), `reports/**`.
- **FORBIDDEN — do not open for writing, another lane is mid-edit:**
  `engine/dxmt/src/winemetal/**`.
- **FORBIDDEN — holds 400+ lines of uncommitted work preserved only in a checkpoint:**
  `engine/wine/dlls/ntdll/unix/macrunner_hb.c` and the rest of `engine/wine/**`.
- **NEVER run Hollow Knight or any other game, and never launch `scripts/mr-run.sh`.**
  Another lane owns the single JIT run slot; a second title under JIT invalidates both
  runs. Your deliverable is a built, self-verified, default-off instrument plus offline
  analysis. Someone else pulls the trigger.
- **Never `pkill`/`killall`,** not even scoped — a live run belonging to the other lane is
  in flight.
- **No commits, no `git add`.** Leave work dirty and write a report instead.

## Loop

Each iteration: pick ONE question → build the smallest instrument that answers it →
**prove the instrument honest** → analyse → write verdict → next question.

Immediate queue (reorder only with a reason recorded in the report):
1. **VS constant-buffer dump.** In `engine/dxmt/src/d3d11/d3d11_context_impl.cpp`, follow
   the pattern ALREADY in that file (`DXMT_HK_DRAW_TRACE_*` kinds,
   `getenv("MACRUNNER_DXMT_DRAW_TRACE_MAX")`, `MACRUNNER_DXMT_SWAPCHAIN_TRACE`) — do not
   invent a new mechanism. Add a default-off gate (e.g. `MACRUNNER_DXMT_VS_CB_DUMP`, plus
   `_MAX`) that at draw time records the bound VS constant buffer: slot, byte size, the
   leading 4x4 float matrices, and a classification per matrix —
   `IDENTITY` / `ZERO` / `NAN_OR_INF` / `DEGENERATE` (near-zero determinant) / `PLAUSIBLE`.
2. **Self-control for the classifier.** Feed a known matrix through the classifier
   (identity, a zero matrix, a NaN matrix, a plausible perspective matrix) and show it
   reports the right class for each. A classifier that has never been shown a known input
   is not evidence.
3. **Aggregate totals at exit.** Counts per classification over the whole run, not just the
   first N records. First-N sampling has already produced two false conclusions on this
   project — a capped trace makes counts meaningless.
4. **Offline provenance.** Where does Hollow Knight's camera matrix come from, and which
   translated x86-64 code computes it? If a specific instruction family is involved
   (e.g. SSE reciprocal/rsqrt/`DIVPS`, `CVTPS2DQ`, denormal handling), check our
   translator's implementation against x86 semantics offline and report any divergence
   that could zero or NaN a matrix. This part needs no run at all.

## Hard gates — every one cost this project a day

- **Build for x86_64-windows.** The game loads the x86_64 DLLs. We once put the Draw trace
  in the unused aarch64 copy and lost a day of measurements. Use `scripts/build-dxmt.sh`,
  then state the produced `d3d11.dll` SHA-256 in your report.
- **Verify the artifact actually landed** — check the SHA of the file that ends up in the
  dist/system32 path, not merely that the build exited 0. A publish step can die silently
  and we have shipped a stale binary while believing it was fresh.
- **Prove the tool before trusting the number.** "It compiled" is not evidence.
- **`not logged` ≠ `did not happen`.** Never conclude absence from a trace you have not
  proven active and unlimited.
- **Unity's own lines go to `launch.stdout`; ours to `launch.stderr`. Grep BOTH,
  case-SENSITIVE.** `grep -i present` matches `ddraw` and paths, and has already produced
  two false findings.

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to `reports/research/LANE-HK-TRANSFORM-PROGRESS.md`.
- State evidence, not status: paste the actual log line, SHA, or number. "Blocked at X"
  with the exact blocker named is a valid result. Never fake forward progress.
- Mark every unproven statement as `[HYPOTHESIS]`.

## Termination

- `LOOP-STATUS: GOAL` — the transform hypothesis is confirmed or refuted with evidence a
  skeptical reviewer cannot dismiss, and the instrument that proves it is built and
  self-verified.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide (a commit, the
  game-run slot, a destructive action, quota). Name it in one sentence.
- Otherwise keep going: next question, next instrument, next analysis.
