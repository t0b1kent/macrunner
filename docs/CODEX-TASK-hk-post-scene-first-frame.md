# TASK: HK First Post-Scene Frame

Run one direct bounded product runtime. Do not add analyzers, probes, source
changes, builds, retries, input, focus changes, activation changes, field forcing,
scene forcing, or shader overrides.

## Why This Run

The prior run proved the language sequence, level1 manager creation, and
`Performing automatic level start`, but it did not measure a post-scene frame:

- last logged `Present/Present1`: run.log line 1981;
- GameManager/UIManager/GameCameras creation: lines 1987/1992/1993;
- `Performing automatic level start`: line 1995;
- timeout: line 2000.

Therefore preserve the previous capability verdict, but explicitly classify
post-scene rendering as unmeasured. Do not claim the shader boundary until this
run observes at least one Present after automatic level start.

## Exact Baseline

- Start from commit `78e1b3c4381104559ee3d7fa8e3cb23b889f1283` and checkpoint:
  `reports/phase4-hollow-knight/checkpoints/20260722-oracle-managed-actuator-105203-VERIFIED_CAPABILITY_NOT_GOLDEN`
- Reproduce the exact successful Windows-oracle + no-allocation actuator setup
  documented by:
  `reports/phase4-hollow-knight/PIXEL-FIRST-ORACLE-MANAGED-ACTUATOR-RESULT.md`
- Use the same Present-positive native/DXMT bytes, Oracle state, effective child
  environment, actuator PE SHA
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`,
  system32 deployment order, cache root, and run-contract inputs.
- Fresh allowlisted prefix under `artifacts/_mr-run-aa-*`; one HK/Wine runtime on
  the host; ABZU untouched.
- Require `observer-init status=armed allocations=excluded`, exact-one lookup,
  successful SetLanguage/ConfirmLanguage, retained gchandle, all three managers,
  and zero faults/rejects. Any identity drift stops as `UNKNOWN`.

## Runtime Gate

- Exactly one run; hard timeout 2700 seconds; no retry.
- Continuously count Present and Present1, but do not emit per-frame logs.
- When the exact line `Performing automatic level start.` appears, atomically
  record boundary time and cumulative Present/Present1 counts.
- Keep the same process alive for at least 300 seconds after that boundary.
- Capture the HK window at boundary +10s, +30s, +60s, +180s, and +300s.
- For every capture, re-resolve the HK window and record pixel counts and SHA.
- Report the delta of GetBuffer, Present, Present1, draw/encoder activity, and
  faults strictly after the automatic-level-start boundary.

## Verdict

- Post-boundary Present > 0 and any non-black pixel: `PIXEL_PASS`. Create a
  compact verified milestone snapshot immediately and make one focused commit
  containing only the report/snapshot; exclude caches and bulk logs.
- Post-boundary Present > 0 and all required captures BLACK:
  `POST_SCENE_RENDER_BLACK`. This alone authorizes the next shader-value task.
  Create a compact verified capability NOT_GOLDEN snapshot and focused commit.
- Automatic level start reached but post-boundary Present = 0:
  `POST_SCENE_SUBMISSION_STALL`. Do not investigate shaders; report the first
  native/Unity render-loop boundary after manager creation.
- Automatic level start or required measurements missing: `UNKNOWN`.
- Any opcode/fault/reject: report the exact first post-manager wall and stop.

Write:
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-FRAME-RESULT.md`.

Preserve evidence and translation cache. Remove only the scoped disposable
prefix and verify no HK/Wine/ABZU residue. Do not alter the existing capability
checkpoint or create GOLDEN without a visible pixel.
