# MILESTONE 2026-06-28 — commit-on-fault fix: SAFE + 260× producer progress, but no frame yet + a 3.5 GB slab-write span to audit

**Implemented the scoped+traced reserved-page commit-on-fault, as greenlit.** It is correct and safe, advances the
producer ~260×, but does NOT reach the first frame in 120 s, and the audit log reveals a large (3.5 GB) sequential
slab-write that needs a legit-vs-runaway call. Per discipline: no pixel → floor `.so` restored byte-exact; fixes
retained uncommitted.

## The fix
`macrunner_hb_try_grow_guard_page` (macrunner_hb.c): after Wine's `virtual_handle_fault` fails, if `mach_vm_region`
shows the page **host-RESERVED (prot==VM_PROT_NONE) AND max_prot & WRITE AND region covers the addr**, `mprotect` it
to **RW** (never EXEC) and return TRUE. Strictly scoped (genuinely-unmapped or non-writable-max pages still fault) and
**logs every fire** (`macrunner-hb-reserved-commit: addr… region… max_prot… n=`). Central place: both `special_write`
(hb_memory.c:1174 path) and `special_grow` route through it, so my landed fault-recovery
(guard→demote→interpret→hb_memory_write→special_write→try_grow_guard_page) drives it — no guard restructure.

## Regression ladder — SAFE
- **Floor-guard (HK, DIRECT_MEM off):** rung 9 / PRESENT_MISSING / VALID_RUN, **reserved-commit fires = 0** → the fix
  is neutral on the floor (the slow floor producer times out before reaching the slab memcpys).
- **Unit suite:** 450 passed / 22 failed — within the proven flaky band (20–22); the commit-fix is Wine-side, outside
  unit-test scope; no new deterministic failure.

## Payoff run (HK, DIRECT_MEM=1 + fix) — huge advance, but no frame
- **reserved-commit fired 265×** — the fix works, committing reserved pages on the over-write path.
- Producer ran from **2,368,086 → ~621,000,000 steps (≈260× further)**; the over-write block `0x87efdef9430` advanced
  from the 0x300 slab to **rcx=0x3e0ffffa0 — a slab ~3.5 GB higher** in the heap. Reached **rung 9 (dxgi-factory)**
  (past Mono — so real forward progress, not a tight loop).
- **Still `c000007b`** at `0x87efdef9430` on the last slab (the over-write crossed into a page the fix did NOT commit),
  and **no Present** (no truePresent, rung 9). ALL_TRIES_FLAKED.

## The audit you asked for (legit slab-init vs masked runaway) — UNRESOLVED, leans "large init, incomplete"
265 commits across ~224 sequential 16 MB slabs (3.5 GB), with the over-write hitting each slab's tail at the same
offset (0x…ffffa0). Two readings:
- **Legit-but-incomplete:** Unity initializes a large memory pool slab-by-slab; the fix commits the boundary pages;
  the producer genuinely advanced (Mono→dxgi-factory). The final fault = the over-write past the LAST slab / pool end
  (a genuine boundary, or a committable page my scope missed). Forward progress to rung 9 supports this.
- **Masked runaway (your "don't mask a bug" caveat):** a single over-write with a corrupted/unbounded length runs
  across 3.5 GB; commit-on-fault feeds it page after page instead of faulting. The 3.5 GB single-block span is the
  red flag. (The reserved-page discriminator was right about the FIRST page's geometry, but the *extent* is the open
  question.)
The reached-rung-9 forward progress tips toward "large legit init that didn't finish in 120 s," but it is NOT proven;
the 3.5 GB extent must be explained before trusting/shipping the fix.

## Decision (discipline)
No pixel → **floor `.so` restored byte-exact**; commit-on-fault + fault-recovery + probe **retained uncommitted**.
Do NOT ship/default-on yet — the audit is unresolved and the run still `c000007b`s.

## Next (precise)
1. **Resolve the audit (decisive):** at the FINAL fault, probe whether `0x3e1000000` is another committable reserved
   page (fix scope miss → extend) OR genuinely beyond a real allocation (→ it's an over-write past the pool = bounded
   per-slab copy hitting the pool end, legit-ish) — AND instrument the memcpy's length/loop-count at `0x87efdef9430`:
   bounded per call (≈16 MB → legit slab-by-slab) vs one unbounded 3.5 GB write (→ runaway/corrupted length).
2. If runaway → trace the corrupted length/count upstream (the real bug; possibly the original DIRECT_MEM-corruption
   hypothesis after all — revisit with the length now in hand).
3. If legit slab-init → a longer run (or warm cache) to see if it presents; AND the proper root fix is
   **VirtualAlloc-commit-time** (`mprotect` RW when the guest commits, not at fault time) — file as the follow-up you
   noted; commit-on-fault becomes a safety net, not the primary fix.
4. Re-test order unchanged once the audit clears (DIRECT_MEM past 0x87efdef9430 → floor-guard → unit → present),
   snapshot only on a real pixel.

**Rails:** scoped+traced fix; regression beyond HK (floor-guard + unit) ✓ safe; floor `.so` restored byte-exact;
fixes uncommitted; no commit (no pixel); scoped `wineserver -k`; disk cleaned (50 GB free).
