# TASK: Corrected HK First Post-Scene Frame

The prior attempt is `UNKNOWN` because its run-local monitor exited on a normal
zero count under `pipefail`, then its PTY sent SIGHUP to the live HK child. It
also leaked 13 terminal variables into the child environment. Correct only this
runtime orchestration and execute one fresh run. Do not change product source,
Wine, HyperBridge, DXMT, the actuator, game state, or translation cache. No build
and no retry loop.

## Fixed Product Baseline

- Commit/checkpoint:
  `78e1b3c4381104559ee3d7fa8e3cb23b889f1283`
  and
  `reports/phase4-hollow-knight/checkpoints/20260722-oracle-managed-actuator-105203-VERIFIED_CAPABILITY_NOT_GOLDEN`.
- Reproduce the accepted Oracle + no-allocation actuator run exactly, including
  actuator SHA
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`,
  deployment order, native/DXMT bytes, registry/save state, and cache root.
- Fresh fixed prefix must match `artifacts/_mr-run-aa-*`. Serialize against all
  HK/Wine activity; do not touch ABZU.

## Preflight Correction 1: Exact Environment

Construct the child environment from the accepted 116-entry environment record,
not from the current terminal environment. Start from an empty environment and
apply only the accepted entries plus the already-defined run-identity path
substitutions. Before launch, compare sorted names and require zero unexpected or
missing names. After launch, require the effective `final-child.json` name set to
match the accepted 116 names exactly; only approved identity-path values may
differ.

Explicitly forbid these leaked names:

```text
API_KEY_DEEP API_TOKEN BROWSER_USE_AVAILABLE_BACKENDS CODEX_CI CODEX_THREAD_ID
GH_PAGER GIT_PAGER NODE_REPL_TRUSTED_BROWSER_CLIENT_SHA256S
NODE_REPL_TRUSTED_CODE_PATHS OPENAI_API_KEY PAGER SSH_AUTH_SOCK
```

Do not print, copy, commit, or upload environment values. Normalize `SHLVL` to
the accepted value. Preserve the accepted locale fields rather than inheriting
the terminal's `LC_ALL`.

## Preflight Correction 2: Zero-Safe Detached Monitor

- Replace every `grep | wc` counter with a total counter that returns rc=0 and
  prints `0` when no lines match, for example a single `awk` invocation with an
  `END { print count + 0 }` clause.
- Under `set -euo pipefail`, prove all counters against an empty synthetic log:
  each must return rc=0 and integer 0.
- Run `mr-run` outside the monitor PTY/session, with stdin detached and output
  redirected to run files. A monitor exit or shell `ERR` must not deliver SIGHUP
  to the runtime process group.
- Keep cleanup scoped and explicit after the runtime has ended. Do not add a
  persistent harness or framework.

If either preflight correction fails, stop before Wine and report
`INVALID_PREFLIGHT`; that does not consume the runtime attempt.

## One Runtime

- Exactly one fresh run, hard cap 2700 seconds.
- Require the proven sequence: `allocations=excluded`, exact-one lookup,
  SetLanguage/ConfirmLanguage OK once, retained gchandle, three managers, zero
  faults/rejects, and `Performing automatic level start.`
- At that exact boundary, record cumulative GetBuffer/Present/Present1 counts.
- Keep the same HK process alive for 300 seconds after the boundary.
- Capture the exact HK window at +10/+30/+60/+180/+300 seconds.
- Report strictly post-boundary deltas for GetBuffer, Present, Present1,
  draw/encoder activity, faults, and pixel counts.

## Verdict

- Post-boundary Present > 0 plus any non-black pixel: `PIXEL_PASS`; create a
  compact verified milestone snapshot and one focused report/snapshot commit.
- Post-boundary Present > 0 plus all captures BLACK: `POST_SCENE_RENDER_BLACK`;
  create a compact verified NOT_GOLDEN capability snapshot and focused commit.
  This result authorizes the shader-value task.
- Automatic level start reached but post-boundary Present remains 0 for the full
  300 seconds: `POST_SCENE_SUBMISSION_STALL`; do not investigate shaders.
- Missing boundary/measurements, env drift, monitor interruption, or HUP:
  `UNKNOWN`; no snapshot, commit, or retry.
- A new opcode/fault/reject identifies the exact next product wall.

Update:
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-FRAME-RESULT.md`.

Preserve both attempts and the cache. Remove only the new disposable prefix and
verify zero HK/Wine/ABZU residue. Never commit raw `final-child.json` or secrets.
