# TASK: HK post-scene passive stack classification

We have a verified blocker floor, not a shader verdict:

- checkpoint:
  `reports/phase4-hollow-knight/checkpoints/20260722-post-scene-submission-stall-125405-VERIFIED_BLOCKER_NOT_GOLDEN`
- `Performing automatic level start.` reached;
- before the boundary: `GetBuffer=1`, `Present/Present1=48/48`;
- for 352 seconds after it: `GetBuffer/Present/Present1/draw/encoder=0`;
- same process/window alive, five black captures, faults/rejects/HUP all zero.

## Goal

Classify the exact native wait/spin/exit boundary that stops Unity submission
after automatic level start. This is a direct wall-breaking run. Do not build a
new observer, profiler, admission framework, trace stream, or shader probe.

## Fixed baseline

Reproduce the staged-deploy method from
`docs/CODEX-TASK-hk-post-scene-first-frame-staged-deploy.md` byte-for-byte:

- same Present-capability checkpoint runtime and DXMT overlay;
- same single staged diff, the sealed no-allocation actuator PE
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`;
- exact clean child environment: 116/116 names, zero missing/unexpected/forbidden
  names and zero unapproved value drift;
- detached non-PTY launch, stdin `DEVNULL`, new session/process group;
- one HK run only, timeout 2400 seconds;
- no input, focus/activation action, scene forcing, source edit, rebuild,
  production install, cache deletion, or shader override.

Preflight must fail closed on live HK/Wine/ABZU collisions, contract blockers,
identity mismatch, staged diff count other than one, or wrong deployed actuator
SHA. Do not retry a product run.

## Passive measurements only

Detect the first exact `Performing automatic level start.` marker and preserve
its byte offset and cumulative counters. Resolve the same live HK host PID from
the final child/window evidence; record executable identity, prefix, process
start time, CPU, RSS, state, and thread count.

At boundary +30 seconds and +120 seconds:

1. Confirm the same PID is alive and post-boundary deltas for
   `GetBuffer/Present/Present1/draw/encoder` are still zero.
2. Run one bounded passive macOS `sample` capture for that PID (5 seconds) and
   save the raw sample under the run directory. Record command, rc, start/end
   timestamps, PID identity, file SHA-256, and size. A sampler failure is
   `UNKNOWN`; it must not kill or restart HK.
3. Record a compact process/thread-state snapshot and one per-window pixel
   capture. Observational commands must be zero-safe and individually rc-logged;
   no `grep | wc` under fatal `pipefail`.

Do not emit high-rate logs. Two passive samples are the entire new diagnostic
variable. Do not attach lldb, suspend threads manually, inject code, or alter
the guest.

## Adjudication

Compare both samples and identify, with thread IDs and top frames, which stable
class owns the stopped submission path:

- Wine server/synchronization wait;
- HyperBridge/JIT spin or guest-PC loop;
- `_MetalLayer_setProps` / main-queue `dispatch_sync` deadlock;
- Cocoa run loop with the Wine/Unity producer thread absent;
- managed/Unity scene-loading work;
- another exact stable native boundary.

`STACK_CLASSIFIED_PASS` requires both samples to show the same causal class,
the same HK PID, zero post-boundary submission deltas, and no fault/reject/HUP.
If samples disagree, the PID changes, identity drifts, or the relevant thread
cannot be identified, return `UNKNOWN` and stop. If a post-scene Present appears,
stop sampling, capture the frame immediately, and report that the submission
stall did not reproduce; do not begin shader work in this task.

Deliver:

- `reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-STACK-CLASSIFICATION.md`;
- compact local evidence with raw sample files and hashes;
- exact next fix boundary, but no implementation yet.

No commit or GOLDEN. Preserve caches and clean only scoped prefix/staged clone.
Do not touch ABZU. Historical Return causality remains UNKNOWN.
