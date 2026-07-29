# Hollow Knight black frame — ROOT-CAUSE VERDICT (final)

> ## ⚠️ SUPERSEDED IN PART — 2026-07-27 13:25, lane HK-MONO
>
> **Both clauses of the Verdict below are refuted.** See
> `HK-ORACLE-TAIL-IS-HUMAN-GAMEPLAY-BOOTSTRAP-COMPLETE-20260727.md`.
>
> 1. **"No scene is ever created"** — false. The menu scene loads: `Unloading 66
>    unused Assets … Loaded Objects now: **47240**` vs oracle **47392** = 99.68 %.
>    (Half-corrected already in the lane journal at 12:43.)
> 2. **"The managed scene bootstrap halts at `Performing automatic level start.`"**
>    — false. It *completes*. The oracle markers used as the halt proof
>    (`Making UI menu lean.` → `Opening_Sequence` → `Levels are ready…`) are emitted
>    only by `UIManager::MakeMenuLean`, whose sole callers are the
>    `<RunStartNewGame>d__359` / `<RunContinueGame>d__361` coroutines — the
>    **Start-Game button flow**. Every caller of `StartNewGame` is an input handler
>    (`StartGameEventTrigger::OnSubmit`, `SaveSlotButton::OnSubmit`, scene-wired
>    UnityEvent buttons); no timer/autostart/command-line path exists.
>    **The oracle `Player.log` is a recording of a human playing the game** (its tail
>    has `Slash Impact R`/`Strike Nail R`/`Run Effects` pool exhaustion,
>    `CameraLockArea.OnTriggerExit2D`, `Opening sequience skipping.`×3).
>    Our runs reproduce every oracle line that needs no input; the first line we lack
>    is the first line that needed a button press.
>
> **What survives:** the graphics-layer exonerations in §"Every graphics layer…"
> are unaffected — but the black frame is now attributed to a *different* graphics-
> adjacent defect the verdict did not test: the VS constant buffers flip
> `IDENTITY,PLAUSIBLE` → `DEGENERATE,NAN_OR_INF` at draw=546 (the same log line as
> `Unloading 66 … 47240`) and stay there for 851/903 unbiased samples, with
> `Screen position out of view frustum` = 17927 vs oracle 0. The `jit_code_hash`
> spin is retired as the mechanism: there is no permanent non-advancement to explain.

Date: 2026-07-27. Lane: HK first-pixel (auto-loop).
Runs: `laneA-combined-decisive-20260726` (iter-1) and `laneA-vscb-full-census-20260727`
(iter-2), a two-run A/A pair on identical admitted artifacts.

## Verdict

**The black frame is not a graphics defect. Hollow Knight's managed scene bootstrap
halts inside the guest Mono runtime at `Performing automatic level start.` — the same
post-scene Mono-JIT pathology already tracked as the project's #1 CPU blocker
(`jit_code_hash` lookup cycle). No scene is ever created, so there is nothing to
render: every presented frame is the cleared backbuffer (0,0,0,255). The first visible
pixel is blocked by the Mono/JIT engine, not by DXMT/Metal/shaders/presentation.**

## The differential proof (working machine vs our runtime, byte-exact log markers)

Working oracle: Windows 11 ARM64 (Parallels) running the SAME x64 Hollow Knight build
through Microsoft's x64→ARM64 translation — i.e. the same class of problem, working
(`WINDOWS-PRISM-ORACLE-BASELINE-20260721.md`: Team Cherry logo +14.8 s, fully rendered
main menu +33.9 s). Its `Player.log` (105 lines) after `Performing automatic level
start.`:

```
Game controller set to None.
Loaded saved language code 'EN'
Making UI menu lean.
Using fallback 1 to find tilemap. Scene Opening_Sequence requires manual fixing.
Opening sequience changing sequences. / skipping.
Levels are ready before cinematics are finished. ...
```

Both our runs (iter-1 and iter-2, independently) after the same line:

| Marker | oracle | iter-1 | iter-2 |
|---|---|---|---|
| `Game controller set to None.` | 1 | 1 | 1 |
| `Loaded saved language code 'EN'` | 1 | **0** | **0** |
| `Making UI menu lean.` | 1 | **0** | **0** |
| `Opening_Sequence` tilemap fallbacks | 3 | **0** | **0** |
| `Opening sequience …` | ≥6 | **0** | **0** |
| `Levels are ready before cinematics…` | 1 | **0** | **0** |
| `Object Pool attached to GlobalPool…` (gameplay prefabs) | 6 | **0** | **0** |
| scene names (`Menu_Title`/`Tutorial_01`/`Town`/`Crossroads`) | present | **0** | **0** |
| `Screen position out of view frustum` | **0** | 19817 | 17927 |

The log streams diverge at exactly one point — immediately after `Game controller set
to None.` — which is also exactly where our producer thread enters the Mono
`jit_code_hash` spin (finite but ~15 min long in both runs; stack-captured 2026-07-26:
`hb_jit_runtime_run → generated ARM64 → hb_jit_helper_exec_two_block_loop →
exec_instr → mem_read`). After the spin, presents resume but the managed sequence
never advances: no scene, no GameCameras, eternal frustum-unprojection failures
(19817× vs 0 on the working machine — the "Couldn't find GameCameras/Game Manager"
lines themselves are benign boot noise, they occur on the oracle too).

## Why the graphics stack cannot be the cause (each layer independently exonerated)

- **Vertex data**: Unity itself writes the black vertex colors `(0,0,0,5/255)` through
  Map/Unmap; IA binding and upload byte-correct (`VERTEX-DATA-BLACK-CAUSE.md`,
  valid run 2026-07-18).
- **Shader translation**: 152/152 real HK DXBC blobs translate clean; rt0 writes and
  discard parity exact (`SHADER-LANE-AIRCONV-OFFLINE-VERDICT-20260726.md`).
- **Stage link / rasterization / interpolation**: causal ladder on the admitted
  artifact `336c76df…` — C2 (fragment input forced magenta) and C3 (VS reg1 forced
  magenta) both produced full-viewport MAGENTA through the real pipeline
  (655360/655360 written pixels = the draw's exact 1024×640 viewport; coverage report
  honest: `coverage_fraction=0.8333` = viewport, not a grid lie).
- **Fragment output path globally**: forcing EVERY PSO's fragment output to opaque
  magenta turned the whole backbuffer magenta (`FRAGMENT-OUTPUT-MAGENTA-AB-RESULT.md`,
  2026-07-17, sealed A/B).
- **Composition graph**: every post-level frame binds the backbuffer RTV,
  last-bound-before-Present1 = backbuffer 210/210
  (`HK-COMPOSITION-GRAPH-OFFLINE-VERDICT-20260726.md`).
- **Presentation transport**: the ordinal-200 readback rides the same
  `macrunner_gpu_readback_schedule` machinery that returned colorful magenta for the
  causal readback in the same run; the drawable texture pointer is logged at schedule
  and completion.
- **Translator arithmetic**: SSE matrix families match x86; MXCSR/FPCR neutral
  (transform-lane audit integrated 2026-07-26).

## The black frame itself — verified, late, A/A

- iter-1: `readback-complete checkpoint=presented-surface tag=0xc8 … pixels=786432
  black=786432 nonblack=0 mean=0,0,0,255` at Present ordinal 200 (of 989), after
  `Performing automatic level start.`
- iter-2: identical result at ordinal 200 (of 899) — **identical pixel hash
  `0xc770038f717d0383`**. A/A pair passes; the black verdict is reproducible, not a
  flake.
- Both runs: zero `UNSUPPORTED`/`MEMORY_FAULT`/`fastfail`/`reject`/`HUP` (both logs,
  case-sensitive).

## Supporting aggregate data (iter-1+iter-2, near-identical at matching ordinals)

- ~45k draws/run sampled by the VS-CB probe (`dxmt-vscb-totals` ×11; e.g. iter-2:
  `draws_sampled=40960 zero=94810 nan_or_inf=3389 degenerate=110683 plausible=65018
  identity=16798`); `drawindexed=32753` by mid-run — the engine submits plenty of
  draws; they are engine-internal batches (splash/UI quads with Unity-written black
  vertex colors), not scene content.
- The ZERO/DEGENERATE-heavy matrix-class mix is time-stable from boot onward and
  consistent with legit 2D sprite/UI transforms; it does not indicate a progressive
  post-level camera collapse. The camera problem is not *degenerate matrices* — it is
  the *absence of the GameCameras scene camera* (managed bootstrap halt).
- Correction on record: an intermediate draft of
  `COMBINED-DECISIVE-RUN-RESULT-20260726.md` claimed "< 4096 draws total" from grepping
  only `launch.stderr`; the PE-side totals live in `launch.stdout`. Retracted and
  amended in that report.

## Run/artifact integrity notes

- Admission per run: preflight contract, prefix-sync probe PASS, real-prefix
  `system32/d3d11.dll=336c76df…97c6`, `winemetal.dll=b4cf672e…`, host
  `aarch64-unix/winemetal.so=f44ad641…` (full-probe build), sealed `final-child.json`
  (126 entries), +2-minute log-growth gate.
- Host-artifact repair this lane made: dist briefly held `cf093248…` (staged for the
  stack-capture runs) which contains **no** present-surface/selector probes; restored
  the full-probe `f44ad641…` from `engine/graphics/build/dxmt-aarch64-hk-present/`
  (backup: `laneA-combined-decisive-20260726/winemetal.so.cf093248.backup`). Dist
  currently holds `f44ad641…`; other lanes should pair expectations accordingly.
- Both runs ended by the 4200 s harness timeout (`exit=124`), prefixes auto-removed,
  no Wine residue; disk ≥ 40 GB throughout.

## What this closes and what it hands off

- **Closed**: the black-frame question. Cause = guest managed bootstrap halt at level
  start (Mono/JIT), not any graphics layer.
- **Handoff**: the fix belongs to the CPU/Mono lane's active `jit_code_hash`
  investigation. The product-facing proof attached here: resolving that spin is what
  unblocks `Loaded saved language code 'EN'` → `Making UI menu lean.` →
  `Opening_Sequence` → the first visible pixel. The working-machine sequence (above)
  is the acceptance checklist for any candidate engine fix; this lane's A/A harness
  (invoker + ordinal-200 presented readback, byte-identical expected black hash
  `0xc770038f717d0383`) is the regression detector: a successful engine fix turns the
  ordinal-200 readback non-black.
