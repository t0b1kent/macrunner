# TASK: HK Oracle + Managed Actuator Direct Pixel Run

Run one direct bounded Hollow Knight product test. Do not build analyzers,
admission frameworks, new probes, or new source code. No rebuild and no retry.

## Fixed Baseline

- Reuse the exact Present-positive checkpoint:
  `reports/phase4-hollow-knight/checkpoints/20260722-present-capability-02b4-VERIFIED_NOT_GOLDEN`
- Reuse the verified Windows-oracle state/prefix preparation from the completed
  run documented in:
  `reports/phase4-hollow-knight/PIXEL-FIRST-WINDOWS-ORACLE-RUNTIME-RESULT.md`
- Preserve its effective child environment byte-for-byte except for the actuator
  bundle listed below. Keep C0, all graphics probes off, and the same cache,
  DXMT, ntdll, winemac, save/config/data, and run-contract inputs.
- Use a fresh allowlisted fixed prefix under `artifacts/_mr-run-aa-*`.
- Serialize against all Wine/HK activity. Do not touch ABZU.

## One Intervention: Sealed No-Allocation Actuator

Use exactly this PE:

`reports/phase4-hollow-knight/gchandle-lifetime-20260721-1629-NOT_GOLDEN/product/mono-profiler-hk_language.dll`

Required SHA-256:

`7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`

After prefix sync, but before game exec/Mono init, atomically replace:

`<fresh-prefix>/drive_c/windows/system32/mono-profiler-hk_language.dll`

Verify regular-file type, size, and exact SHA after replacement. Perform no
later prefix/dist sync. Add only these actuator environment changes:

```text
MONO_ENV_OPTIONS=--profile=hk_language
MACRUNNER_HB_LANGUAGE_FLOW_OBSERVER=1
MACRUNNER_HB_LANGUAGE_ONESHOT_SEQUENCE=1
WINEDLLOVERRIDES=mono-profiler-hk_language=n;d3d9=n,b;d3d11,dxgi,d3d10core,winemetal=n,b
```

Allocation callbacks/profiling are forbidden. Before accepting the run, require:

```text
observer-init status=armed allocations=excluded
```

If the PE identity or this marker is wrong/missing, stop `UNKNOWN`; do not retry.

## Runtime

- Exactly one run, timeout 1800 seconds.
- No input, focus change, activation, field forcing, scene forcing, or semantic
  graphics override.
- Capture passively at first Present, immediately before the managed sequence,
  then +10s, +60s, and +240s after both calls return.
- Record exactly-once lookup, thread identity, `SetLanguage("EN")`,
  `ConfirmLanguage()`, retained gchandle state, GameManager/UIManager/GameCameras,
  automatic level start, Menu_Title/Opening_Sequence/scene loads, swapchain,
  GetBuffer, Present/Present1, readback/pixel counts, and all faults/rejects.

## Verdict

- Calls OK + scene/managers + Present + non-black pixel: `PIXEL_PASS`. Create a
  compact verified milestone snapshot immediately; exclude caches and run-log
  bulk. Commit only focused new report/manifest files, never the broad dirty tree.
- Calls OK + scene/managers + Present + BLACK: determined
  `SCENE_CAPABILITY_PASS_RENDER_BLACK`. Create a compact verified NOT_GOLDEN
  capability snapshot; this returns the next task directly to shader value
  production.
- Calls OK but scene/managers absent: determined managed bootstrap blocker.
- Any new fault/opcode: report the exact first post-confirm wall.
- Missing actuator/deploy/measurement: `UNKNOWN`.

Write `reports/phase4-hollow-knight/PIXEL-FIRST-ORACLE-MANAGED-ACTUATOR-RESULT.md`.
Preserve evidence and translation cache; remove only the scoped disposable prefix
and verify no HK/Wine residue. No other source changes or commits.
