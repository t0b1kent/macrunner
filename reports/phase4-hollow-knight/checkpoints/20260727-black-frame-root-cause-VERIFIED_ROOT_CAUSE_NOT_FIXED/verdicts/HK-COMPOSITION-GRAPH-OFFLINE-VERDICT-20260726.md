# HK Composition Graph — offline verdict (2026-07-26)

**Question:** do Hollow Knight's draws land in a render target that is never presented?
**Verdict: NO — composition candidate REFUTED at the structural level.** The swapchain
backbuffer RTV is bound in every covered frame (including 209 frames *after*
`Performing automatic level start.`), is always the *last* target bound before
`Present1`, and 128/128 sampled `DrawIndexed` across two independent runs hit the
backbuffer RTV. The "backbuffer only ever receives clears" asymmetry is not just
absent — the sampled asymmetry is *inverted*: sampled clears go mostly to offscreen
RTVs, sampled draws go exclusively to the backbuffer RTV.

Analyzer: `tools/hk_composition/analyze_swaptrace.py` (reproducible, no game run needed).

## Runs analyzed (each separately, pointers never mixed)

| run | file | totals (uncapped, last `dxmt-drawtrace-totals`) |
|---|---|---|
| A | `laneA-sidechannel-grid-20260726-120316/launch.stdout` | drawindexed=41807 clear=2433 omset=15486 present=857 |
| B | `laneA-ring-ledger-v4-20260725-205648/launch.stdout` | drawindexed=39022 clear=2270 omset=14446 present=803 |
| C | `laneA-ring-ledger-v2-20260725-133318/launch.stdout` | (no totals line; census only) |

## 1. Identity chain — CLOSED in all three runs

- Run A: swapchain `0xee232e6a0` → `GetBuffer iid=ID3D11Resource out=0xee23331d0`
  (same pointer as second `ResizeBuffers iid=backbuffer out=0xee23331d0`)
  → `CreateRenderTargetView(1) resource=0xee23331d0` → RTVs **`0xee2333330`, `0xee2333460`**.
- Run B: swapchain `0xee232e6a0`→ (run-B addressing) backbuffer `0xee0f66070`
  → RTVs **`0xee0f661d0`, `0xee0f66300`**.
- Run C: backbuffer `0xeccf66f50` → RTVs **`0xeccf670b0`, `0xeccf671e0`**.
- No missing link in any run. 19 distinct resources have RTVs, 38 RTVs created,
  36 of them offscreen (Unity render/post targets) — normal.

## 2. Binding census — identical in all three runs (deterministic)

Of 4096 logged `OMSetRenderTargets` per run (arg1 = `ppRTVs[0]`, the actual RTV
pointer — verified in `engine/dxmt/src/d3d11/d3d11_context_impl.cpp:3022-3024`):

- **937 binds of the backbuffer RTV** (run A: `0xee2333460`)
- 3158 binds of offscreen RTVs (8 regular + 1 rare; e.g. run A `0x709eccc3e0`×627)
- **1 true unbind** (NumViews=0, ppRTVs=NULL)
- **0 anomalous null binds** (NumViews>0 with RTV=NULL)
- 18 distinct RTVs ever bound in slot 0

The prompt's "arg1=0x0 is the most common binding (3296 records)" does **not**
reproduce in any of these runs — there is exactly one unbind per run. Treat the
3296 figure as an aggregation artifact, not a finding.

## 3. Where the draws fall (capped sample — stated honestly)

`dxmt-hk-drawtrace` logs at most 64 `DrawIndexed` (default
`MACRUNNER_DXMT_DRAW_TRACE_MAX`), and all 64 occur *before* level start
(level start lands after present #48; the 64-draw cap is exhausted around frame 4):

- **Of the first 64 sampled draws in run A: 64/64 → backbuffer RTV `0xee2333460`.**
- **Of the first 64 sampled draws in run B: 64/64 → backbuffer RTV `0xee0f66300`.**
- Post-level-start draws are NOT directly sampled in any existing run (cap
  artifact). Totals for them exist (41807) but per-draw RTV attribution does not.

## 4. Structural proof for post-level frames (this is the decisive part)

The swaptrace slot budget (4096 shared records) covers OMSet events through
frame ~258 in runs A and B. `Performing automatic level start.` happens after
present #48. So frames **49–258 are post-level AND fully covered**:

- **Every one of the 209 covered post-level frames binds the backbuffer RTV
  exactly 4 times** (histogram: {4: 209}, zero frames with 0 backbuffer binds;
  the 602 "zero-bind" frames in a naive count are frames >258 where OMSet logging
  had already stopped — budget artifact, verified: last OMSet record is in
  inter-present interval #258).
- **The last RTV bound before `Present1` is the backbuffer RTV in 100% of covered
  frames** — pre-level (48/48) and post-level (210/210, runs A; pattern identical
  in B given byte-identical census).

So each frame ends: … offscreen passes → bind backbuffer → (final pass) →
`Present1` on swapchain whose GetBuffer texture feeds exactly those RTVs. The
presented surface participates in rendering every single frame. The composition
graph is honest; draws are not being diverted into a never-presented target.

## 5. Clear vs draw asymmetry — hypothesis inverted, not confirmed

Sampled clears (run A, 64): 59 → offscreen RTVs (`0x709d53b930`×20,
`0x709ecc6e40`×20, `0x709ecc7620`×19), 1 → backbuffer RTV. Sampled draws: all →
backbuffer. The predicted signature (clears hit backbuffer, draws hit offscreen
never copied back) does not exist; if anything the observed signature is the
opposite.

## Residual (what this data cannot prove)

Post-level-start `DrawIndexed`→RTV attribution is missing because the drawtrace
cap (64) is exhausted pre-level. If one wants *direct* per-draw proof for scene
frames, the smallest change (queue item 5), default-off, for the run lane:

- In `engine/dxmt/src/d3d11/d3d11_context_impl.cpp`, `dxmt_hk_draw_trace_call`:
  add the context's currently-bound `state_.OutputMerger.RTVs[0]` pointer to the
  printed line (one field, e.g. `boundrtv=%p`). Gate unchanged
  (`MACRUNNER_DXMT_SWAPCHAIN_TRACE`), zero cost when off.
- Run with `MACRUNNER_DXMT_DRAW_TRACE_MAX=8192` (covers ~160 post-level frames
  at ~49 draws/frame) — or keep 64 and add a `MACRUNNER_DXMT_DRAW_TRACE_SKIP=N`
  first-N-skip so the logged window lands after level start.
- Territory: `engine/dxmt/src/d3d11/**` only; do NOT touch `winemetal/**`.

## Consequence for HK-COMBINED-DECISIVE-RUN-SPEC

The composition branch can be de-prioritized: the run no longer needs to
distinguish "draws land offscreen forever" — traces already refute it. What
remains worth measuring on screen: whether the final backbuffer pass produces
non-black pixels (value production / blending / present path), since target
identity (this lane), attachment/encoder/readback honesty (C1), and shader
translation (shader lane) are now all cleared.
