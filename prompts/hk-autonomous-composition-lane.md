# AUTONOMOUS LANE (OFFLINE, NO RUN NEEDED): where do Hollow Knight's draws actually land?

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-COMPOSITION-PROGRESS.md`. Work continuously; do not wait for a
human between steps.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Your question

Hollow Knight draws (`DrawIndexed≈41807`, `Present≈857`, `Clear≈2270`, faults=0,
`Performing automatic level start.` reached) and the screen is black. Two candidates have
already been eliminated (see below). Yours is the composition graph:

**Are the draws landing in a render target that is never presented?**

`OMSetRenderTargets` runs ~18 times per frame. If Unity binds an offscreen target for the
scene draws and the swapchain backbuffer only ever receives clears, you get exactly this
symptom: tens of thousands of draws, a working pipeline, and a black window.

## The data already exists — no game run is required for the first pass

Captured runs under `reports/phase4-hollow-knight/laneA-*2026072*/launch.stdout` contain a
`dxmt-hk-swaptrace:` stream with everything needed to build the identity chain:

| method | records | why it matters |
|---|---|---|
| `ResizeBuffers` … `iid=backbuffer out=0x…` | 52 | names the backbuffer object |
| `GetBuffer` | 26 | swapchain → backbuffer texture |
| `CreateRenderTargetView` / `…View1` | 232 | texture → RTV pointer |
| `OMSetRenderTargets` `arg0=<NumViews> arg1=<ppRTVs> arg2=<pDSV>` | 21204 | every binding |
| `ClearRenderTargetView` | 144 | which target gets cleared |
| `DrawIndexed` | 1024 | **capped sample, see the gate below** |
| `Present` / `Present1` | 3721 each | frame boundaries |

An immediately suspicious fact to explain, NOT to assume: `arg1=0x0` is the single most
common binding (3296 records). Check `arg0` on those — `NumViews=0, ppRTVs=NULL` is a
legitimate unbind that Unity does routinely, so do not call it a finding until you have
separated genuine unbinds from a null bind with `NumViews>0`.

## Immediate queue

1. **Identity chain.** For each captured run separately (never mix runs — different runs
   have different pointer values), resolve: swapchain → backbuffer → RTV pointer. State the
   exact addresses. If the chain cannot be closed from the trace, say precisely which link
   is missing.
2. **Binding census.** Of the `OMSetRenderTargets` records, how many bind the backbuffer
   RTV, how many bind other (offscreen) RTVs, how many are true unbinds? Report counts, and
   how many DISTINCT RTVs exist.
3. **Where the draws fall.** Correlate each `DrawIndexed` with the RTV that was bound on the
   same `tid` at that point in the stream. Does the scene geometry go to the backbuffer or
   to an offscreen target? This is the question that decides the lane.
4. **Clear vs draw asymmetry.** Which target do the `ClearRenderTargetView` calls hit? If
   clears hit the backbuffer while draws hit an offscreen target that is never copied back,
   that IS the root cause — and it would be provable from these logs alone.
5. **If and only if the logs cannot answer it,** design the smallest additional trace field
   needed (e.g. resolved RTV pointer per draw) in `engine/dxmt/src/d3d11/**`, default-off,
   and hand it to the run lane. Do not run it yourself.

## Hard gate specific to this data

**The `DrawIndexed` trace is CAPPED at first-N (1024 records here, against a real
`DrawIndexed≈41807`).** Counts from it are a sample, not a total. Never write "only N draws
happened" from a capped trace — that exact mistake has already produced a false finding on
this project. Say "of the first 1024 sampled draws, X went to …", and if you need totals,
say so as a blocker.

## Territory — hard boundaries, another agent is editing this repo right now

- **YOURS:** `tools/**`, `reports/**`, and `engine/dxmt/src/d3d11/**` if queue item 5 is
  reached.
- **FORBIDDEN, another lane is mid-edit:** `engine/dxmt/src/winemetal/**`.
- **FORBIDDEN, holds 400+ lines of uncommitted work preserved only in a checkpoint:**
  `engine/wine/dlls/ntdll/unix/macrunner_hb.c` and the rest of `engine/wine/**`.
- **NEVER run Hollow Knight or any game, never launch `scripts/mr-run.sh`,** and never
  `pkill`/`killall`: another lane owns the single JIT run slot and has a live run in flight.
- **No commits, no `git add`.**

## Already settled — do not re-derive, do not re-run

- **C1 magenta control PASSED**: injected `rt0=1,0,1,1` reached the target, encoder `rtv`
  equalled the readback address. The attachment, encoder and readback transport are honest.
- **Shader translation REFUTED** (`SHADER-LANE-AIRCONV-OFFLINE-VERDICT-20260726.md`,
  independently re-verified): 152/152 HK DXBC blobs translate, a known-good
  `mov o0.xyzw, l(1,0,1,1); ret` control emits the constant to rt0, 82/82 PS write rt0,
  0 PS with `o0_writes==0`, discard parity 28/28.
- **Translator SSE matrix arithmetic MATCHES x86**; MXCSR `0x1f80`, host `FPCR=0x0`.
- A combined decisive run is queued for the run-owning lane
  (`HK-COMBINED-DECISIVE-RUN-SPEC.md`). Your offline answer may well arrive first — if it
  does, say so plainly, because it would change what that run should measure.

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to `reports/research/LANE-HK-COMPOSITION-PROGRESS.md`.
- State evidence, not status: paste the actual trace line, address, or count.
- Mark every unproven statement as `[HYPOTHESIS]`.

## Termination

- `LOOP-STATUS: GOAL` — you can say, with evidence from the traces, whether the scene draws
  reach the presented backbuffer or not.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide (a commit, the
  run slot for an uncapped trace, quota). Name it in one sentence.
- Otherwise keep going: next question, next analysis.
