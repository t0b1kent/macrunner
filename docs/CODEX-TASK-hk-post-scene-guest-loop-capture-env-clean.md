# TASK: HK post-scene guest-loop capture, clean-environment correction

Use `gpt-5.6-sol` at `xhigh`. HK worktree only. This is a direct continuation
of the corrected LLDB mapping task, not a new investigation or harness project.

## Accepted evidence

- Prior result is orchestration-only `UNKNOWN_IDENTITY_DRIFT / NOT_GOLDEN`:
  `reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-CAPTURE-CORRECTED-RESULT.md`
  (SHA-256 `35b2f9ee1bbdd136d50dba2cf6f7a56530b2f73246ab10a718e0012da6c365ac`).
- Its runtime evidence is
  `reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-corrected-20260722-1812`.
- Corrected LLDB signal-policy dry-run is accepted and MUST NOT be rebuilt or
  rerun. `PHASE0-CORRECTED-PROOF.json` SHA-256 is
  `d639c9b272463d0febab23167c1a26ba41bc482de7ecaabcdb88b280e9c7a828`.
  Reuse the exact sealed scripts:
  - `dry-run/hk_lldb_capture.py` SHA-256
    `c2c0bab9af56885602ed98e227d409902f1f3d9ce41a5e98c3286145ce3192af`
  - `dry-run/lldb_capture_supervisor.py` SHA-256
    `0bea31fe7a8e2b72a5c089d78699f2591bcf59c0dae73b81db968d0bda523154`
- The product bytes and staged tree from that attempt remain the baseline:
  - `ntdll.so` `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`
  - ARM64X `ntdll.dll` `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`
  - x86_64 `ntdll.dll` `b08516ba28c2d0d884b22673301bfc6eb3b5b48ee7458cd1248303289e431b78`
  - `winemac.so` `02b4d94ecb5827ea331decccb846f1bf6ff97df632ea28f6b40951794f8c9604`
  - managed actuator `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`
- Known-good child-environment template is the 116-entry `final-child.json`
  from `reports/phase4-hollow-knight/laneA-post-scene-guest-loop-map-20260722-162042`.

## Exact correction

The failed attempt's run-local launcher contains:

```python
env = os.environ.copy()
env.update(load_child_environment())
```

That inherited 13 Codex/terminal names. For the new run-local launcher, change
only those two lines to:

```python
env = load_child_environment()
```

Keep the existing `os.execve(argv[0], argv, env)`. Do not merge, inherit,
export, or forward the launching terminal's environment at any later layer.
Do not print environment values or secrets.

The 13 names that MUST be absent are:
`API_KEY_DEEP`, `API_TOKEN`, `BROWSER_USE_AVAILABLE_BACKENDS`, `CODEX_CI`,
`CODEX_THREAD_ID`, `GH_PAGER`, `GIT_PAGER`, `LC_ALL`,
`NODE_REPL_TRUSTED_BROWSER_CLIENT_SHA256S`,
`NODE_REPL_TRUSTED_CODE_PATHS`, `OPENAI_API_KEY`, `PAGER`, and
`SSH_AUTH_SOCK`.

## Phase 1: static launch proof, no game

Before the sole runtime, exercise `load_child_environment()` without exec and
write a compact name-only/hash proof. Fail closed unless all are true:

- parent mapping contains exactly 116 names;
- sorted-name SHA-256 (newline-delimited with final newline) is
  `9793680dfb7541b522811065ad2f1a6693cb0b6d64147366288bfbe6014ef532`;
- added names = 0, missing names = 0, all 13 names above absent;
- parent `WINEDLLPATH` has four unique entries and SHA-256
  `ccd4ec106d916d895c9bd9ff14ad4b2abd7614eeef90646f7a0405c457fbabe8`;
- the existing `mr-run.sh` prepend deterministically yields five unique child
  entries and SHA-256
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`;
- forbidden shader/high-rate trace variables are absent;
- product/staged bytes above still match and no HK/Wine/ABZU process exists.

This proof is a check of one changed line, not authorization to build another
preflight framework. If it fails, stop and report the exact name-only delta.

## Phase 2: one corrected mapping runtime

Authorize exactly one fresh, detached non-PTY HK runtime, timeout 2400 seconds,
using the same staged dist, C0 DXMT overlay, cache, managed actuator, save/config
contract, and product bytes as the accepted attempt. Do not run ABZU.

Immediately after `final-child.json` appears, compare the actual child against
the Phase 1 proof. Continue only with exactly 116 names, zero added/missing or
secret-bearing names, exact five-entry `WINEDLLPATH` hash above, READY contract,
and the expected actuator SHA. Otherwise stop the owned prefix, no retry.

Wait passively for the exact `Performing automatic level start` marker. After
the marker:

1. Take one passive `sample` and identify the native thread whose stack contains
   both `hb_jit_helper_exec_two_block_loop` and `mem_read`.
2. Reuse the accepted corrected LLDB scripts unchanged. Require that exact TID
   to exist, set the one-shot helper breakpoint restricted to it, and continue
   for at most 30 seconds. Expected handled SIGSEGV/SIGBUS/SIGILL must pass
   through without stopping; an unhandled crash or wrong stop is `UNKNOWN`.
3. On the exact hit, capture once: native TID, `x0/x1/x2`, `ctx->pc`, both block
   headers, guest RIPs, instruction counts, and exact IR bytes. Detach cleanly.
4. Map the captured helper state to the exact guest instruction/two-block loop.
   Do not infer guest RIP from the old run-local host PC `0x11d397e38`.

No `LOOP_TOP`, high-rate trace, input, focus/activation, scene forcing, shader
override, cache reset, or broad probe is allowed.

## Phase 3: bounded wall repair, conditional on a valid capture

- If the capture proves one incorrect/missing decoder or execution family,
  implement the smallest family-level fix. Add the exact instruction regression,
  containing two-block/corridor regression, and existing core floors. Rebuild
  only required HK products.
- Then authorize one validation runtime with the same clean 116-name launch.
  Require scene/managers, the automatic-level marker, and post-marker
  `GetBuffer/Present/Present1/Draw/encoder` deltas. Capture pixels passively.
- If interpreter and JIT agree, do not patch. Identify the missing state writer
  or stable guest loop and stop; do not stack a second hypothesis.
- If the exact LLDB hit is missed, stop `UNKNOWN`; no retry and no speculative
  source change.

## Milestone policy

- Restored post-scene submission is a verified capability even if pixels remain
  black: create a compact `VERIFIED_CAPABILITY_NOT_GOLDEN` snapshot excluding
  caches, verify its checksums, and commit only the narrow source/tests plus the
  checkpoint metadata.
- A verified non-black game pixel requires an immediate verified GOLDEN snapshot
  and narrow commit.
- `UNKNOWN`, missed capture, orchestration failure, or static-only repair gets no
  product snapshot and no source commit.
- Never commit broad dirty diffs or mutate/remove prior evidence.

## Deliverable

Update
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-CAPTURE-CORRECTED-RESULT.md`
with the clean-environment proof, runtime identity, exact LLDB adjudication,
guest RIP/two-block mapping, conditional repair/validation result, hashes, and
cleanup. Preserve the invalid 129-name attempt as evidence. Cleanup only owned
prefix/staged residue, keep translation caches, report disk headroom, and return
control.
