# MILESTONE 2026-06-28 — JIT fault-recovery fix IMPLEMENTED+VERIFIED; DIRECT_MEM = 22× producer throughput; new wall = SSE over-read into unmapped page

**Deliberate, per operator's non-negotiables:** reconciled the uncommitted WIP first (built on the known clean
floor), ran regression BEYOND HK (unit suite + floor-guard), and made **no commit** (no pixel) — floor `.so`
restored byte-exact. My source fix retained uncommitted for follow-up.

## The fix (implemented)
Root gap (prior milestone): the JIT signal guard caught a recoverable native fault and called
`set_jit_interp_fallback_result(... out->faulted=true)`, but the caller just `return`ed → `c000007b`. The
"interpreter fallback" was named but never re-ran the block.

Implemented (uncommitted): `hb_runtime.c` + `hb_runtime.h` + `hb_arm64_codegen.c`:
1. Added `bool force_interp` to `hb_block_cache_entry_t`.
2. Un-static `hb_jit_helper_exec_ir_block_once` (block-level fault-safe IR interpreter) so the runtime can call it.
3. Named the recoverable-fault reason (`HB_JIT_NATIVE_SIGNAL_FAULT_REASON`); the guard sets it by pointer.
4. At BOTH JIT-loop dispatch sites: on a guard fault with that reason, **demote the block to `force_interp`,
   clear the fault, and re-run it via the fault-safe IR interpreter** (the guard already restored ctx to block
   entry, so re-running from entry is correct). `force_interp` blocks skip native exec thereafter (no re-fault).
   If the interpreter ALSO faults → genuine fault, propagate.

## Verification (regression BEYOND HK, per non-negotiable #2)
- **Compiles clean** under `-Wall -Wextra -Werror` (incremental, 2.6 s).
- **Non-HK unit suite:** no NEW deterministic failures vs baseline. The suite is flaky (proved: two identical
  builds run back-to-back differ — flaky `out.result` execution tests + environmental `mprotect errno=13`); the
  only stable failures are pre-existing WIP `code_buf->size` codegen-size assertions, present in baseline AND mine.
- **Floor-guard (HK, DIRECT_MEM off):** rung 9 / PRESENT_MISSING / clean — **fix is neutral on the floor**
  (recovery never triggers without a fault), exactly as designed.

## Payoff run (HK, `MACRUNNER_HB_JIT_DIRECT_MEM=1` + fix)
- **The fix works:** the guard caught the SIGBUS (`signal=10`), recovery demoted+interpreted, and the producer
  ran **22× more steps (177,554 → 2,368,086)** — confirming DIRECT_MEM is a large producer-throughput lever.
- **But it uncovered a NEW genuine wall:** at `unityplayer 0x87efdef9430` (rva 0x19e9430) the SSE memcpy
  `movdqu [rdx]/[rdx+0x10]/[rdx+0x20]/[rdx+0x30]` reads **across a page boundary into unmapped `0x321000000`**
  (`fault=0x321000000`, 16-aligned). The fault-safe **interpreter ALSO returns `MEMORY_FAULT`** → this is a
  **genuine memory fault, NOT a direct-mem/codegen artifact** (recovery correctly propagated it → `c000007b`).
  → It's an SSE over-read that's benign on Windows (next page committed) but faults on MacRunner (mapping gap);
  DIRECT_MEM merely accelerated the producer far enough to reach it (2.4M steps in; the slow floor times out first).

## Decision (per non-negotiables)
- **No pixel → no commit.** DIRECT_MEM still `c000007b`s before Present (still rung 9). Floor `.so` restored
  byte-exact (Codex's deployed state unchanged). Source fix kept uncommitted.
- **Do NOT default-on DIRECT_MEM** — it reaches a genuine fault. The fault-recovery fix is correct and a
  prerequisite, but not sufficient alone.

## First-publish / present / frame status (asked)
- First-publish: not improved-to-completion — with DIRECT_MEM the producer runs 22× further but `c000007b`s at
  the over-read before publishing; floor (DIRECT_MEM off) = ~30 s, PRESENT_MISSING, no crash.
- Present / frame: **none** (no truePresent, no pixel) in either config.

## Next (precise)
The new wall is **memory-mapping completeness**, not codegen: an SSE memcpy over-reads past a buffer at a page
boundary into unmapped `0x321000000`. Options: (a) ensure guest allocations have a mapped guard/padding page after
them (Windows-like), so benign SSE over-reads don't fault; (b) in `hb_memory_read`/special_read, treat a read that
straddles into an adjacent unmapped page as a partial/zero-filled read for the in-bounds part (matches x86
over-read semantics where the program only USES the in-bounds bytes); (c) RE-CONFIRM it's an over-read (dump rdx +
the source-buffer bounds at the fault) vs a genuinely bad pointer from upstream. Then re-test DIRECT_MEM with the
fix; if it clears, run the floor-guard + unit regression again before considering default-on (+ snapshot only on a
real pixel).

**Rails:** WIP triaged first; built on the verified clean floor; regression beyond HK (unit + floor-guard); floor
`.so` restored; no commit (no pixel); scoped `wineserver -k`; disk cleaned. Backups in session scratchpad.
