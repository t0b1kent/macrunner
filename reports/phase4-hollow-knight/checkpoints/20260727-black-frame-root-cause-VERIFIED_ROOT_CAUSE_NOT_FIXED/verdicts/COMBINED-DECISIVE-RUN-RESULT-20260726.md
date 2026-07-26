# Combined decisive run — RESULT (C0,C2,C3 + VS CB dump + late presented readback)

Date: 2026-07-26/27. Run dir: `reports/phase4-hollow-knight/laneA-combined-decisive-20260726/`.
Spec executed: `HK-COMBINED-DECISIVE-RUN-SPEC.md`. Ended by timeout `exit=124` after 4200 s.

## Admission chain (all verified before/after launch, byte-exact)

- Dist artifact: `engine/graphics/dist/dxmt/x86_64-windows/d3d11.dll` sha256
  `336c76dff17411e76eb1b30e35161e32ba3a5f5d417f93034e66bb5ee96397c6`; `strings` re-verified:
  `C0,C2,C3`, `_mr_c2_fragment_input_magenta`, `_mr_c3_vertex_reg1_magenta`,
  `MACRUNNER_DXMT_VS_CB_DUMP`, `dxmt-vscb-totals` all present.
- **Host-artifact defect caught pre-launch**: dist `aarch64-unix/winemetal.so` was
  `cf093248…` (user-staged for the stack-capture runs) which strings-shows **zero**
  present-surface / causal-target-readback / ordinal-selector probes — the spec's
  "zero output = probe not loaded" trap. Replaced atomically with the full-probe build
  `f44ad6415a954539ca94a950300f09b98206f2bc5dc517515b85ed98b311b5a1`
  (from `engine/graphics/build/dxmt-aarch64-hk-present/`, the exact host artifact the
  late-ordinal instrument run already proved at runtime). `cf093248` backed up at
  `laneA-combined-decisive-20260726/winemetal.so.cf093248.backup`.
- Preflight: `prefix-sync-probe-result.json` = PASS (system32 d3d11=336c76df…,
  winemetal.dll=b4cf672e…, host=f44ad641…). Real prefix system32 re-hashed after
  launch: identical.
- `final-child.json`: 126 entries; exact values `MACRUNNER_HB_CAUSAL_CONTROL=C0,C2,C3`,
  `MACRUNNER_DXMT_VS_CB_DUMP=1`, `…_MAX=512`, `MACRUNNER_HB_PRESENT_SURFACE_READBACK=1`,
  `…_ORDINAL=200`, `MACRUNNER_HB_CAUSAL_PRESENT_SURFACE_READBACK=1`,
  `MACRUNNER_HB_CAUSAL_PRESENT_SURFACE_CONTROL=C1`, `…_SIDECHANNEL_GRID=1`,
  `…_TARGET_READBACK=1`, `MACRUNNER_HB_BACKEND=jit`, cold run. No forcing control on any
  measurement path (direct present control and grid control absent by construction).
- +2-minute log gate: `launch.stdout` 6170 B and `launch.stderr` 357289 B, both grew.
- Run hygiene: child PID 70832, detached own session, stdin DEVNULL; zero `UNSUPPORTED`,
  `MEMORY_FAULT`, `fastfail`, `reject`, `HUP` in either log (case-sensitive, both grepped).

## Results

### Causal ladder rungs (boot window, target_draw=2, one-shot — stated limitation)

- C0 real fragment output: `class=BLACK written=655360 black=655360 magenta=0 other=0`
  grid `coverage=partial coverage_fraction=0.833333333 nonzero=0` — honest format; the
  131072 unwritten coordinates are exactly outside the draw's viewport
  (`viewport=0,64,1024,640`; 1024×640=655360). **Every pixel the draw covers is
  verdict-covered.**
- C2 fragment-input forced magenta: `class=MAGENTA written=655360 black=0 magenta=655360`.
- C3 VS reg1 forced magenta: `class=MAGENTA written=655360 magenta=655360`.
- All `draw-select`/`sidechannel-*`/`encoder-end` phases `result=ok`,
  `expected_mask=0x0d claimed_mask=0x0d`; signature checksum matched.
- Causal present-surface transport control: `phase=arm result=ok` then
  `phase=complete result=not-presented` — the causal draw's command buffer presents no
  drawable, so the C1 injection control did not fire on this path. Presented-surface
  transport is instead proven same-run by the causal-C3 GPU readback through the
  **identical** `macrunner_gpu_readback_schedule` machinery:
  `readback-complete checkpoint=causal-C3 … colorful=655360 max=255,0,255,255`.

### LATE presented surface (the decisive column)

`Performing automatic level start.` at line 208 (Present #48). The post-scene JIT spin
(Producer thread, ~200 % CPU, Present frozen at 48) **resolved after ~15 minutes** —
it is a long finite stall, not a terminal boundary; presents then flowed 48 → 989.

- `phase=selector result=ok ordinal=200`
- `phase=schedule result=ok ordinal=200 command_buffer=0x91cd84700 drawable=0x91b0adf80
  texture=0x98cdd4f00`
- `readback-complete checkpoint=presented-surface tag=0xc8 … pixels=786432
  black=786432 nonblack=0 colorful=0 min=0,0,0,255 max=0,0,0,255 mean=0,0,0,255`

**The real, late, post-level presented drawable is uniformly black.** This is not the
boot window: ordinal 200 of 989 presents, after automatic level start.

### Decision-table reading

Presented black (late) + C2 magenta + C3 magenta formally selects the row
"pixels ARE covered and shaded → fragment shader's own arithmetic/texture sampling
zeroes the colour". Shader translation is already refuted offline. **But two aggregate
signals in this same run narrow it further:**

1. **`dxmt-vscb-totals` DID emit — 11 times — in `launch.stdout`** (the PE-side
   d3d11.dll probe lines, including swaptrace/drawtrace/vscb detail+totals, land in
   `launch.stdout`, not `launch.stderr`; only the host winemetal lines go to
   `launch.stderr`). The first version of this report grepped only `launch.stderr`
   and wrongly concluded "< 4096 draws in the whole run". **RETRACTED.** Actual
   cumulative totals, stable proportions across the run:

   | draws_sampled | buffers | zero | nan_or_inf | degenerate | plausible | identity |
   |---|---|---|---|---|---|---|
   | 4096 | 16376 | 9524 | 498 | 11662 | 6466 | 1622 |
   | 45056 | 180216 | 104292 | 3710 | 121690 | 71520 | 18488 |

   Plus an unlimited drawtrace aggregate: `dxmt-drawtrace-totals: drawindexed=32753
   clear=1901 omset=12114 present=680 present1=680` (mid-run snapshot). So post-level
   the game **does** submit draws in quantity; "nothing is drawn" is refuted.
   The ZERO/DEGENERATE-heavy class mix is roughly time-stable and, for a 2D game
   (zero-Z-scale sprite models, partially-written CB ranges), a large legit fraction
   cannot be excluded from totals alone — per-slot per-draw post-level data (iter-2)
   is required to separate legit 2D degeneracy from a broken camera.
2. `Screen position out of view frustum` appears **19817 times**, from boot (line 104)
   to run end, where it reports the screen **center** (512, 383) failing against the
   gameplay camera rect `0 64 1024 640` — Unity's unprojection guard (`|w|<=1e-7`,
   incl. NaN; transform-lane iter-3) rejecting every frame's center ray. It fires at
   boot too (with the splash camera), so its abnormality is not established without
   an oracle comparison.

VS CB boot detail census (509 buffer records, draws 1–129, classifier is
scale-invariant with host selftest): DEGENERATE 581, ZERO 288, NAN_OR_INF 99,
PLAUSIBLE 156, IDENTITY 21, UNKNOWN 705. NAN in a shipping Unity draw is never
legitimate; but this is boot-window data.

## Verdict

- **VERIFIED**: late presented frame is uniformly black (ordinal 200, honest transport).
- **VERIFIED**: rasterization/interpolation/output path works when fed a constant —
  C2 and C3 both produced full-viewport magenta through the real pipeline.
- **VERIFIED**: post-level draw submission is NOT low — 45056 vscb-sampled draws and
  `drawindexed=32753` by mid-run; the earlier "< 4096 draws" claim (v1 of this report)
  is retracted (grep was aimed at the wrong stream).
- **OPEN — two surviving mechanism families**, now that draws are submitted, the
  pipeline demonstrably works (C2/C3 magenta), shaders translate cleanly and the
  composition graph is structurally sound, yet the late presented frame is black:
  (a) **GPU-side transform**: draws execute but vertices land off-screen / zero-area
  (degenerate MVP) → nothing rasterizes → black; (b) **fragment shading**: draws
  rasterize but the PS outputs black (texture content/binding or arithmetic).
  The vscb totals mix (ZERO 33% / DEGENERATE 38% / PLAUSIBLE 22%) is time-stable and
  cannot by itself separate legit 2D degeneracy from a broken camera; per-slot
  per-draw post-level classes (iter-2) discriminate.
- **[HYPOTHESIS]** the repeating end-of-run frustum warning (screen center fails
  unprojection against the gameplay camera rect) hints at (a), but it also fires at
  boot, so it is corroboration, not proof.
- The ladder's one-draw/boot-window limitation (spec §"Known limitation") stands: rung
  verdicts describe `ps_bef43b20`/`vs_59c2a0b5` at target_draw=2 only.

## Next question (one run, one variable)

Are the post-level camera/VS matrices degenerate **per-slot per-draw** (separating
legit 2D sprite-model degeneracy from a broken view/projection)? Same run contract,
single change:
`MACRUNNER_DXMT_VS_CB_DUMP_MAX=512 → 1000000` (env-driven, no rebuild) — per-draw
detail lines for **every** draw of the run give (a) an exact post-level draw census and
(b) per-draw matrix classes post-level. Everything else byte-identical (ladder, late
ordinal-200 readback = A/A re-check of the black verdict).
