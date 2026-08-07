# RCA CORRECTION 2026-06-27 — HK "split-mapping / two host backings" root cause is REFUTED (probe-field misread)

**Method:** ultracode multi-agent burst (9 read-only agents: 5 facets → synthesis → 3 adversarial verifiers),
then **operator-side independent verification** of the load-bearing claim. 2 of 3 verifiers refuted the
"split mapping" synthesis; direct probe re-parse + source confirm the refutation. READ-ONLY; Codex HK lane untouched.

## Verdict: there is NO guest→host address-resolution split. Both threads resolve guest VA 0x34008a350 to the SAME host page.

The premise "producer host backing 0xb988046c0 vs consumer 0xaa6c08a80 (~3.78 GB apart)" is a **misread of the
probe's `mem=` field**, which is `ctx->memory` — the `hb_memory_t` **struct pointer**, NOT a data address
(`macrunner_hb.c:23166`, printed as `mem=%p` at `:23153`). The **actual** resolved host address is
`host_r`/`host_rw` = `hb_memory_host_ptr(ctx->memory, guest_addr, …)` (`macrunner_hb.c:23118`/`:23125`).

### Direct probe re-parse (all three runs the brief/verifiers cite)

| run | thread | tid | host_r / host_rw | region_base / region_host / alloc | `mem=` (ctx->memory struct ptr) |
|---|---|---|---|---|---|
| mapresolve-221542 | consumer | b | **0x34008a350 / 0x34008a350** | 0x340000000 / **0x0** / **0** | 0x71932e400 |
| mapresolve-221542 | producer | 4000 | **0x34008a350** (+0x300d60f20 = a 2nd item) | 0x340000000 / **0x0** / **0** | 0x71c3c1680 |
| hostseq-215005 | consumer | 7000 | **0x34008a350 / 0x34008a350** | — | 0xb988049c0 |
| hostseq-215005 | producer | 8000 | **0x34008a350** | — | 0xb988046c0 |
| hostaddr-213250 | consumer | 7000 | **0x34008a350 / 0x34008a350** | — | 0xaa6c08a80 |
| hostaddr-213250 | producer | c | **0x34008a350** | — | 0xaa6c08780 |

- **`host_r` = `host_rw` = `0x34008a350` for producer AND consumer in every run** (identity: host == guest).
  In mapresolve, both see the **same identity region**: `region_base=0x340000000, region_host=0x0 (host_base NULL),
  region_alloc=0`. Same `pid`. **No split mapping; no producer-private region.**
- The brief's "0xb988046c0 vs 0xaa6c08a80" pair is **cross-referenced from two different runs** (0xb988046c0 =
  producer `mem=` in *hostseq*; 0xaa6c08a80 = consumer `mem=` in *hostaddr*). **Within a single run** the two
  `mem=` structs differ by **0x300** (adjacent heap allocations), not 3.78 GB. The "3.78 GB mirror" is an artifact.

### Why all three A/B toggles were NEGATIVE — now explained
hot-cache, direct-mem, and store-fence all left the result unchanged **because every x64 region-creation path makes
an IDENTITY region** (`hb_memory_map(base!=0)` → `host_base=NULL` `hb_memory.c:681-685`; `hb_memory_sync_live_range`
→ `host_base=NULL` `:790-791`; `hb_memory_map_private` — the only `host_base!=NULL` allocator — has **no x64 caller**).
So every access route (identity memcpy, mach special-handler, direct-mem copy) resolves `0x34008a350` to the **one
real Wine page**. Nothing routes the producer's store to a different page → no toggle can change a split that doesn't exist.

## The "permanent 572× invisibility" is NOT demonstrated by these logs (temporal misalignment)

In every run the **consumer's entire sampling window precedes the producer's first write by ~30 s**, non-overlapping:
- mapresolve: consumer `now_us 1782562936454979..554307` (99 ms wide); producer `1782562967191046..191316` → **+30.6 s later**.
- hostseq: consumer `…450561061..626610`; producer `…480584108..584394` → **+30.0 s later**.
- In all runs **consumer `ready_seq` is stuck at 0..0** (never observed the producer's sequence), while the producer's
  `ready_seq` advances `0..606/639` and its own `host_value` goes `0x0→0x1` (the write lands, producer-side).

So the consumer logged `host_value=0` because it sampled **before the producer ever produced** (and then hit its
per-thread line budget: consumer 511-543 lines vs producer 6 lines). **There is ZERO captured consumer read AFTER the
producer's store.** Genuine cross-thread invisibility is *unproven* by these probes.

## What stands / falls

- **CONFIRMED (structural):** each guest x64 thread runs on its own `hb_memory_t` (`ctx->memory = hb_memory_create(0)`,
  `macrunner_hb.c:23119`; no x64 singleton — only guest32 has one, `:380`). Real, but **causally inert** here (both
  instances classify the item identically as identity → same page). The lockless region tree
  (`find_region_normalized`/`tree_insert`, `hb_memory.c`) is a real latent hardening item, but a race gives transient
  wrong lookups, not a stable seconds-long split — not this bug.
- **CONFIRMED (symptom):** consumer reads `esi=0` at the gate, loops, never presents (CrossOver oracle showed `esi=1`
  on the correct side). The *symptom* is real; the *mechanism* is what's refuted.
- **REFUTED:** guest→host split mapping / two host backings / producer-private region / "force-identity" or
  "shared-singleton" unify fix (the unify fix is a **no-op** — x64 is already identity).
- **REFUTED:** ARM64X EC/native dual-`.data` (UnityPlayer is plain x86-64 JIT; the item is a >4 GB heap object, not PE `.data`).
- **UNRESOLVED (the actual question):** why the consumer never observes the produced frame.

## Redirected next steps (read-only instrumentable — for Codex/Lane A)

1. **Time-align the probe.** Capture a consumer sample **strictly after** the producer's `ready_seq` advances
   (raise/stagger per-thread line budgets so both threads log the same wall-clock window). Only a *post-store
   consumer-still-0* read proves real invisibility. Today's logs can't.
2. **Verify "same item" identity (rule out ABA / slot reuse).** Instrument the item **base + generation/refcount**,
   not just VA+0x40. The producer's pool VA `0x34008a350` may be reused for a *different logical item* between the
   consumer's early window and the producer's 30 s-later writes.
3. **If same-address invisibility is then confirmed**, the mechanism is at the same page, so investigate:
   (a) **store codegen** of `mov [item+0x40],1` @rva 0x586da3 — does it actually reach `hb_memory_write` for that VA,
   or get elided/folded/written to a different effective address? (b) **write-side direct-mem path** —
   `MACRUNNER_HB_DIRECT_MEM_WRITE` (`macrunner_hb.c:5666`) is a **separate** gate that falls back to the master
   `MACRUNNER_HB_DIRECT_MEM` (default-OFF, `:5648/5642`); the "DIRECT_MEM=0 no change" A/B may have been a **no-op**
   (already off) — confirm what the live HK run env actually sets, and A/B `MACRUNNER_HB_DIRECT_MEM_WRITE` explicitly.

## Note on the unify-fix guards (moot now, but recorded)
If a `hb_memory_host_ptr`/read/write change is ever made, the fix-safety verifier enumerated the guards that MUST be
preserved (else it regresses guest32/SMC): (1) `guest32_base==NULL` AND addr outside `[guest32_base,+HB_GUEST32_SIZE)`;
(2) `!region->is_guest32`; (3) `!region->allocated`; (4) the EXEC-write SMC guard + `bump_generation`
(`hb_memory.c:1365/1201`). The guest32 mirror (`macrunner_hb.c:380`) and EC/native `.data` split are NOT at risk
(different objects/layers). But no such fix is warranted unless step 3 confirms a same-address mechanism.

**Rails:** READ-ONLY; no edits/build/run; Codex HK lane + golden + tree untouched. Supersedes the "split-mapping"
framing in the present-stall thread; corrects the `mem=` interpretation in the host-addr probe brief.
