# MILESTONE 2026-06-28 — commit-TIME fix implemented but fires 0×: REFUTES the commit-coherence-gap premise. The guest never commits the over-write tail.

**Implemented the VirtualAlloc-commit-time fix as specified** (scoped + traced, in `macrunner_hb_sync_virtual_region`
— the single function all guest commit paths route through). Tested with DIRECT_MEM=1. **It fired 0 times.** Combined
with the commit-on-fault path still firing 265× on the same tail, this is decisive: the guest's tail pages are
**never observed as committed** by MacRunner — the over-write writes into **never-(host)-committed reserved memory**,
not a committed page MacRunner mis-tracked. So the "commit-coherence gap" framing is refuted.

## The fix (kept uncommitted)
`macrunner_hb_sync_virtual_region`: on a writable, non-exec commit whose HOST page is the reserved `VM_PROT_NONE`
(with `max_prot & WRITE`, region covers base), `mprotect` it RW (clamped to the reserved region, never EXEC), traced.
Scoped so it cannot clobber Wine's accessible/RO/exec pages. (Correct general hardening for a real reserve→commit gap;
just not exercised by HK — see below.)

## Result (HK, DIRECT_MEM=1 + commit-time fix + commit-on-fault)
- `macrunner-hb-commit-mprotect` (commit-TIME): **0 fires.**
- `macrunner-hb-reserved-commit` (commit-on-FAULT): **265 fires** (unchanged).
- Still `c000007b` at `0x87efdef9430`, ~617 M steps, rung 9, **no Present**.

## What this proves
`sync_virtual_region` is the one function every guest commit observation flows through (call sites: 7616/7646 fault
sync, 8471 exec, 13371 data alloc, 9699/14195/… specific). My hook lives inside it. **0 fires ⇒ no guest commit path
ever marks the over-write's tail pages writable.** And the host page stays `PROT_NONE` (last turn's probe) ⇒ no commit
reached the host either. Therefore: **the guest does not commit the tail; the memcpy writes past the committed 16 MB
head into never-committed reserved memory.** On Windows that would AV — so HK working on Windows means one of:
1. **MacRunner under-tracks the guest's commits** — on Windows Unity commits more of the 256 MB region (incrementally
   as it fills, or up front), but under MacRunner only the 16 MB head is ever committed (the `[add] protect=0x4`).
   The incremental tail commits aren't happening / aren't reaching the host. → fix = make those commits actually
   commit (find why they don't), NOT commit-time mprotect-on-observe (there's nothing to observe).
2. **The memcpy over-extends under MacRunner** — on Windows the copy is bounded to the committed head; MacRunner's
   JIT runs it longer (a length/loop-bound issue or upstream corruption), writing into the reserved tail.

The alloc-trace (last turn) showed the over-write stays *within* the 256 MB reservation (not past the allocation,
not across gaps) — so it's "correct region, past the committed sub-part," consistent with either 1 or 2.

## Status / decision
- **Keep commit-on-fault** (verified safe: floor-guard rung 9 neutral, unit within flaky band; advances producer
  260×). It is the only mechanism that actually moves HK forward today.
- **Keep the commit-time fix uncommitted** (sound general hardening, fires 0× for HK).
- No pixel → **floor `.so` restored byte-exact**; all fixes uncommitted; no commit.

## Next — the clean differential test (not more MacRunner-side probing)
**CrossOver allocation oracle:** run HK under CrossOver and trace the same 256 MB allocator region's commit extent
(how much of `[0x300000000, 0x310000000)` does Unity commit on the working platform?).
- If CrossOver commits **>16 MB / up to where the memcpy writes** → MacRunner **under-commits** (hypothesis 1): the
  guest's tail commits don't take effect under MacRunner → find/fix that commit path.
- If CrossOver also commits only ~16 MB and the memcpy stays in-bounds → MacRunner's **memcpy over-extends**
  (hypothesis 2, a length/codegen or upstream-corruption issue) → trace the loop bound / length source.
Plus: probe the final `0x3e0` fault page (reserved-tail-the-fix-missed vs the unreserved gap) to characterize the last crash.

**Rails:** scoped+traced fix; floor `.so` restored byte-exact; all fixes (fault-recovery + commit-on-fault +
commit-time + audit probes) uncommitted; no commit (no pixel); scoped `wineserver -k`; disk 51 GB.
