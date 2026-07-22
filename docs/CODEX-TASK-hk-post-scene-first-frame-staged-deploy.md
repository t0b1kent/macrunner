# TASK: HK Post-Scene Frame With Deterministic Staged Deploy

The last attempt is `UNKNOWN` only because an external deploy watcher missed the
prefix-sync-to-child window. Eliminate that race entirely. Do not add another
watcher, hook, analyzer, or polling framework. Do not change product source,
Wine, HyperBridge, DXMT, game state, or translation cache. No build and no retry
loop.

## Preserve Proven Corrections

- Preserve the verified clean child environment: exactly 116/116 accepted names,
  zero drift, zero forbidden names, accepted `SHLVL` and locale values.
- Preserve the detached non-PTY lifecycle. Monitor failure must not send HUP to
  `mr-run` or HK.
- Reuse commit/checkpoint:
  `78e1b3c4381104559ee3d7fa8e3cb23b889f1283`
  and
  `reports/phase4-hollow-knight/checkpoints/20260722-oracle-managed-actuator-105203-VERIFIED_CAPABILITY_NOT_GOLDEN`.
- Fresh allowlisted prefix under `artifacts/_mr-run-aa-*`; serialize all HK/Wine
  activity; ABZU untouched.

## Deterministic Deploy Before Launch

1. Before invoking `mr-run`, make an isolated APFS clone/copy of the checkpoint's
   Present-positive dist into a disposable run-local staged-dist root. Never
   modify the checkpoint or working/production dist.
2. Atomically replace exactly this file in the staged dist:

   `lib/wine/x86_64-windows/mono-profiler-hk_language.dll`

   Source PE:

   `reports/phase4-hollow-knight/gchandle-lifetime-20260721-1629-NOT_GOLDEN/product/mono-profiler-hk_language.dll`

   Required SHA-256:

   `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`

3. Verify the staged tree differs from the checkpoint dist in exactly that one
   regular file. Record baseline/staged tree identities and the one-file diff.
4. Point `MACRUNNER_LANEA_WINE_DIST`, loader paths, and prefix sync at this staged
   dist before `mr-run` starts. Do not perform any post-launch deployment.
5. Let normal prefix sync copy the already-staged PE into system32. Require the
   live prefix file SHA to equal `7c62033f...570b` before accepting any product
   measurement. Also require `final-child.json` and
   `observer-init status=armed allocations=excluded`.

Any staged-tree diff other than the one profiler file, wrong live-prefix SHA, or
missing profiler marker is `INVALID_PREFLIGHT/DEPLOYMENT`; stop without consuming
the single runtime attempt. Do not retry.

## One Runtime

- Execute exactly one fresh run, hard cap 2700 seconds, with the same Oracle
  state, actuator variables, native/DXMT bytes, cache root, and run-contract
  inputs as the accepted capability run.
- Require exact-one lookup, SetLanguage/ConfirmLanguage OK once, retained
  gchandle, all three managers, zero faults/rejects, and
  `Performing automatic level start.`
- At that boundary record cumulative GetBuffer/Present/Present1 counts, keep the
  same process alive for 300 seconds, and capture at +10/+30/+60/+180/+300.
- Report strictly post-boundary GetBuffer, Present, Present1, draw/encoder,
  fault, and pixel deltas. All zero-count operations must return rc=0.

## Verdict

- Post-boundary Present > 0 and any non-black pixel: `PIXEL_PASS`; make a compact
  verified milestone snapshot and one focused report/snapshot commit.
- Post-boundary Present > 0 and all captures BLACK: `POST_SCENE_RENDER_BLACK`;
  make a compact verified NOT_GOLDEN capability snapshot and focused commit.
  Only this result authorizes shader-value work.
- Automatic level start plus zero post-boundary Present for 300 seconds:
  `POST_SCENE_SUBMISSION_STALL`; do not investigate shaders.
- Missing measurements, env drift, HUP, or orchestration failure: `UNKNOWN`; no
  snapshot, commit, or retry.
- New opcode/fault/reject: report the exact first post-manager product wall.

Update:
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-FRAME-RESULT.md`.

Preserve all attempts and the translation cache. Remove only the disposable
prefix and staged-dist clone; verify zero HK/Wine/ABZU residue. Never commit raw
`final-child.json`, secret values, generated dist, cache, or bulk logs.
