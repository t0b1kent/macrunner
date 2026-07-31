# Chaining's defect is snapshot granularity, not control transfer — 2026-07-31

Five runs, three refuted hypotheses, and then a structural mismatch that explains every
observation. Written before attempting the fix, so the reasoning can be checked against the
evidence rather than against the outcome.

## The mismatch

`run_jit_block_with_signal_guard` (`hb_runtime.c:4266`):

```
:4322   hb_ctx_snapshot_save(&frame.snapshot, ctx);     // once
:4343   exec(ctx);
:4355   hb_ctx_snapshot_restore(ctx, &frame.snapshot);  // on the fault path
```

The snapshot is taken **once per dispatch**. Without chaining a dispatch is exactly one block,
so the contract holds: a fault part-way through a block rolls that block back and the dispatcher
retries.

**Chaining makes one dispatch span several blocks.** Our own traces show `blocks=2`, `3`, `4`,
`6` per transition. The snapshot still describes the state before the FIRST block of the chain,
so a fault in the third block restores the context to before the first — **discarding guest work
that legitimately completed**.

The restore path is not exceptional here. This engine takes **3 665 920 faults per run, 99.3% of
them BUS_ADRALN**, at a rate that grows 1324/s -> 5895/s. It is one of the hottest paths in the
system.

## Why this fits every observation

- **Damage accumulates across consecutive chained edges** — one rollback per fault, and faults
  are continuous.
- **The values are identical run to run.** They are not garbage; they are *stale* — a coherent
  state snapshotted at the chain entry. Determinism is expected, not surprising.
- **Declining the known-bad edge only relocated the fault**
  (`MACRUNNER_HB_CHAIN_REFUSE_NEAR`: `reason=runtime pc=0x87ef2469a30` became
  `reason=import-thunk pc=0x6f0000003b90`). Any chain longer than one block has the defect, so
  the differential's verdict "general to chaining" is exactly what this predicts.
- **Mixed-era register state** — `rcx 5000000000000 -> f0e0993f`, `rdx 11a5fdb60 -> 320eec31`,
  `rbp` collapsing to 8 — is what a partial rollback looks like: some values from before the
  chain, some from after.

## What was ruled out first, and how

1. **Trampoline entering the successor at +12, re-running `MOV X19, X0` with a helper's return
   value in X0.** Refuted by reading: the patched chain slot writes `MOV X0, X19` before
   branching, so X0 is the context there by design. (`+16` was tried and changed nothing;
   reverted.)
2. **A chainable terminator leaving `ctx->pc` stale, so the trampoline guard admits the wrong
   successor.** Refuted by reading all five emit paths — `JMP`, `Jcc` (fused and helper), `CALL`
   direct (native and helper) and `CALL` indirect all store `ctx->pc`.
3. **Stale `rip`.** This one was a genuine invariant break — `sync_arch_pc_after_jit_block`
   (`:3430`) runs only in the C dispatcher, which a chained edge never reaches. Fixed and kept
   (commit `1f1080de`); the log now shows `rip == pc`. **The fault did not change.**

## The shape of the fix

The snapshot exists to undo a *partially executed* block. Work by blocks that completed before
the faulting one is legitimate and must not be undone. So the rollback granularity has to be the
block, not the dispatch.

The naive form — refresh the snapshot at every chained edge — costs the 760-byte
`hb_ctx_snapshot_save` per block, which is a large part of the per-dispatch cost chaining exists
to remove (~350-500 host instructions and ~1.5 KB of traffic per block, against a translated
block of ~34 ARM64 instructions). Paying it back per block would surrender much of the win.

Directions worth measuring, cheapest first:

1. **Narrow what the snapshot covers.** `HB_CTX_INTERP_ONLY_BEGIN/END` already carve out an
   interpreter-only region. If the set of fields a partial block can leave inconsistent is
   smaller than "the whole context", the per-edge refresh becomes cheap enough to be free.
2. **Make the rollback non-destructive for completed blocks** — restore only the faulting
   block's effects rather than the chain's, which needs to know where the faulting block began
   (`block_cache_find_native_pc(rt->block_cache, frame.host_pc)` at `:4356` already recovers the
   faulting entry).
3. **Refresh the snapshot in the chain slot**, so the cost is paid only on edges actually taken,
   and only for the fields from (1).

## Status

Gate default OFF; nothing regresses. Chaining now reproduces identically across five runs at the
same guest pc, with the mechanism named and the control-transfer path exonerated by reading
rather than by argument.

Not claimed: that fixing this makes chaining work. It is the first hypothesis that explains all
four observations at once, which the previous three did not.
