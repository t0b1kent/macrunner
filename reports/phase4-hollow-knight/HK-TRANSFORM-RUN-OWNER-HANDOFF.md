# HK-TRANSFORM → run owner: how to run the VS-CB transform probe

Date: 2026-07-26. One page. Everything the run-owning lane needs to turn the
iter-1 instrument into a verdict on the degenerate-transform hypothesis.

## Preconditions (hard gates, already verified this side)

- Built artifact: `engine/graphics/dist/dxmt/x86_64-windows/d3d11.dll`
  SHA-256 `336c76dff17411e76eb1b30e35161e32ba3a5f5d417f93034e66bb5ee96397c6`
  (contains the probe; strings verified).
- **Before launch, the preflight `prefix-sync-probe` MUST print
  `d3d11_sha256=336c76dff17411e76eb1b30e35161e32ba3a5f5d417f93034e66bb5ee96397c6`.**
  If it prints `172ddbdb...` the prefix was synced before this build — re-sync.
  If the run falls back to the `dxmt-builtin-overlay` copy (`f88868fc...`),
  the probe is silently absent: `MACRUNNER_DXMT_VS_CB_DUMP` will produce
  **zero** output. Absence of `dxmt-vscb-totals` lines = probe not loaded,
  NOT "no draws".

## Run knobs

```sh
MACRUNNER_DXMT_VS_CB_DUMP=1        # master gate (default off)
MACRUNNER_DXMT_VS_CB_DUMP_MAX=512  # per-buffer detail lines (default 256)
```

No other run-contract variable changes. This probe is read-only at draw time;
it does not alter rendering. One title, one run, serial — unchanged.

## What to grep (stderr only — our lines go to `launch.stderr`)

```sh
grep -c '^dxmt-hk-vscb:' launch.stderr        # detail lines (>0 = probe active)
grep '^dxmt-vscb-totals:' launch.stderr | tail -1
```

Proof-of-life FIRST: if `dxmt-hk-vscb:` count is 0 while DrawIndexed counts
are high, the DLL loaded is stale (see Preconditions). Do not interpret.

## Decision table (final `dxmt-vscb-totals` line)

```text
dxmt-vscb-totals: draws_sampled=N buffers=B unreadable=U identity=I zero=Z
  nan_or_inf=Q degenerate=D plausible=P detail_logged=L reason=...
```

- `P >> 0`, `Z=Q=D≈0` → **transform hypothesis REFUTED** for rendered draws;
  boundary moves to material/target/presenter lanes.
- `D` or `Q` or `Z` significantly > 0 → hypothesis **CONFIRMED**; the capped
  `dxmt-hk-vscb:` detail lines identify slot, buffer identity, and the raw
  floats of the offending matrices (first `L` buffers only — if more
  resolution is needed, re-run with a higher `_MAX`; aggregate counts are
  unaffected).
- `U` comparable to `B` → inconclusive: buffers not CPU-readable; say so
  explicitly (`unreadable` ≠ `plausible`).
- If the process is SIGKILLed, the `dll-process-detach` line is absent —
  use the last `reason=periodic` line; it covers all draws up to the last
  multiple of 4096.

## Interpretation aids

- `IDENTITY` matrices are a flag, not a verdict: UI/ortho passes can be
  legitimately identity. Correlate with the detail lines' slot/bytewidth.
- The matrices dumped are the leading ≤256 bytes of each bound VS CB at
  `FirstConstant<<4`. Unity's per-frame camera CB layout is NOT assumed;
  the classification covers every bound slot precisely so that no
  assumption about "projection lives in b0" is needed.
- Cross-reference with the emission-path analysis
  (`HK-TRANSFORM-ITER3-FRUSTUM-WARNING-EMISSION-PATH.md`): the repeated
  `(0,767)` warning fires when Unity's unprojection guard sees `|w| ≤ 1e-7`
  or NaN. If VS CBs are PLAUSIBLE while the warning repeats, the warning is
  a camera/UI-state artifact (missing UIManager), not evidence of a bad
  render transform.
