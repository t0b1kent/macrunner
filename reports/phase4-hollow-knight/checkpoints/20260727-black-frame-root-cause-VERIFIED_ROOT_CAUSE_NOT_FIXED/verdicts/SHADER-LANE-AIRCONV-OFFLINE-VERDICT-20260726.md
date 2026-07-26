# HK shader lane — offline DXBC→AIR verdict: translation REFUTED as black-frame cause

Date: 2026-07-26
Lane: HK-SHADER (offline, no game run, no engine edit)
Verdict: **DXBC→Metal shader translation is REFUTED as the cause of the black render target,
for the complete shader population Hollow Knight ships, with an instrument proven honest
by a known-good constant-colour control.**

## Instrument (proven honest, gate 3)

- Binary: `engine/graphics/build/dxmt-aarch64/src/airconv/darwin/airconv`
  SHA-256 `d2f0f1e79f1034f9efa3b3e086dcb0ab8cb9791ef296c01f33cf7d4d682bb414`,
  built 2026-07-26 18:02 local from a `git status`-clean `engine/dxmt/src/airconv` tree
  (newest source mtime 2026-07-22 < binary mtime; no stale-binary risk).
- Known-good control: hand-assembled DXBC `mov o0.xyzw, l(1.0,0.0,1.0,1.0); ret`
  built by surgical token edit of the real captured HK identity PS (valid
  ISGN/OSGN/SHDR container kept): `control/const_magenta.dxbc`
  SHA-256 `a55c8f1b4bceb1912ef5ab32147168949f5eaf42979424ae5088ac7d645f0346`.
- airconv output (`control/const_magenta.ll`, SHA-256
  `d9b7e7a31ae0dfd783d0875eecc97e58ac44fe1ac43e37afe3ca6b920bbd36f0`):

  ```llvm
  define <{ <4 x float> }> @shader_main(<4 x float> %0) local_unnamed_addr {
  entry:
    ret <{ <4 x float> }> <{ <4 x float> <float 1.000000e+00, float 0.000000e+00, float 1.000000e+00, float 1.000000e+00> }>
  }
  !13 = !{!"air.render_target", i32 0, i32 0, ...}
  ```

  Constant magenta reaches colour attachment 0 with correct metadata. The translator
  propagates constants and maps `SV_Target0`→rt0 exactly.
- Instrument defect found and fixed **in my own patcher**, not airconv: first control
  build encoded the SM5 instruction-length field at bits 16–23 instead of 24–30
  (`0x00080036` vs correct `0x08000036`). `CShaderCodeParser::ParseInstruction`
  advances `m_pCurrentToken = pStart + InstructionLength`
  (`engine/dxmt/libs/DXBCParser/ShaderBinary.cpp:1086`), so a zero length loops
  forever. Robustness note only: DXBCParser has no `len==0` guard; real fxc output
  never emits it. No engine change made.

## Population (gate 1: real shaders, full coverage)

152 unique DXBC blobs, all real Hollow Knight bytecode:

- `artifacts/hk-unity-shader-dxbc/run-20260603-210346/blobs/` — 128 shaders extracted
  from the installed game's `Hollow Knight_Data/resources.assets` (Unity 2020.2.9,
  manifest.tsv with per-blob SHA-256).
- `artifacts/hk-dxbc-corpus/probe-20260603-203534/blobs/` — 24 Unity built-in shaders
  the game links.
- Stage split (parsed from SHDR version tokens): **82 pixel, 46 vertex, 24 compute**.
- Separately: the 5 runtime-captured PS + 1 paired VS from
  `laneA-fragment-translation-try1-155643` (the shaders observed at the real render
  boundary) were re-translated offline and compared against the in-run `.ll` captures.

## Translation results (gate 2)

`corpus-translate-results.jsonl` (SHA-256 `6868f2d520084fd1568454a411d5c0c256837fb668f6172258a0f51b98054843`):

- **152/152 translate, 0 failures, 0 hangs** with the current binary (60 s per-file bound).
- All `.ll` outputs in `corpus-ll/` (154 files incl. spot checks), per-file SHA-256 in
  the results JSONL.

Semantic scan of all generated AIR (`corpus-air-analysis.json`, SHA-256
`97e3c5d4824f8c9866f1b7d52fcb5a355b64c8fca6aee528f163def761319b08`), cross-checked
against an independent DXBC token-stream scan (`corpus-dxbc-scan.jsonl`, SHA-256
`346c02c0f6c987f90dc97a3a3f4b19ef56b48fad58092499524d58c6da4e1c69`):

- **Zero-write:** no PS lacks a `ret`; all 82 PS carry `"air.render_target", i32 0`
  metadata. No constant-all-zero return anywhere — except exactly **2/82 PS that are
  constant-black BY DESIGN**: their DXBC is `mov o0, l(0,0,0,0)` and AIR returns
  `zeroinitializer` (faithful; Unity utility passes
  `0086_..._2964bc56...`, `0111_..._7598abb8...`).
- **Discard:** 28/82 PS contain DXBC `discard`; all 28 produce exactly one conditional
  `air.discard_fragment` site; **zero** spurious discards in the other 54. Spot
  differential on `ps_1b003f56` (gate 4): DXBC
  `add r0.x, r0.w, -v3.x; lt r0.x, r0.x, l(0); discard_nz r0.x` ↔ AIR
  `%25 = fsub float %24, %7; %26 = fcmp olt float %25, 0.0; br i1 %26, label %discard_fulfilled, label %discard_otherwise`
  — condition is data-dependent (tex alpha vs cutout threshold), not constant-true,
  and the non-discard path falls through to the colour write. The apparent add→fsub
  flip is the DXBC NEG operand modifier, visible as explicit `fneg`+`fadd` in the
  pre-opt AIR (`ps_1b003f56_....preopt.ll` lines 89–101).
- **Bindings:** zero `undef`/`null`/`poison` texture or sampler arguments in any
  `air.sample_*` call across all 152 modules. Texture and sampler pointers are loaded
  from the argument-buffer struct (e.g. `getelementptr %argument_buffer_struct` →
  `load %struct._texture_2d_t addrspace(1)*` → `air.sample_texture_2d`), i.e. bindings
  resolve to runtime-supplied objects, not baked defaults. `select …, null` sites in
  Unity built-ins are constant-buffer pointer selection, not texture binds.
  Sample-count parity DXBC↔AIR is exact for 80/82 PS (the remaining 2 use
  `customdata` blocks my own scanner skips; airconv parses them fine — scanner
  limitation, not translator).
- **Runtime-capture parity:** offline vs in-run `.ll` for the 5 captured PS: identical
  opcode histograms except the UNORM output epilogue (one `fadd`/`fsub` of 1/127500),
  which the CLI omits because it runs without PSO context
  (`unorm_output_reg_mask`, `dxbc_signature.cpp:588`). That bias is 7.8e-6 and cannot
  turn non-black into black.

## Boundary of this verdict

- [HYPOTHESIS] The population covers `resources.assets` + Unity built-ins + all
  runtime captures. If HK compiles additional variants from level-specific bundles at
  runtime, they are not in this set; however the 5 shaders actually observed drawing
  at the render boundary are covered and match.
- Offline CLI omits PSO-context epilogues (UNORM bias inspected above; dual-source
  blending and sample_mask are runtime state, audited separately in
  `SHADER-INPUTS-CAUSE.md` — no mask bit-order bug found there).

## Consequence for the black-frame investigation

Every half of the lane's hypothesis is refuted with byte-level evidence:

1. "airconv writes zero to rt0" — false for all 82 PS (constants, inputs and sampled
   values all reach `ret` → rt0; only the 2 by-design black shaders return zero).
2. "airconv emits a dominating/constant discard" — false; discard parity is exact and
   every discard condition is data-dependent.
3. "airconv drops texture/sampler bindings to a black default" — false; all samples
   read resolved argument-buffer pointers, and runtime binding content was separately
   proven nonblack (`SHADER-INPUTS-CAUSE.md`, 84/84 bound, 98–100 % nonblack texels).

Combined with `VS-TRANSLATION-FIX-VERIFY.md` (the exact draw's **vertex buffer itself
contains RGB=0,0,0** and DXBC/AIR copy it faithfully) and
`FRAGMENT-TRANSLATION-CAUSE.md`, the black value enters the pipeline as **data**
(game-supplied vertex colours / upstream draw content), not as a translation artifact.
The next causal boundary is upstream of shader translation (CPU-side value production
or earlier draw state) — outside this lane.

## Artifacts

All under `reports/phase4-hollow-knight/shader-lane-offline-20260726/`:
`make_magenta_control.py` (SHA `c56821a6…`), `dxbc_scan.py` (SHA `1cecb5ef…`),
`translate_corpus.py`, `control/`, `real/` (5 re-translated runtime PS),
`corpus-ll/` (152 outputs), `corpus-dxbc-scan.jsonl`, `corpus-translate-results.jsonl`,
`corpus-air-analysis.json`.
