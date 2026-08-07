# MILESTONE 2026-06-28 — CrossOver allocation oracle: +virtual blocked (release build), but boot-config + render-inference => leans HYP1 (MacRunner under-commits); over-write = SSE tail over-shoot at 16MB allocator-block boundary, NOT a runaway

**Goal:** measure Unity's commit extent on the working platform (CrossOver) to settle HYP1 (MacRunner under-commits)
vs HYP2 (MacRunner over-extends). The direct `WINEDEBUG=+virtual` trace FAILED — CrossOver's Wine is a RELEASE build
with `TRACE()` compiled out, so `+virtual` emits nothing (raw 25s capture = only HK stdout, zero `virtual:` lines).
But the run captured HK's boot config, which is decisive in another way.

## Decisive new data — Unity allocator geometry (HK boot config)
```
memorysetup-main-allocator-block-size  = 16777216  (16 MB = 0x1000000)
memorysetup-thread-allocator-block-size= 16777216
memorysetup-gfx-main / gfx-thread      = 16777216
memorysetup-bucket / cache             = 4194304   (4 MB)
```
**Unity's main allocator block = 16 MB — exactly MacRunner's committed `[add] size=0x1000000` chunk.** So:
- MacRunner commits ONE 16 MB allocator block at a time within the 256 MB reservation.
- The over-write dst `rcx=0x300ffffa0` is **0x60 (96 bytes) before the 16 MB block end (0x301000000)**. The SSE
  memcpy's 64-byte chunks straddle that boundary, writing ~32 bytes **past the block end into the next (reserved)
  16 MB block** — a classic **SSE tail over-shoot at the allocator-block boundary**, NOT a 3.5 GB runaway.
- The "3.5 GB span" = the producer sequentially processing **~265 16 MB blocks**, each with a small boundary
  over-shoot (265 commits ≈ 265 block boundaries). NOT corrupted-length, NOT DIRECT_MEM corruption.

## Verdict (inference, strong): leans HYP1 — MacRunner under-commits
`+virtual` can't give CrossOver's commit extent, but the **present-gate oracle already proved HK RENDERS under
CrossOver** (esi=1, present slots execute). Rendering ⇒ the producer's memcpys completed without faulting ⇒ their
targets (incl. the block-boundary over-shoot) are **committed/accessible on CrossOver**. Under MacRunner the same
target is **reserved (PROT_NONE) → fault**. So CrossOver makes that memory accessible and MacRunner doesn't =
**MacRunner under-commits relative to the working platform.**

Cross-check: my commit-TIME fix fired **0×** (`sync_virtual_region` never observes these block commits). So Unity's
16 MB-block commits reach the host on CrossOver but are **not tracked/applied on MacRunner** — they bypass
`sync_virtual_region`. The commit-coherence gap is in an **untracked block-commit path**, not in the over-write itself.

## Fix direction
- **Keep commit-on-fault** (verified safe; the correct pragmatic mechanism for the over-shoot — commits the next
  block's page on the fault; 265 fires; advances producer 260×).
- **Proper root fix:** make MacRunner observe/apply Unity's 16 MB-block commits. They do NOT flow through
  `sync_virtual_region` (commit-time fired 0×), so the right hook is the path that records the `[add] 16 MB` head
  commit (macrunner_hb.c:13370-13371 `remember_virtual_region`+`sync_virtual_region`, or the guest
  NtAllocateVirtualMemory(MEM_COMMIT) interception) — extend whichever handles the SUBSEQUENT block commits to also
  mprotect RW. (commit-time-in-sync_virtual_region was the wrong hook: those commits don't reach it.)
- Remaining last-region c000007b = one block boundary the on-fault path didn't cover (final region edge).

## Caveat to verify (don't hand-wave)
~265 × 16 MB ≈ 4.2 GB of block copying for HK's first frame is a LOT for a 2D game. Likely Unity scene-load
zero-init/copy of its allocator blocks, but worth a sanity check that the producer isn't re-copying. The forward
progress to rung 9 (past Mono) argues it's real init, not a tight loop.

## Next (to make the verdict DECISIVE rather than inferred — optional)
winedbg under CrossOver: `break *0x6ffffe009430` (UnityPlayer+0x19e9430, the memcpy), conditional on rcx near a
16 MB boundary, then `print *(long*)((($rcx|0xffffff)+1))` — if the next block reads OK on CrossOver, it is committed
there (confirms HYP1). Then fix the untracked MacRunner block-commit path + re-test the ladder → present.

**Rails:** CrossOver bottle only (no MacRunner rebuild/deploy this turn); MacRunner floor `.so` intact (bytes
unchanged); all fixes uncommitted; no commit (no pixel); scoped `wineserver -k`; disk 51 GB.
