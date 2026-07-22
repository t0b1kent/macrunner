# TASK: HK guest-loop capture after hash-adjudicator retraction

Use `gpt-5.6-sol` at `xhigh`. HK worktree only. This is the same direct
guest-loop capture task. Do not create another harness, preflight version, or
instrumentation family.

## Mandatory retraction

The latest attempt
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-env-clean-20260722-1900`
was stopped by a false local adjudication, not by environment drift.

Independent byte-level comparison proves:

- its live child `WINEDLLPATH` and the accepted 116-name source-template value
  are byte-for-byte equal;
- both are 900 UTF-8 bytes;
- SHA-256 of the raw value bytes, with no newline or normalization, is
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`;
- `6ae1e2285c346316c822b24c8c45afb2cdc1e76ad2a519a0b5b53dd0bc0449d1`
  is instead SHA-256 of the five entries joined by newline plus a final newline.
  It is a different serialization of the same value and MUST NOT be used for
  the raw-value contract;
- `run-contract.json` was `READY` with `blockers=[]`;
- its save snapshot manifest was `PRESENT`, five files, 11121 bytes, inventory
  SHA-256 `442d9b2310ccc6b88ab24a554a4b70c9747607f52469f275a0bbc2c88dcac919`;
- `MACRUNNER_RUN_CONTRACT_SAVE_PATH` is absent from all 114 retained
  `final-child.json` files. Its absence is not drift and is not a child-env
  requirement. The authoritative save gate is `run-contract.json`.

Append an auditable correction to
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-CAPTURE-CORRECTED-RESULT.md`:
retract `UNKNOWN_IDENTITY_DRIFT`; classify that stopped attempt as
`INVALID_ADJUDICATOR / PRODUCT_NOT_MEASURED / NOT_GOLDEN`. Preserve the original
text and evidence; do not erase history.

## Phase 0: adjudicator correction, no game

Reuse the clean launcher and exact environment construction already proven:

```python
env = load_child_environment()
os.execve(argv[0], argv, env)
```

The canonical live check MUST be exactly equivalent to:

```python
actual = live_entries["WINEDLLPATH"]
baseline = baseline_entries["WINEDLLPATH"]
assert actual == baseline
assert len(actual.encode("utf-8")) == 900
assert hashlib.sha256(actual.encode("utf-8")).hexdigest() == \
    "cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432"
assert len(actual.split(":")) == 5
assert len(set(actual.split(":"))) == 5
```

No newline, delimiter conversion, sorting, JSON encoding, shell echo, or path
normalization is allowed before hashing.

Run this corrected adjudicator against the already preserved 1900
`final-child.json` and the accepted baseline
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-map-20260722-162042/final-child.json`.
It MUST produce PASS before a game is authorized. Also require:

- environment names exactly 116/116;
- sorted-name hash
  `9793680dfb7541b522811065ad2f1a6693cb0b6d64147366288bfbe6014ef532`;
- all prior 13 inherited names absent;
- current 1900 `run-contract.json`: `READY`, blockers zero, save/config/data
  manifests `PRESENT`.

If this static replay does not PASS, stop. Do not change product bytes,
environment values, runner, or save state to satisfy the checker.

## Phase 1: one fresh product runtime

The 1900 run was terminated before the marker solely by the false checker, so
authorize exactly one fresh runtime continuation. Reuse unchanged:

- the env-clean launch construction and accepted 116-name baseline;
- the same staged dist, C0 DXMT overlay, translation cache, save/config state,
  managed actuator, detached non-PTY lifecycle, and 2400-second timeout;
- product hashes:
  - `ntdll.so` `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`
  - ARM64X `ntdll.dll` `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`
  - x86_64 `ntdll.dll` `b08516ba28c2d0d884b22673301bfc6eb3b5b48ee7458cd1248303289e431b78`
  - `winemac.so` `02b4d94ecb5827ea331decccb846f1bf6ff97df632ea28f6b40951794f8c9604`
  - actuator `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.

After live `final-child.json`, apply only the canonical raw-byte check above,
the exact 116-name check, and actual `run-contract.json` READY/blockers/manifests.
Do not require a child save-path variable. If these pass, continue; do not stop
on the noncanonical `6ae1...` digest.

## Phase 2: exact post-scene capture

Wait passively for the exact `Performing automatic level start` marker. Then:

1. Take one passive `sample`; select the native thread whose stack contains both
   `hb_jit_helper_exec_two_block_loop` and `mem_read`.
2. Reuse without edits the accepted corrected LLDB scripts from the 1812
   evidence:
   - `hk_lldb_capture.py` SHA-256
     `c2c0bab9af56885602ed98e227d409902f1f3d9ce41a5e98c3286145ce3192af`
   - `lldb_capture_supervisor.py` SHA-256
     `0bea31fe7a8e2b72a5c089d78699f2591bcf59c0dae73b81db968d0bda523154`.
3. Require that exact TID, use the one-shot helper breakpoint restricted to it,
   pass expected handled SIGSEGV/SIGBUS/SIGILL through, and continue for at most
   30 seconds. Wrong stop, missing TID, timeout, or unhandled crash is `UNKNOWN`.
4. On the exact hit capture once: native TID, `x0/x1/x2`, `ctx->pc`, both block
   headers, guest RIPs, instruction counts, and exact IR bytes. Detach cleanly.
5. Map the captured state to the exact guest instruction/two-block loop. Never
   reuse old run-local host PC `0x11d397e38` as guest identity.

No `LOOP_TOP`, high-rate trace, input, focus/activation, scene forcing, shader
override, cache reset, ABZU runtime, or unrelated probe.

## Phase 3: conditional narrow repair

- If the valid capture proves one decoder/execution-family defect, implement the
  smallest family fix and exact instruction + containing corridor tests. Run
  focused and existing core floors, then one clean-env validation runtime.
- Validation must reach managers/scene/automatic-level marker and measure
  post-marker `GetBuffer/Present/Present1/Draw/encoder` plus passive pixels.
- If interpreter and JIT agree, do not patch. Identify the stable guest loop or
  missing state writer and stop; do not stack another hypothesis.
- If capture is missed, stop without retry or speculative source work.

## Snapshot and commit

- Restored post-scene submission: compact checksum-verified
  `VERIFIED_CAPABILITY_NOT_GOLDEN` snapshot, excluding caches, then commit only
  the narrow source/tests and checkpoint metadata.
- Verified non-black game pixel: immediate verified GOLDEN snapshot and narrow
  commit.
- `UNKNOWN`, orchestration failure, or static-only finding: no product snapshot
  and no source commit.
- Preserve every prior run and do not commit broad dirty diffs.

Update the same result report with the retraction, canonical hash algorithm,
fresh runtime identity, LLDB evidence, guest mapping, conditional repair result,
and cleanup. Remove only owned prefix/staged residue, preserve translation cache,
report collisions and disk headroom, then return control.
