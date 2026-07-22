# TASK: HK post-scene passive stack classification, corrected parent env

The previous attempt is `UNKNOWN` because it consumed the accepted five-entry
**child** `WINEDLLPATH` as its parent value. Unchanged
`scripts/mr-run.sh:405-410` then prepended `MACRUNNER_DXMT_ROOT`, producing six
entries. The game was stopped before the scene boundary and neither passive
sample ran. This was orchestration only, not a product result.

This message authorizes one corrected runtime. Preserve the invalid attempt and
do not rerun it.

## Only correction

Follow `docs/CODEX-TASK-hk-post-scene-passive-stack-classification.md` exactly,
with one and only one launch correction:

1. Start from the same sealed 116-name child-environment template.
2. For the **parent** `WINEDLLPATH`, remove exactly the leading bare
   `MACRUNNER_DXMT_ROOT` component. The parent value must contain exactly these
   four ordered, unique roots:
   - DXMT `x86_64-windows`;
   - DXMT `x86_64-unix`;
   - staged runtime `x86_64-windows`;
   - staged runtime `x86_64-unix`.
3. Do not edit or bypass `scripts/mr-run.sh`. Its normal prepend must produce the
   accepted child value:
   `MACRUNNER_DXMT_ROOT:<four parent roots>`.
4. Before launch, prove the computed expected child value has five entries,
   five unique entries, and SHA-256
   `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`.
5. As soon as actual `final-child.json` exists, require exact equality with that
   computed value plus the original 116/116 name set, zero
   missing/unexpected/forbidden names, and zero other unapproved value changes.
   Any mismatch is `UNKNOWN` and an immediate scoped stop.

No source edit, rebuild, new observer, new profiler, trace stream, input,
focus/activation action, scene forcing, shader override, production install, or
cache reset is authorized. Keep the same staged runtime, DXMT overlay, actuator
SHA `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`,
detached non-PTY lifecycle, and 2400-second bound. One product run only; no
retry.

## Required product measurement

After the exact first `Performing automatic level start.` boundary:

- at +30 seconds and +120 seconds, confirm the same HK PID remains alive and
  post-boundary `GetBuffer/Present/Present1/draw/encoder` deltas remain zero;
- take exactly one five-second passive macOS `sample` of that PID at each point;
- record PID/executable/prefix/start-time identity, process state, CPU, RSS,
  thread count, sample rc/size/SHA-256, and one pixel capture at each point;
- keep every observation zero-safe and rc-logged so an empty count cannot kill
  the monitor or HK process.

Do not attach lldb, inject code, suspend threads manually, or add high-rate
logging. If a post-scene Present appears, capture immediately and report that
the submission stall did not reproduce; do not start shader work.

## Verdict

`STACK_CLASSIFIED_PASS` requires both samples to identify the same stable causal
class and relevant thread(s), with exact top frames, same PID, zero
post-boundary submission deltas, and zero fault/reject/HUP. Classify the boundary
as Wine synchronization, HyperBridge/JIT spin, Metal/main-queue deadlock,
Cocoa-only with missing Unity producer, managed/scene work, or another exact
native stack. Disagreement, missing samples, identity drift, or an unidentifiable
thread is `UNKNOWN`.

Update
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-STACK-CLASSIFICATION.md`
with a clearly separate corrected-attempt section and preserve the first
attempt's `UNKNOWN`. Deliver compact evidence and the exact next fix boundary.
No commit, snapshot, GOLDEN, or implementation; the coordinator will checkpoint
only a determined result. Preserve caches, remove only the scoped prefix/staged
clone, and do not touch ABZU. Historical Return causality remains `UNKNOWN`.
