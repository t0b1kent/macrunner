# TASK: HK post-scene guest loop pair -> exact HyperBridge fix

Use `gpt-5.6-sol` at `xhigh`. This is a direct product-wall task, not an
observer-framework task.

## Proven floor

Verify first:

- checkpoint:
  `reports/phase4-hollow-knight/checkpoints/20260722-post-scene-jit-spin-145129-VERIFIED_BLOCKER_NOT_GOLDEN`;
- manifest SHA-256:
  `e9bd8c9a8068228b8da56bd64a72f0a3f84b859cf539b95eb1a66c3a736c11bf`;
- `SHA256SUMS` SHA-256:
  `b390b4efc5967172f8c1eb88a18cfb92e2534a2adf1b2b0b9cd6e7454030b3dc`;
- checkpoint commit: `760b903c`.

The floor proves a stable post-scene Unity producer thread in:

`hb_jit_runtime_run -> generated ARM64 -> hb_jit_helper_exec_two_block_loop -> exec_instr -> mem_read`

while post-scene `GetBuffer/Present/Present1/draw/encoder=0` and
fault/reject/HUP=0. The old host PC `0x11d397e38` is run-local and MUST NOT be
used as a selector in a new run.

## Goal

Obtain the live two-block guest pair and exact IR/memory condition without
adding a logger, then build a focused interpreter-vs-JIT reproducer. Implement
only a proven semantic/progress fix and validate it once against the post-scene
submission floor.

## Hard scope

- Hollow Knight worktree only. Do not touch ABZU.
- No input, focus/activation, language/scene forcing beyond the already sealed
  no-allocation actuator, shader overrides, cache reset, production install, or
  broad cleanup.
- Do not enable `MACRUNNER_HB_TRACE_HELPER_LOOP_TOP`: current code dumps the top
  table on every `budget_hit` and can create a trace bomb.
- No new observer, profiler, admission, ledger, or versioned harness.
- At most one mapping runtime. A second runtime is authorized only after a
  specific source fix has a focused failing-then-passing test.

## Phase 0: static/live-debug dry run

Before launching, prove all of this:

1. The staged Unix `ntdll.so` SHA is
   `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`;
   ARM64X PE `ntdll.dll` is
   `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`.
2. `nm` resolves `_hb_jit_helper_exec_two_block_loop` in the Unix binary.
3. Compile a throwaway `offsetof` checker outside tracked source and require:
   `sizeof(hb_ir_block_t)=72`, block `guest_addr/instrs/instr_count` offsets
   `8/16/24`; `sizeof(hb_ir_instr_t)=184`, instruction
   `op/cc/dst/src1/src2/guest_addr/guest_len/target` offsets
   `0/4/8/56/104/152/160/168`; `hb_context_t.pc` offset `544`.
4. Prepare an ephemeral LLDB Python command under the new run directory. It may
   only read process memory and write evidence. At a breakpoint hit it must
   record `x0/x1/x2`, `ctx->pc`, both 32-byte block headers, exact
   `instr_count * 184` IR bytes for each block, general registers, backtrace,
   loaded-module bases, timestamps, and read errors. Bound each block to
   `instr_count <= 256`; otherwise stop UNKNOWN. It must not call code in the
   target or mutate target memory.
5. Demonstrate the script against a local fixture or a stopped throwaway
   process. Prove timeout/detach behavior. Do not launch HK if this dry run
   fails.

## Phase 1: one mapping runtime

Reuse the exact runtime/deployment/lifecycle from
`docs/CODEX-TASK-hk-post-scene-passive-stack-classification-corrected.md`:

- same staged runtime, C0 DXMT overlay and sealed actuator PE
  `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`;
- exact clean child environment `116/116`;
- parent `WINEDLLPATH` has the proven four unique roots and unmodified
  `mr-run.sh` prepends DXMT to produce the exact five-entry child value with
  SHA-256 `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`;
- deploy actuator after prefix sync and before final child;
- detached non-PTY lifecycle, one process, 2400-second bound.

Fail closed on any identity/deploy/env drift.

After the first exact `Performing automatic level start.` marker:

1. Confirm the same child PID is alive and post-scene submission deltas remain
   zero.
2. Take one bounded 5-second macOS `sample`. Identify the native TID whose
   stack contains both `hb_jit_helper_exec_two_block_loop` and `mem_read`.
   Require that it is the same Unity/DXGI producer class as the checkpoint.
3. Attach LLDB to that PID. Set a one-shot breakpoint on
   `hb_jit_helper_exec_two_block_loop` (the LLDB-resolved name; Mach-O `nm`
   displays a leading underscore), restricted with `--thread-id` to that
   exact live TID. Bound attach-to-hit to 30 seconds.
4. On the single hit, run only the read-only capture from Phase 0, then detach
   immediately and let the bounded run finish. No second breakpoint, process
   call, register write, memory write, or retry.

Missing symbol, wrong thread, timeout, invalid header, read failure, PID change,
or inability to detach is `UNKNOWN`; preserve evidence and stop.

## Phase 2: identify the actual wall

Offline, decode both captured IR blocks using the checked layouts and source
enums. Recover original guest bytes from live evidence or the exact loaded PE
image/RVA. Produce:

- first/second guest VA, module+RVA, guest bytes and disassembly;
- full ordered IR for both blocks and branch edge;
- the exact `mem_read` operand/effective address and relevant captured x64
  register/flags state;
- whether the available evidence actually proves budget exhaustion; otherwise
  record that point as unknown rather than inferring it from the stable stack;
- one classification: JIT semantic mismatch, stale/incorrect memory mapping,
  valid guest polling loop awaiting an external writer, or insufficient
  evidence.

Create the smallest exact-byte fixture for the pair. Compare interpreter and
JIT per iteration for PC, relevant registers, flags, memory address/value, and
exit/progress. The fixture must fail before any source fix and pass after it.

If interpreter and JIT agree, do not patch HyperBridge. Identify the missing
writer/state transition and stop with a determined next boundary. If evidence
is insufficient, report `UNKNOWN`; do not speculate.

## Phase 3: conditional fix and validation

Only for a proven interpreter/JIT or mapping mismatch:

1. Implement the smallest source fix in the owning HyperBridge path.
2. Run the exact fixture, containing family tests, `phase1_core`, static source
   floors, deterministic rebuild and `git diff --check`.
3. Stage only the rebuilt ntdll pair into a fresh clone; prove the binary diff.
4. Run one validation runtime with the same identity contract, no LLDB and no
   diagnostic trace. Require the old guest pair to make progress and measure
   post-scene `GetBuffer/Present/Present1`, faults and pixel.

If post-scene Present returns, create a compact
`VERIFIED_CAPABILITY_NOT_GOLDEN` checkpoint and a focused commit before further
risky work. If a real non-black pixel appears, create the verified pixel
snapshot immediately. If the fix fails, preserve `NOT_GOLDEN` evidence and do
not retry or stack a second hypothesis.

## Deliverable

Write
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-RESULT.md`
with raw LLDB capture hashes, guest pair/IR reconstruction, fixture result,
classification, any exact source fix, and validation outcome. Update
`docs/ACTIVE-INVESTIGATION.md` and the HK heartbeat. Preserve translation
caches; clean only the scoped prefix/staged clone; report disk headroom and
process residue.
