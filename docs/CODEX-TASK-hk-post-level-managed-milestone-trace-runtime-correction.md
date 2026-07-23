# TASK: HK post-level managed trace runtime env correction

## Why this correction exists

`CODEX-TASK-hk-post-level-managed-milestone-trace.md` reached a valid
build-only result, then stopped before launcher execution because its required
default-off flag was absent from the immutable 116-name child environment. That
was a task-contract contradiction, not a product result.

The prior attempt did not run Wine or Hollow Knight. This handoff authorizes one
and only one deliberate child-environment delta so the already sealed observer
can be measured. It is not permission to inherit a terminal environment or add
any other flag.

## Frozen inputs

Reuse, without source edits or rebuild:

- the sealed observer PE from
  `reports/phase4-hollow-knight/laneA-post-level-managed-milestone-trace-20260723-142025`
  with SHA-256
  `7d403703b66e5d3d5f2e1d9207b1d77e43a5502db9b34eaeda4117cc097db807`;
- the completed static identity and focused build evidence in that same root;
- the accepted foreground launcher source
  `reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104/launch_hash_retraction_mapping.py`;
- the disposable mode-normalized staged clone policy from
  `docs/CODEX-TASK-hk-mono-jit-hash-cycle-mode-normalized-clone.md`.

Do not edit `tools/hk_language_observer/hk_language_observer.c`, rebuild the
PE, change the working dist, or alter any historical evidence.

## Exact launcher correction

Create an owned copy of the accepted foreground launcher in this run's evidence
directory. Relative to the accepted launcher, permit only these semantic edits:

1. `RUN_DIR` for this run.
2. `PREFIX` for this run.
3. `STAGED_DIST` for this mode-normalized disposable clone.
4. Immediately after `load_child_environment()` has normalized its accepted
   child map, add exactly:

   ```python
   env["MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE"] = "1"
   ```

Do not export this variable from the terminal or use a wrapper. It must be an
explicit entry of the launcher's `os.execve(..., env)` map. Do not add, remove,
or alter any other environment name/value beyond the established owned-path
substitutions and this one pair.

## Pre-launch identity proof

Before calling the launcher, derive the normalized expected map from
`load_child_environment()` and verify all of the following without starting
Wine:

1. The normalized baseline has exactly 116 names.
2. The intended map has exactly 117 names.
3. The only name-set delta is
   `MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE`.
4. Its only allowed value is `1`.
5. Raw `WINEDLLPATH` is unchanged and hashes to
   `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`.
6. All forbidden inherited names remain absent. Do not print secret-bearing
   values.
7. Recreate the mode-normalized clone and require strict `4732/4732` before
   execution.

Any failure here is `TRACE_INVALID_ENV_DELTA`; do not run the game or retry.

## One runtime

1. Atomically deploy the frozen sealed observer PE to the owned per-run prefix
   `system32` before Mono initialization. Verify its SHA-256.
2. Execute the owned foreground launcher once. Do not use `&`, `nohup`,
   `setsid`, `disown`, a detached wrapper, a PTY wrapper, or parent-shell
   exports.
3. Require the live `final-child.json` environment to equal the precomputed
   117-name map exactly. An unexpected name, value, or `WINEDLLPATH` hash is
   `TRACE_INVALID_ENV_DELTA`, not a product observation.
4. Require `run-contract=READY`, the sealed observer SHA, and the explicit
   `post-level-milestone-trace=armed` startup marker before trusting telemetry.
5. Wait for exact `Performing automatic level start.` and preserve the process
   for at least 120 seconds afterward. Capture the game window passively at
   marker `+10`, `+30`, `+60`, and `+120` seconds.
6. Collect exactly the callback evidence and decision table required by the
   original post-level trace task. Do not attach LLDB, inject input, modify
   fields, force activation, add a Draw observer, or make any claim from the
   existing swaptrace's zero `Draw*` count.

No retry is authorized.

## Result and preservation

Update:

- `reports/phase4-hollow-knight/PIXEL-FIRST-POST-LEVEL-MANAGED-MILESTONE-TRACE-RESULT.md`
- an owned compact evidence directory containing the launcher diff, precomputed
  116-to-117 proof, final-child comparison, sealed PE proof, milestone records,
  capture hashes, and cleanup proof.

If all three managed milestones leave normally, create a compact
`VERIFIED_CAPABILITY_NOT_GOLDEN` checkpoint. If a non-black game-window capture
appears, snapshot it immediately. For any invalid launch or incomplete trace,
retain compact `NOT_GOLDEN` evidence only. Do not commit product code or touch
ABZU.
