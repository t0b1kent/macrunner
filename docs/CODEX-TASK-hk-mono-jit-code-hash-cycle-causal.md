# TASK: HK Mono `jit_code_hash` cycle causal cut

## Objective

Break the current product wall, not build another observer framework.

Use one bounded Hollow Knight runtime to distinguish a real
`MonoDomain::jit_code_hash` chain cycle from a HyperBridge indirect-call/return
error. If and only if a concrete cycle is proved, cut exactly one back-edge in
the disposable process and immediately measure whether scene submission and
pixels resume.

Read first:

- `reports/phase4-hollow-knight/HK-MONO-JIT-CODE-HASH-STATIC-MAP.md`
- `reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-CAPTURE-CORRECTED-RESULT.md`
- `reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104/GUEST-LOOP-CAPTURE-ADJUDICATION.json`

This task authorizes one runtime and one process-memory write only under the
strict cycle condition below. It does not authorize a production source patch,
rebuild, install, cache reset, input/focus work, shader override, retry, or a
second runtime.

## Established facts: do not re-investigate

Exact Mono PE:

- SHA-256 `c9c3d552f0e2abaa19d7233e9595290ae10812642a13486a94bcf3d8f7a6e365`;
- `mono_internal_hash_table_lookup` RVA `0x70d20`;
- stable loop PC RVA `0x70d79`;
- owner `lookup_method` RVA `0x2832a0`;
- lookup call/return RVAs `0x2832f0` / `0x2832f5`;
- `mono_internal_hash_table_insert` RVA `0x70ef0`;
- JIT registration insert call RVA `0x27ddce`.

Exact static layout:

- table = `domain + 0xc0`;
- table fields: hash callback `+0x00`, key callback `+0x08`, next callback
  `+0x10`, size `+0x18` (`uint32`), entry count `+0x1c` (`uint32`), buckets
  `+0x20` (`pointer`);
- `MonoJitInfo::method` = node `+0x00`;
- `MonoJitInfo::next_jit_code_hash` = node `+0x08`;
- `key_extract(node) == *(uint64_t *)(node+0)`;
- `next_value(node) == node+8`.

Previous live state was table `rdi`, current node `rbx`, requested key `rsi`,
bucket index `rdx`, and `rdi == r14 + 0xc0`. Addresses are ASLR-dependent and
must be derived again; do not reuse their old absolute values.

## Phase 0: bounded LLDB command, no product build

Create only an ephemeral LLDB Python command under the new run directory. Base
it on the accepted scripts from the successful 2104 run:

- `hk_lldb_capture.py` SHA-256
  `c2c0bab9af56885602ed98e227d409902f1f3d9ce41a5e98c3286145ce3192af`;
- `lldb_capture_supervisor.py` SHA-256
  `0bea31fe7a8e2b72a5c089d78699f2591bcf59c0dae73b81db968d0bda523154`.

The command must:

1. use the already accepted signal policy and thread-restricted breakpoint;
2. read `hb_context_t` guest registers using the offsets already proved by the
   2104 `ctx-prefix.bin` capture;
3. validate all reads before any write;
4. emit one JSON record containing raw addresses, values, ordered chain nodes,
   hashes, and the decision;
5. contain no loop without a hard cap and no broad tracing.

Before HK, run local synthetic-memory fixtures for: finite chain, self-cycle,
two-node cycle, unreadable node, and traversal cap. All five expected decisions
must pass. This is a script check, not a new preflight product.

## Runtime identity: reuse the proven launch, do not rewrite it

Use the successful 2104 launch construction as the sole template:

- launcher SHA-256
  `df877d9b39ee1d4a79f755fec14170d23aa7017b6f21fbb3921399c109e9f2e5`;
- exact `load_child_environment()` and `os.execve()` behavior;
- exact clean child environment `116/116` and forbidden-name count `0`;
- canonical raw UTF-8 `WINEDLLPATH` SHA-256
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`;
- actual `run-contract.json` must be `READY` with zero blockers;
- same staged dist, C0 DXMT overlay, save/config/data state, managed actuator,
  warmed translation cache, detached non-PTY lifecycle, and 2400-second bound.

Only the new owned run-root/prefix names and the post-marker LLDB command path
may differ from the 2104 launcher. Do not merge inherited terminal/Codex env,
normalize `WINEDLLPATH`, add env names, or alter product bytes.

Required product hashes:

- Unix `ntdll.so`: `9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`;
- ARM64X `ntdll.dll`: `3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`;
- x86_64 `ntdll.dll`: `b08516ba28c2d0d884b22673301bfc6eb3b5b48ee7458cd1248303289e431b78`;
- `winemac.so`: `02b4d94ecb5827ea331decccb846f1bf6ff97df632ea28f6b40951794f8c9604`;
- managed actuator: `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.

Any identity, deployment, contract, collision, or lifecycle mismatch is
`UNKNOWN`; stop before LLDB and do not retry.

## Phase 1: exact live chain adjudication

Wait passively for the first exact `Performing automatic level start.` marker.
Select the post-marker thread containing both
`hb_jit_helper_exec_two_block_loop` and `mem_read`, as in the accepted 2104
capture. Attach once.

At the exact target-thread helper state whose `ctx->pc` maps to Mono RVA
`0x70d79`:

1. derive `mono_base = ctx_pc - 0x70d79`;
2. verify the exact guest bytes at RVAs `0x70d20`, `0x70d79`, `0x2832a0`,
   `0x2832e6`, and `0x27ddbd` against the sealed PE before continuing;
3. record guest `rax/rbx/rcx/rdx/rsi/rdi/rsp/rbp/r13/r14/r15`;
4. require `rdi == r14 + 0xc0` and
   `*(uint64_t *)(rsp+0x28) == mono_base + 0x2832f5`;
5. read and record the six table fields listed above;
6. require `0 < size`, `entry_count < 1<<24`, non-null buckets, and
   `rdx < size`;
7. read `bucket_head = *(uint64_t *)(buckets + rdx*8)`;
8. traverse directly from `bucket_head`, never by invoking guest callbacks.
   For each node record `node`, `method=*(node+0)`, and `next=*(node+8)`.
   Stop at NULL, first repeated node, unreadable/misaligned node, or
   `min(entry_count+1, 4096)` nodes.

Also validate callback semantics without trusting names:

- key callback must return/read node offset `0`;
- next callback must return address `node+8`;
- callback pointers and their first 16 bytes must be recorded and lie within the
  exact loaded Mono image.

Use a second thread-restricted `exec_instr` stop at guest RVA `0x70d84` if the
accepted helper path exposes it. Record post-callback `rax`; the expected value
is the pre-callback current node plus `8`. Failure to obtain this optional stop
does not erase a directly proved memory cycle, but it forbids claiming an
indirect-call mismatch.

## Phase 2: one conditional process-memory cut

Do not write memory unless all of the following are true:

- exact Mono identity and caller checks passed;
- direct traversal found a concrete repeated node;
- every node up to the repeat was readable and 8-byte aligned;
- the repeated edge address is exactly `predecessor + 8`;
- reading that edge again still yields the repeated node;
- no other required read failed.

If true, preserve the complete pre-write chain JSON and bytes. Write exactly one
8-byte zero to `predecessor + 8`, read it back, record the before/after values,
detach, and resume. No other guest or host memory may be changed.

If the chain is finite, corrupt/unreadable, or callback/caller validation fails,
perform zero writes, detach, stop the run scoped to its prefix, and report.

## Phase 3: immediate product measurement

After a valid one-edge cut, keep the same process alive for up to 300 seconds.
Capture passive per-window frames at approximately +10, +30, +60, +180, and
+300 seconds from the cut. Record deltas for:

- `GetBuffer`, `Present`, `Present1`, draw, and encoder;
- scene/manager/automatic-level markers;
- `UNSUPPORTED`, `MEMORY_FAULT`, reject, SIGILL/SIGBUS/c0000005/c000007b;
- exact pixel counts using the existing checker.

Do not send input, activate/focus, inject shader values, or perform another
memory repair.

## Machine verdict

Use exactly one:

- `HASH_CHAIN_CYCLE_CUT_PIXEL_PASS_NOT_GOLDEN`: cycle proved, one edge cut,
  submission resumed, and a verified non-black game frame appeared;
- `HASH_CHAIN_CYCLE_CUT_SUBMISSION_PASS_RENDER_BLACK_NOT_GOLDEN`: cycle proved,
  submission resumed, frames remained black;
- `HASH_CHAIN_CYCLE_CUT_NO_EFFECT_NOT_GOLDEN`: cycle proved and cut, but no
  submission resumed;
- `INDIRECT_CALL_RETURN_MISMATCH_PROVEN_NOT_GOLDEN`: direct chain is finite and
  post-callback `rax != current_node+8` with exact callback bytes validated;
- `FINITE_CHAIN_NO_CALL_MISMATCH_UNKNOWN`: direct chain is finite and callback
  result is correct, contradicting the stable-loop account;
- `UNKNOWN`: any missing identity, capture, required read, or invalid mutation
  precondition.

Do not label any outcome GOLDEN because the only authorized repair is an
ephemeral process-memory mutation. Do not create a product commit.

## Evidence and cleanup

Write:

- `reports/phase4-hollow-knight/PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`;
- a compact `NOT_GOLDEN` evidence directory with the chain JSON, callback bytes,
  context snapshot, mutation record (if any), frame hashes/statistics, runtime
  identity, and `SHA256SUMS`.

Update `docs/ACTIVE-INVESTIGATION.md` and the HK heartbeat with only the new
determined boundary. Preserve the full run directory and translation cache.
Remove only the owned prefix/staged clone, confirm HK/Wine/ABZU residue zero,
and report disk headroom. Never touch the v1-v5 admission apparatus or ABZU.
