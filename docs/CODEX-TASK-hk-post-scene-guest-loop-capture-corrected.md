# TASK: HK post-scene guest-loop capture, corrected LLDB signal policy

Use `gpt-5.6-sol` at `xhigh`. Preserve the completed invalid capture attempt;
do not overwrite or relabel it.

## Why one corrected mapping run is authorized

The prior run in
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-map-20260722-162042`
was otherwise valid and reproduced the product wall, but LLDB stopped on an
unrelated Wine-handled `EXC_BAD_ACCESS` in `wine_xinput_hid_update` before the
thread-filtered helper breakpoint. The supervisor configured only `SIGINT`; it
did not pass Wine/HyperBridge fault and suspend signals to the inferior.

Prior report:
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-RESULT.md`
(SHA-256
`d30ab769623060912ae1eb339cd9b75a03ac18fc0b1a6d3f444ff500021f22ce`).

This is debugger orchestration failure, not a product result. Authorize one
corrected mapping runtime with identical product bytes and baseline. No source
fix, rebuild, or second product runtime is allowed before a valid target hit.

## Immutable baseline

Follow
`docs/CODEX-TASK-hk-post-scene-guest-loop-pair-and-fix.md` and
`docs/CODEX-TASK-hk-post-scene-passive-stack-classification-corrected.md`
exactly, including:

- checkpoint commit `760b903c` and checkpoint checksums;
- Unix `ntdll.so`
  `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`;
- ARM64X `ntdll.dll`
  `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`;
- actuator PE
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`;
- exact `116/116` child environment and five-entry child `WINEDLLPATH` hash
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`;
- same staged runtime, C0 DXMT overlay, deploy order, detached non-PTY lifecycle
  and 2400-second bound.

The only authorized runtime correction is the LLDB signal policy below. No
input, focus/activation, shader work, scene forcing, profiler change, cache
reset, product install, high-rate trace, or `LOOP_TOP` is allowed.

## Mandatory dry run before HK

Reuse the already-created read-only capture script and supervisor only after
copying them into the new run directory and sealing their hashes. Correct the
supervisor command sequence so that, after attach and before `continue`, it
executes and records:

```text
process handle SIGSEGV --stop false --notify false --pass true
process handle SIGBUS  --stop false --notify false --pass true
process handle SIGILL  --stop false --notify false --pass true
process handle SIGUSR1 --stop false --notify false --pass true
process handle SIGUSR2 --stop false --notify false --pass true
process handle SIGINT  --stop true  --notify true  --pass false
process handle -t
```

Do not pass `SIGTRAP` or `SIGSTOP`.

Extend the throwaway fixture so it installs a SIGSEGV handler, executes
`raise(SIGSEGV)`, proves the handler returns, and then enters
`hb_jit_helper_exec_two_block_loop`. With the exact corrected supervisor,
require all of the following before launching HK:

- policy table shows PASS/STOP/NOTIFY = `true/false/false` for
  SIGSEGV/SIGBUS/SIGILL/SIGUSR1/SIGUSR2;
- fixture SIGSEGV is passed to and handled by the inferior;
- LLDB does not stop on that fault;
- the exact thread-restricted helper breakpoint is hit;
- capture script writes valid headers/IR and the supervisor detaches cleanly;
- timeout path still detaches and leaves the fixture alive.

Any failure is build-only `UNKNOWN`; do not launch HK.

## One corrected mapping runtime

Run the identical sealed HK baseline once. After the first exact
`Performing automatic level start.` marker:

1. Confirm same child identity and zero post-marker submission/fault deltas.
2. Take one five-second passive `sample` and derive the live native TID whose
   section contains both `hb_jit_helper_exec_two_block_loop` and `mem_read`.
3. Attach corrected LLDB. Require the TID to exist in `thread list`.
4. Set the one-shot `hb_jit_helper_exec_two_block_loop` breakpoint restricted
   to that exact TID, then continue for at most 30 seconds.
5. Accept the stop only when stop reason is that breakpoint and selected TID is
   exactly the requested producer TID. Any other stop must be classified:
   expected passed signals continue automatically; an unhandled crash or wrong
   stop is `UNKNOWN`, followed by clean detach and no retry.
6. On the valid hit, run the existing read-only capture exactly once, verify
   `x0/x1/x2`, `ctx->pc`, both block headers and exact IR byte lengths, then
   detach immediately. Never call inferior code or modify registers/memory.

## After a valid capture

Resume Phase 2 and the conditional Phase 3 of
`docs/CODEX-TASK-hk-post-scene-guest-loop-pair-and-fix.md`:

- reconstruct the guest pair, module RVAs, bytes, IR and memory condition;
- create the smallest exact-byte interpreter-vs-JIT fixture;
- patch only a proven mismatch with a failing-then-passing test;
- allow one validation runtime only after focused/static/regression floors pass.

If interpreter and JIT agree, do not patch: identify the missing writer/state
transition and stop. Do not stack hypotheses.

## Deliverable

Write
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-CAPTURE-CORRECTED-RESULT.md`
and retain links/hashes to both the invalid and corrected attempts. A valid
guest-pair capture may be preserved as compact `NOT_GOLDEN` evidence; create a
capability checkpoint/commit only if a tested fix restores post-scene Present.
Create GOLDEN only for a verified non-black product pixel. Preserve caches,
clean only scoped runtime residue, and report disk headroom.
