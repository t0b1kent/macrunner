# HK combined decisive run — one run, four answers

Date: 2026-07-26. Coordinator spec for the lane that owns the JIT run slot.

Rationale: the run slot is the project bottleneck (~40 min, strictly serial). Three lanes
have each produced a probe for a different candidate cause. All three probes plus the
causal ladder are present in a **single already-built artifact**, so they cost one run
together instead of three runs apart.

## Artifact — verified, not assumed

`engine/graphics/dist/dxmt/x86_64-windows/d3d11.dll`
sha256 `336c76dff17411e76eb1b30e35161e32ba3a5f5d417f93034e66bb5ee96397c6`

Verified by `strings` on that exact file to contain all of:

| Symbol | Meaning |
|---|---|
| `MACRUNNER_HB_CAUSAL_CONTROL` | causal-ladder gate |
| `C0,C2,C3` | the built-in **combined** ladder mode (see `d3d11_shader.cpp:55`) |
| `_mr_c2_fragment_input_magenta` | C2 variant present |
| `_mr_c3_vertex_reg1_magenta` | C3 variant present |
| `MACRUNNER_DXMT_VS_CB_DUMP` | transform probe gate |
| `dxmt-vscb-totals` | transform probe aggregate emitter |

**Before launch the preflight `prefix-sync-probe` MUST print
`d3d11_sha256=336c76df…97c6`.** If it prints `172ddbdb…` the prefix was synced before this
build — re-sync. If the run falls back to the `dxmt-builtin-overlay` copy (`f88868fc…`)
both probes are silently absent and produce zero output. **Zero output = probe not loaded,
NOT "nothing happened".**

Because this changes the admitted `d3d11.dll` from `172ddbd…13f41`, the run's own control
must be taken on **this** artifact. Never pair a control taken on the old DLL with a result
taken on the new one.

## Knobs

```sh
MACRUNNER_HB_CAUSAL_CONTROL=C0,C2,C3   # combined ladder — natively supported, not a hack
MACRUNNER_DXMT_VS_CB_DUMP=1            # transform probe, read-only at draw time
MACRUNNER_DXMT_VS_CB_DUMP_MAX=512
# plus the presented-surface readback + its control (CAUSAL_PRESENT_SURFACE_READBACK /
# CAUSAL_PRESENT_SURFACE_CONTROL, owned by the run lane)
```

## Mandatory gate: sample LATE, not at boot

Every causal-target verdict so far was taken at `Present=4` / `DrawIndexed=3`. Full runs
reach `Present≈857` / `DrawIndexed≈41807`. **Four presents with three draws is Unity's boot
window, which is black on a working machine too.** Gate the readbacks on a late frame —
`Present>=200`, or cumulative `DrawIndexed>=1000`, or after the
`Performing automatic level start.` marker. A black readback at `Present=4` proves nothing
and must not be reported as a finding.

## Decision table — what each outcome means

| Presented surface (late frame) | C2 (fragment input forced magenta) | C3 (VS reg1 forced magenta) | Verdict |
|---|---|---|---|
| non-black | — | — | rendering is fine; the defect is in **presentation/compositing to the window**, not the pipeline |
| black | magenta | — | pixels ARE covered and shaded → the fragment shader's own arithmetic/texture sampling zeroes the colour → **shader translation / texture binding** |
| black | black | magenta | fragments are never covered, but VS output can reach → **rasterisation / interpolation** between the stages |
| black | black | black | nothing reaches the stage at all → back to the **target/composition graph**: draws land in an offscreen target that is never composited |

`dxmt-vscb-totals` runs alongside for free and answers the transform question numerically
(`IDENTITY` / `ZERO` / `NAN_OR_INF` / `DEGENERATE` / `PLAUSIBLE` counts), independently of
which branch above is taken.

## Known limitation — state it in the report, do not paper over it

The ladder variants are keyed to two specific shader hashes in `d3d11_shader.cpp:64-71`:
`ps_bef43b20_f8e9cf38dc7294cc7203c7fb6cd68b27ed713b6a` and
`vs_59c2a0b5_adcd61f38fa5038495d5d87e7f3a67bb8d794d86`. **If those are not the shaders that
draw the actual scene, the ladder speaks about one draw, not about the frame.** Confirm the
hashes are live in this run — and how many draws use them — before generalising any rung's
result to the whole frame.

## Prior results this supersedes

- C1 (fragment final magenta) **PASSED**: injected `rt0=1,0,1,1` reached the target and the
  encoder `rtv` equalled the readback address. The attachment, encoder and readback
  transport are proven honest — do not re-litigate them.
- Translator SSE matrix families **MATCH x86**; MXCSR for HK is `0x1f80` (masks + RNE, no
  FTZ/DAZ) and host `FPCR=0x0`. Degenerate-matrix-by-translator-arithmetic is weakened, but
  not eliminated — only `dxmt-vscb-totals` settles it.
