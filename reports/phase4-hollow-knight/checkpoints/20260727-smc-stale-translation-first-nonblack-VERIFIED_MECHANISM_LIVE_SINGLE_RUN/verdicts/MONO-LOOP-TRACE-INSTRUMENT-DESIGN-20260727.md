# Mono/JIT loop trace instrument design — 2026-07-27

Question: can the existing two-block JIT helper trace distinguish a finite helper budget yield from a permanently non-advancing managed thread, without flooding the live HK log?

Classification: `STATIC_INSTRUMENTATION_FINDING_NOT_PRODUCT_RESULT`.

## Evidence

`engine/hyperbridge/src/hb_arm64_codegen.c:8335-8377` shows that
`hb_jit_helper_exec_two_block_loop()` executes only while `ctx->pc` equals one
of its two captured guest blocks.  It returns immediately after either block
changes `ctx->pc` outside that pair.  At the configured block budget it leaves
the PC in the pair, records a budget hit, returns `HB_OK`, and lets the runtime
re-enter the cached JIT block.  Therefore a helper-stack sample alone proves
neither an infinite helper loop nor a corrupted Mono hash key.

The existing aggregate instrument at lines `7442-7476` already records
`first`, `second`, call count, executed block count, and budget-hit count.  Its
dump condition includes `|| budget_hit` at line `7472`; a hot loop that reaches
the budget consequently emits a full top-table for every helper call.  This is
the known unbounded-output defect, not evidence about guest progress.

The prior valid pair is deliberately not reusable: it was run-local
`0x87efdd9f244` / `0x87efdd9f285` and its second block was a return epilogue
(`mov rbx,[rsp+0x40]; xor eax,eax; ...; ret`) according to
`POST-SCENE-GUEST-PAIR-CAPTURE-20260726.md`.  It cannot establish a later
run's managed-thread state or be installed as a fixed watch address.

## Next experiment

Repair only the aggregate dump rate-limit so that budget hits remain counted
but are dumped at the same first-16/interval cadence.  Compile the affected
JIT object and run a deterministic control that creates more budget-hit records
than the interval; the expected output is a bounded number of top-table
snapshots while the final snapshot contains a non-zero `budget_hits` total.

Only after that control passes will a fresh HK run enable the aggregate trace.
The live run must prove source-to-system32 identity, final-child environment,
and both growing launch logs before its result is interpreted.

No claim is made here that `jit_code_hash` causes the scene stall.  That remains
`[HYPOTHESIS]` until a fresh managed-thread trace supplies a post-spin boundary
or a reproducible failed lookup mechanism.
