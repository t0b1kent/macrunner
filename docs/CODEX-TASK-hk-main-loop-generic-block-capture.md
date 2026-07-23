# TASK: HK main post-scene loop generic block capture

## Objective

The Mono `jit_code_hash` hypothesis is closed for the actual post-scene
main-runtime thread. A valid run selected the same full-runtime thread in two
samples, reached automatic level start, and captured:

```text
ctx->pc = 0x87efd2441c0
hb_jit_helper_exec_two_block_loop
  -> <unknown guest caller>
  -> hb_jit_runtime_run
  -> macrunner_hb_run_x64
```

That PC is not the sealed Mono `mono_internal_hash_table_lookup` corridor. The
current Mono-specific capture deliberately did not retain generic block and IR
data (`optional_exec_instr_stop.attempted=false`). This task captures the real
loop once using the already validated read-only generic capture from 2104.

This is a direct wall investigation, not a new observer or preflight system.
No product source, build, install, cache, input, focus, shader override,
memory write, retry, or second HK runtime is authorized.

## Immutable capture implementation

Copy these two existing scripts into this task's evidence root and seal their
SHA-256 before use. Do not modify either script:

| Script | SHA-256 |
| --- | --- |
| `reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104/dry-run/hk_lldb_capture.py` | `c2c0bab9af56885602ed98e227d409902f1f3d9ce41a5e98c3286145ce3192af` |
| `reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104/dry-run/lldb_capture_supervisor.py` | `0bea31fe7a8e2b72a5c089d78699f2591bcf59c0dae73b81db968d0bda523154` |

Use the accepted signal policy already proven by the main-thread capture. The
generic capture is read-only: it must not call inferior code, write guest
memory, modify registers, or issue `--cut-if-cycle`.

## One runtime

1. Recreate the disposable mode-normalized staging clone exactly as in
   `docs/CODEX-TASK-hk-mono-jit-hash-cycle-mode-normalized-clone.md`: source and
   historical tree immutable, only the two established clone modes changed,
   strict verifier `4732/4732` after correction.
2. Reuse the accepted 2104 foreground launcher. Relative to it, alter only
   `RUN_DIR`, `PREFIX`, and `STAGED_DIST` for the owned clone. No exported
   orchestration variables, detached wrapper, `&`, `nohup`, `setsid`, `disown`,
   PTY wrapper, or second launcher.
3. Require the already proven live identity before LLDB: `run-contract=READY`,
   child `116/116`, forbidden inherited names `0`, raw `WINEDLLPATH` SHA-256
   `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`, and
   profiler SHA-256
   `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.
4. Wait for exact `Performing automatic level start.` Take two passive
   post-marker samples about ten seconds apart. Use the already proven semantic
   selector from `CODEX-TASK-hk-mono-jit-main-thread-capture.md`: both samples
   must select the same unique full-runtime path
   `run_x64 -> hb_jit_runtime_run -> run_jit_block_with_signal_guard ->
   two_block_loop -> ir_block_once`.
5. Attach LLDB once to that TID. Set the one-shot
   `hb_jit_helper_exec_two_block_loop` breakpoint restricted to that thread,
   continue for at most 30 seconds, run the immutable generic capture exactly
   once on the valid breakpoint hit, then detach cleanly.

## Required generic evidence

The capture must contain and validate:

- PID, selected native TID, stop reason, and signal-policy result;
- `x0` context and `x1`/`x2` block addresses;
- exact `ctx->pc` and all guest general registers;
- both block headers, instruction counts, raw IR bytes, and raw guest-byte
  windows with hashes;
- host backtrace and loaded-module list sufficient to resolve the owner of
  `ctx->pc` to a module plus RVA, or explicitly prove it is unmapped;
- a no-write declaration (`memory/register/inferior-call = false`).

After detach, perform static-only analysis of this captured pair:

1. Decode every instruction in both captured guest blocks from the recorded
   bytes, including direct/indirect call and branch targets.
2. Match each decoded instruction to its captured IR operation(s).
3. Resolve `ctx->pc` to the owning PE/module and RVA from the captured mapping;
   never assume Mono.
4. State the exact loop condition: register/memory read, branch condition, and
   callback or state writer that must change for submission to resume.
5. Compare interpreter semantics with the recorded HyperBridge IR only for the
   exact captured instruction pair. Do not propose a broad ISA family or patch
   before this comparison is complete.

## Result

Write
`reports/phase4-hollow-knight/PIXEL-FIRST-MAIN-LOOP-GENERIC-CAPTURE-RESULT.md`
and seal compact NOT_GOLDEN evidence. The verdict must be one of:

- `EXACT_IR_OR_EXECUTION_MISMATCH_PROVEN`;
- `STATE_OR_CALLBACK_WRITER_IDENTIFIED`;
- `UNMAPPED_GUEST_PC`;
- `UNKNOWN_CAPTURE_INVALID`.

No source fix or validation runtime belongs in this task. A verified non-black
frame is the only trigger for an immediate capability snapshot. Preserve
translation caches; after sealing evidence, remove only the owned prefix and
disposable clone.
