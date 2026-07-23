# TASK: HK post-level managed trace final-child env correction

## Purpose

The prior runtime correction launched successfully and reached a valid
`final-child.json`, but its comparison incorrectly treated four deterministic
runner transformations as uncontrolled drift:

- `DYLD_LIBRARY_PATH`
- `DYLD_FALLBACK_LIBRARY_PATH`
- `SHLVL`
- `_`

The exact formulas below reproduce the actual final child from
`laneA-post-level-managed-milestone-trace-runtime-correction-20260723-145255`.
This is a doc-scoped comparator correction, not a shared verifier change and
not a value-agnostic exception.

## Frozen inputs

Reuse without source edits or rebuild:

- sealed observer PE SHA-256
  `7d403703b66e5d3d5f2e1d9207b1d77e43a5502db9b34eaeda4117cc097db807`;
- completed static/build evidence from
  `laneA-post-level-managed-milestone-trace-20260723-142025`;
- accepted launcher source from
  `laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104`;
- mode-normalized clone policy from
  `docs/CODEX-TASK-hk-mono-jit-hash-cycle-mode-normalized-clone.md`.

Do not edit `tools/run_contract_final_child_capture.py`, any global verifier,
`tools/hk_language_observer/hk_language_observer.c`, `scripts/mr-run.sh`, the
working dist, historical evidence, or product code.

## Exact final-child model

Create an owned copy of the accepted foreground launcher. Allowed launch
semantic edits are only `RUN_DIR`, `PREFIX`, `STAGED_DIST`, and the declared
trace pair:

```python
env["MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE"] = "1"
```

Do not export any parent-shell variable. Build the expected final child map in
that owned launcher (or an adjacent owned no-write comparator) as follows:

```python
parent = load_child_environment()
parent["MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE"] = "1"

expected = dict(parent)
expected["WINEDLLPATH"] = f"{DXMT_ROOT}:{parent['WINEDLLPATH']}"
homebrew = parent["MACRUNNER_HOMEBREW_LIBRARY_PATHS"]
wine_unix = str(STAGED_DIST / "lib/wine/aarch64-unix")
expected["DYLD_LIBRARY_PATH"] = f"{wine_unix}:{homebrew}"
expected["DYLD_FALLBACK_LIBRARY_PATH"] = (
    f"{wine_unix}:{homebrew}:/usr/local/lib:/usr/lib"
)
assert parent["SHLVL"] == "5"
expected["SHLVL"] = "6"
expected["_"] = str(STAGED_DIST / "bin/wine")
```

This is strict normalization by known runner semantics, not a wildcard or a
path-equivalence rule:

1. Final map has exactly 117 names.
2. The only baseline name delta is exactly
   `MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE=1`.
3. `WINEDLLPATH` must equal the formula and SHA-256
   `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`.
4. `DYLD_*` must equal the formulas byte-for-byte, have exactly one leading
   current `wine_unix` entry, and contain no stale staged-dist entry.
5. `SHLVL` must be exactly `5 -> 6`; `_` must equal the owned current
   `STAGED_DIST/bin/wine` path exactly.
6. Every other name and value, including `PWD`, `TMPDIR`, and all
   reproducibility-critical values, must equal the normalized parent map
   byte-for-byte. No `ENV_VALUE_AGNOSTIC` list is permitted.

## Offline proof before runtime

Before Wine exists, prove the model against the preserved invalid final child:

`laneA-post-level-managed-milestone-trace-runtime-correction-20260723-145255/final-child.json`.

The formula must reproduce all 117 names and values exactly. Also prove that
each of these mutations is rejected by the owned comparator:

1. one changed `DYLD_LIBRARY_PATH` suffix;
2. a stale staged-dist entry in either `DYLD_*` value;
3. a wrong `_` path;
4. one undeclared extra name.

The comparator is task-local evidence only. Do not commit or install it as a
shared verifier policy.

Recreate the disposable mode-normalized clone and require strict `4732/4732`.
Any failure is `TRACE_INVALID_FINAL_ENV_MODEL`; do not launch Wine or retry.

## One corrected runtime

1. Atomically install the frozen PE into the owned per-run prefix `system32`
   before Mono initialization and verify its SHA-256.
2. Launch the owned foreground launcher once, without `&`, `nohup`, `setsid`,
   `disown`, detached/PTY wrappers, or parent-shell exports.
3. Parse live `final-child.json` with the strict model above. A mismatch is
   `TRACE_INVALID_FINAL_ENV_MODEL`; do not infer product behavior from it.
4. Require `run-contract=READY` and the exact
   `post-level-milestone-trace=armed` startup marker.
5. On exact `Performing automatic level start.`, preserve the process for at
   least 120 seconds and passively capture the game window at `+10`, `+30`,
   `+60`, and `+120` seconds.
6. Use the original three-method decision table unchanged. No LLDB, input,
   field write, forced activation, Draw observer, shader override, source edit,
   rebuild, or retry is authorized.

## Result

Update the existing post-level milestone report with the final-child model,
offline proof, live comparison, callback sequence, capture hashes, and cleanup.

All three normal managed leaves require a compact
`VERIFIED_CAPABILITY_NOT_GOLDEN` checkpoint. A verified non-black capture
requires an immediate pixel snapshot. Any invalid launch retains compact
`NOT_GOLDEN` evidence only. Do not commit product code or touch ABZU.
