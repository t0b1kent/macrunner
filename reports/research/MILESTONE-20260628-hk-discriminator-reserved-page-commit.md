# MILESTONE 2026-06-28 — Discriminator verdict: CASE 1/2 (reserved-but-uncommitted page), NOT OOB/corruption. Fix = commit reserved page on direct-mem write fault.

**Per operator's discriminator plan.** Region probe + host-mapping probe (added to the JIT signal-guard fault
report, env-free, restored after) give a DECISIVE verdict. No pixel → floor `.so` restored byte-exact; fault-recovery
fix + probe retained uncommitted. Converges with Spark (single over-write site).

## Discriminator data (HK, DIRECT_MEM=1, over-write fault at unityplayer 0x87efdef9430)
```
fault host insn = str x20,[x21]  (movdqa [rcx] WRITE; dmb-ishst before it)   [over-WRITE, from prior SANITY]
macrunner-hb-fault-region: fault=301000000 fault_reg=NONE
  dst_rcx=300ffffa0 dst_reg=YES d_base=300000000 d_end=301000000 d_perm=3(RW) d_alloc=0 d_spans_fault=0
  src_rdx=300ffff7c src_reg=YES s_base=300000000 s_end=301000000
macrunner-hb-fault-host:   fault=301000000 kr=0 host_region_base=301000000 host_region_end=308000000
                           prot=0x0(PROT_NONE) max_prot=0x7(RWX) covers_fault=1
```

## Verdict: CASE 1/2 — reserved-but-uncommitted page (NOT case 3 OOB, NOT direct-mem corruption)
- The dst pointer rcx is valid; the committed slab head [0x300000000,0x301000000) is RW; the HOST has the fault page
  inside a **reserved region [0x301000000, 0x308000000), prot=PROT_NONE, max_prot=RWX, covers_fault=1**.
- So the dst allocation's **reservation legitimately spans past the fault page** (committed head + reserved tail of
  one ~128MB Unity-allocator slab). The write into the reserved tail is **NOT** an over-run past the allocation
  (case 3) and **NOT** a corrupted length — it is a write into a reserved-but-uncommitted page of the same alloc.
- `hb`'s region table only registered the committed head (`d_end=0x301000000`, `fault_reg=NONE`) — an incomplete
  view; the actual reservation extends to 0x308000000.

## Why direct-mem faults where the floor/lazy path does not
`hb_memory_write` commits a reserved page on fault via `special_grow` + `goto grow_retry` (hb_memory.c:1174). The
**direct-mem JIT write emits a raw `str` to the identity address, bypassing `hb_memory_write` and its commit hook** →
it hits the raw PROT_NONE reserved page → SIGBUS. (The slow floor producer times out before reaching this write, so
the floor's clean rung-9 doesn't prove it handles this page — it just never gets there.)

## The fix (now precisely specified) — leverages the already-implemented fault-recovery
**Make `special_grow` commit reserved-committable pages.** When a guest write faults on a host page that
mach_vm_region reports as reserved (`prot==PROT_NONE`, `max_prot & WRITE`, region covers the addr), commit it
(mprotect to the writable max_prot subset) + register an `hb` region, then retry — exactly what the operator's plan
prescribed for case 1/2 ("a guest write straddling into an adjacent RESERVED page → map/commit it, don't fault").
Best placed in the macrunner_hb `special_grow` handler (macrunner_hb.c), because:
- the lazy path already calls `special_grow` (hb_memory.c:533, :1174), AND
- **my fault-recovery routes the direct-mem fault back through `hb_memory_write` → `special_grow`** (guard → demote
  block to interpret → `hb_jit_helper_exec_ir_block_once` → `hb_memory_write`). So enhancing `special_grow` makes the
  EXISTING fault-recovery resolve this case — no signal-guard restructure needed.

**Open correctness nuance (decide before shipping):** the host shows the page *reserved* (prot=NONE), not committed.
Either (i) the guest committed it and MacRunner's `VirtualAlloc(MEM_COMMIT)` left it PROT_NONE — a commit-coherence
bug, and committing-on-fault is the robust fix (and the real fix is the commit path); or (ii) the guest writes to
reserved-uncommitted memory (an AV on Windows too) — in which case auto-commit MASKS a guest bug. The slab shape
(committed head + RWX-max reserved tail, sequential allocator writes) strongly favors (i). Recommend: scope the
auto-commit to `max_prot & WRITE` reserved pages only (genuine unmapped pages still fault), AND add a one-line trace
when it fires, so a real AV isn't silently masked.

## Status / next
- Discriminator: **DONE — CASE 1/2 (reserved page), floor-reproducibility moot** (it's a legit reserved page, not
  corruption; the region+host probe is decisive without the impractical long floor run).
- Fix: **specified, not yet applied** (enhance `special_grow` to commit reserved-committable pages). Next: implement
  it → re-test in order (DIRECT_MEM=1 past 0x87efdef9430? → floor-guard rung9 → unit regression → first-publish→~6s +
  present truePresent>0 / non-black capture). Snapshot/commit only on a real pixel.
- Producer advance this turn: still `c000007b` at the over-write (fix not yet applied); no Present, no pixel.

**Rails:** probe added then floor `.so` restored byte-exact; fault-recovery fix + probe retained uncommitted; no
commit (no pixel); scoped `wineserver -k`; disk cleaned. Supersedes the OOB-suspicion in
MILESTONE-20260628-hk-wall-is-overwrite-not-overread.md — it is reserved-page (1/2), not OOB (3).
