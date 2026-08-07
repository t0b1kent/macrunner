# MILESTONE 2026-06-28 — SANITY result: the 0x87efdef9430 wall is an over-WRITE, not an over-read → fix (b) does NOT apply

**Per the operator's plan step 1 (SANITY first).** The conclusion flips the planned fix: this fault is an SSE
**over-WRITE** (destination crosses into an unmapped page), so the zero-fill read fix (b) cannot help it — it needs
the mapped-page class (a), exactly as the operator's caveat foresaw. No code changed this turn (diagnosis only);
floor `.so` intact; fault-recovery fix still uncommitted.

## Evidence (decisive)
Register state at the fault (from the prior payoff run's signal-guard dump):
```
rax=32023c018 rcx=320ffffa0 rdx=320ffff7c rbx=32023c130 rdi=320213740
fault=321000000  signal=10 (SIGBUS)  fault_align=0
host natinsn w[-2..0]= 910182b5  d5033abf  [f90002b4]
```
- `rdx` (src) and `rcx` (dst) and rax/rbx/rdi are all **valid 0x320-region guest-heap pointers — NOT garbage**
  (so it is NOT a bad-upstream-pointer bug at the immediate level).
- **The faulting host instruction `[f90002b4]` = `str x20,[x21]`** (verified: `emit_str_x(20,21,0)` =
  `0xf9000000|(21<<5)|20` = `0xf90002b4`), preceded by `d5033abf` = `DMB ISHST` (verified: `emit_dmb_ishst`). That
  is the **store** half of `emit_direct_mem128_store_from_x20_x22` — i.e. the `movdqa [rcx]` **WRITE**, not the
  `movdqu [rdx]` read.
- Geometry: the dst `rcx=0x320ffffa0` advances by 0x40/chunk; a chunk crosses the page boundary and the store hits
  **unmapped `0x321000000`** → SIGBUS. (The reads from `rdx=0x320ffff7c` stay below 0x321000000 on this iteration.)

⇒ **It is an over-WRITE.** The prior milestone's "movdqu over-read" framing was wrong for this site; the
fault-safe interpreter also faulting confirmed it's *genuine*, but it's the **store** that faults, not the load.

## Consequence for the fix
- **(b) zero-fill is INAPPLICABLE here** — you cannot zero-fill a write into an unmapped page; the bytes must land
  somewhere real. (b) remains a valid general fix for over-READ sites if/where they exist, but it will NOT get the
  producer past `0x87efdef9430`.
- This site needs the **(a)** class: a real mapped page for the over-write. The operator's caveat called this
  exactly ("if an over-write exists, add (a) for writes") and tied it to the Spark lane's over-read/over-write
  survey — this finding is the over-write the survey is looking for; fold it in.

## Open question that determines (a)'s correct form (needs one targeted probe / the Spark survey)
Is `0x321000000`:
1. **A mapping-sync gap** — Wine reserved/committed it but `hb_memory` didn't register it (the buffer legitimately
   spans into it). Fix: sync/register the page (mapping-completeness) — safe, correct.
2. **Reserved-but-uncommitted** — a Windows reserved page the guest writes to expecting auto-commit. Fix: commit on
   write-fault (extend the existing `special_grow` recovery to the direct-write fault path), or a guard/padding page
   after guest allocations.
3. **Genuinely OOB** — the memcpy writes past its destination buffer because an upstream **length/pointer is
   corrupted**. If so this is NOT a mapping fix at all — and it could be a **direct-mem correctness symptom**
   (direct-mem mis-stored an earlier value → corrupted a size/count → over-write). This would be a different,
   higher-priority bug.

**Critical discriminator (cheap):** does this over-write also occur on the FLOOR (DIRECT_MEM off) if the producer
runs long enough, or is it direct-mem-specific? If floor-too → genuine guest behavior → (a) mapping fix (case 1/2).
If direct-mem-only → suspect direct-mem corruption (case 3) → fix direct-mem, not mapping. The Spark survey +
a region probe at `0x321000000` (is it inside a Wine reservation? what's its commit state?) settles 1 vs 2 vs 3.

## Recommendation
Do not implement (b) for this fault (won't help) and do not blind-implement (a) until 1/2/3 is settled (its form
differs per case, and case 3 means a different fix entirely). Next: (i) fold in the Spark over-read/over-write
survey; (ii) probe `0x321000000`'s reservation/commit state + re-run the FLOOR longer to see if the over-write is
floor-reproducible or direct-mem-specific; (iii) then apply the matching (a) fix (map/commit) OR chase the
direct-mem corruption, and re-test (DIRECT_MEM=1 past 0x87efdef9430 → floor-guard → unit regression → present).

**Rails:** SANITY-first per plan; no code change this turn; floor `.so` intact; fault-recovery fix retained
uncommitted; snapshot/commit only on a real pixel (none yet). Supersedes the over-read framing in
MILESTONE-20260628-hk-faultrecovery-fix-and-newwall.md for this specific site.
