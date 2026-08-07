# MILESTONE 2026-06-28 — NtAllocateVirtualMemory trace: VERDICT = LEGIT case-1/2 (over-write into reserved tail of Unity's own 256MB regions), NOT a runaway

**Allocation-level trace (the clean, cold-path test).** Used the existing `MACRUNNER_HB_TRACE_VIRTUAL_REGION`
env on the RESTORED FLOOR `.so` (no rebuild, no code change), filtered for base in `[0x300000000, 0x400000000)`.
Decisive: the over-write target is **NOT one giant alloc** and **NOT many tiny allocs spanned by a runaway** — it
is **multiple 256MB reservations, and the over-write stays within each reservation's own reserved tail**.

## Trace (distinct records, 0x3xx range)
```
base=0x300000000 size=0x10000000 protect=0x1 [sync]   <- 256MB RESERVED (PAGE_NOACCESS)
base=0x300000000 size=0x1000000  protect=0x4 [add]    <- 16MB committed head (PAGE_READWRITE)
base=0x320000000 size=0x10000000 protect=0x1 [sync]   <- 256MB reserved   (512MB after 0x300)
base=0x320000000 size=0x1000000  protect=0x4 [add]    <- 16MB committed head
base=0x340000000 size=0x10000000 protect=0x1 [sync]   + 0x340000000 +4MB committed
base=0x360000000 size=0x10000000 protect=0x1 [sync]   + 0x360000000 +2MB committed
base=0x380000000 size=0x10000000 protect=0x1 [sync]   + 0x380000000 +4MB committed + many 64KB commits
...
```
Pattern: **256MB reservations** (`protect=0x1 NOACCESS`, size `0x10000000`) at **512MB spacing**
(0x300000000, 0x320000000, 0x340000000, 0x360000000, 0x380000000, …), each with a **committed RW head**
(`protect=0x4`). Between reservations are **unreserved 256MB gaps** (e.g. `[0x310000000, 0x320000000)`).

## Why this is decisive (legit, not runaway)
- The over-write starts at dst `rcx=0x300ffffa0` (in the committed head `[0x300000000,0x301000000)`) and crosses
  into `0x301000000` — the **reserved tail of the SAME 256MB region `[0x300000000,0x310000000)`** (exactly the
  host-probe's reserved `[0x301000000,0x308000000)`, max_prot=RWX). = **CASE 1/2** (write into the allocation's own
  reserved page).
- The producer processes ~8 regions (0x300…0x3e0); 265 commits / ~8 regions ≈ **33 pages each** = a small bounded
  over-write into each region's reserved tail. A single 3.5GB runaway would have to cross the **unreserved gaps**
  between regions and fault immediately (the commit-fix cannot commit an unreserved gap). It does NOT — so it is
  **per-region bounded**, not one continuous copy. The "3.5GB span" = touching 8 separate 512MB-spaced regions.
- ⇒ **DIRECT_MEM is NOT corrupting a length; this is not a runaway.** Confirmed.

## Root cause (the real bug under the symptom)
On Windows, writing PAGE_NOACCESS reserved memory AVs — so the guest believes the tail is **committed**, but
MacRunner left it `protect=0x1 (NOACCESS)`. A **commit-coherence gap**: the guest's `VirtualAlloc(MEM_COMMIT)` of the
reserved tail did not make MacRunner's page accessible. Commit-on-fault (my fix) papers over it correctly; the clean
root fix is **commit-time** mprotect.

## Status / next
- **Keep the commit-on-fault fix** — verified safe (floor-guard rung 9 neutral; unit within flaky band) and it is the
  correct mechanism for this legit case; advances the producer ~260×.
- The remaining last-region c000007b (0x3e0) is **one region's reserved tail the fix didn't commit** (a per-region
  edge: differing reserve/commit state, or the over-write crossing past that region's 256MB into the unreserved gap)
  — NOT a corruption/length bug.
- NEXT (no rabbit-hole): (1) implement the **VirtualAlloc-commit-time** fix — mprotect RW when the guest commits a
  sub-range of these reserved regions, so the memcpy never faults (also removes the demote-to-interpret slowdown);
  (2) probe the final 0x3e0 fault page (reserved-tail my-fix-missed vs the unreserved gap) to clear the last crash;
  (3) re-test the ladder (DIRECT_MEM past 0x87efdef9430 → floor-guard → unit → present); snapshot only on a real pixel.

**Rails:** existing trace on the restored floor `.so` (no rebuild, no code change this turn); floor intact; all fixes
uncommitted; no commit (no pixel); scoped `wineserver -k`; 45MB trace log cleaned (disk 50GB).
