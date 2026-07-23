# TASK: HK Mono `jit_code_hash` main-runtime-thread causal capture

## Why this is the next run

The mode-normalized run was otherwise valid: strict clone verification passed
`4732/4732`, the child identity passed, automatic level start occurred, and
the renderer reached `GetBuffer=1`, `Present=48`, and `Present1=48` without a
fault or reject.

Its LLDB capture did not test the intended state. The selector chose the first
thread containing `hb_jit_helper_exec_two_block_loop` and `mem_read`:
`Thread_6099551`. That thread is under `macrunner_hb_call_import_thunk`, not
under `hb_jit_runtime_run`; its guest PC `0x87ef2945bcb` is not the sealed Mono
`mono_internal_hash_table_lookup` corridor.

The same passive sample contains the one semantically relevant candidate:
`Thread_6100216`, whose path is:

```text
macrunner_hb_x64_thread_entry
  -> macrunner_hb_run_x64
  -> hb_jit_runtime_run
  -> run_jit_block_with_signal_guard
  -> hb_jit_helper_exec_two_block_loop
  -> hb_jit_helper_exec_ir_block_once
  -> exec_instr
```

This task corrects selection only. It does not authorize a product change,
rebuild, shader test, input work, observer, additional profiler feature,
production install, or retry.

## Phase 0: deterministic target selection

Create the runtime-local selection helper and its fixture under this task's
new evidence root. It must parse a post-marker sample into per-thread blocks
and select a target only when exactly one block contains all of:

1. `macrunner_hb_run_x64`;
2. `hb_jit_runtime_run`;
3. `run_jit_block_with_signal_guard`;
4. `hb_jit_helper_exec_two_block_loop`; and
5. `hb_jit_helper_exec_ir_block_once`.

Any block rooted in `macrunner_hb_call_import_thunk` without the complete
runtime path is not a target. Do not select by sample order, helper hit count,
or the first matching substring.

Run exactly these offline fixtures before Wine:

- the preserved 105043 sample must select only `Thread_6100216`;
- a fixture containing the old `Thread_6099551` import-thunk block must select
  no target;
- zero or multiple full-runtime candidates must return `AMBIGUOUS_OR_ABSENT`
  and prevent LLDB attachment.

Write the fixture decisions as compact JSON. This is a small selection check,
not a new admission or preflight framework.

## Phase 1: one valid runtime, same accepted identity

Recreate the disposable mode-normalized clone exactly as specified in
`docs/CODEX-TASK-hk-mono-jit-hash-cycle-mode-normalized-clone.md`:

- source candidate and historical tree remain immutable;
- only the two established `0755 -> 0555` clone modes may be changed;
- strict historical verifier must pass all `4732/4732` entries after the two
  changes; do not weaken its mode policy.

Start from the accepted 2104 foreground launcher. Relative to it, alter only
`RUN_DIR`, `PREFIX`, and `STAGED_DIST` for this owned clone. Do not export
orchestration variables. Do not use a detached wrapper, `&`, `nohup`, `setsid`,
`disown`, a PTY wrapper, or a second launcher.

Before LLDB, require:

- `run-contract=READY` with zero blockers;
- child environment `116/116` and zero forbidden inherited names;
- raw `WINEDLLPATH` SHA-256
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`;
- profiler SHA-256
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.

Wait for the exact automatic-level-start marker. Take two passive post-marker
samples roughly 10 seconds apart. Both must select the same unique full-runtime
candidate by Phase 0's topology. Otherwise record the selection table and stop
without LLDB or a memory write.

## Phase 2: one targeted capture and conditional causal cut

Attach LLDB once, restricted to that selected TID, using the accepted signal
policy and one-shot `hb_jit_helper_exec_two_block_loop` breakpoint.

Execute the existing `hk-mono-cycle` capture from
`docs/CODEX-TASK-hk-mono-jit-code-hash-cycle-causal.md` unchanged. The capture
must validate that the guest PC is in the sealed Mono image and satisfies all
existing byte, caller, callback, and bucket checks before any write.

- If the exact Mono loop validates and a real chain back-edge is proved, make
  exactly one existing-authorized edge cut and perform the bounded post-cut
  measurement in the same process.
- If the selected main-runtime capture is not the Mono loop, record its exact
  PC, two translated blocks, caller path, and decision as
  `MAIN_RUNTIME_NOT_MONO_HASH_LOOP`. Do not mutate memory and do not try another
  thread or another runtime.
- If the chain is finite or a call/return mismatch is proven, keep the existing
  classification rules and do not invent a cut.

## Result

Update
`reports/phase4-hollow-knight/PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`
with the old wrong-candidate retraction, fixture decisions, both sample
selection tables, live identity, target capture, any conditional cut, and
post-cut counters. Seal compact NOT_GOLDEN evidence.

Create an immediate verified capability snapshot only if a non-black game frame
appears. Otherwise do not call the outcome GOLDEN or commit product code.
Preserve translation caches. After sealing evidence, remove only the owned
prefix and disposable clone.
