# LANE HK-MEMORY — progress journal

12:34 · lane start · measuring 48GB attribution, 1121 regions, refused bases · exploring macrunner_hb + hyperbridge memory paths
12:37 · BACKEND FAILOVER · `kimi` retired after 2 consecutive returns under 180s with no journal line; continuing on `claude`.
12:52 · code-side attributions found: 128 MiB `MAP_JIT` arena per runtime (`hb_jit.c:82`), 40 MiB inline
  `hb_block_cache_t` (`hb_runtime.h:69`), per-thread runtime pool with no thread-exit teardown
  (`macrunner_hb.c:12675/12692`), guest `VirtualAlloc` → `NtAllocateVirtualMemory` (`macrunner_hb.c:29947`).
13:05 · GROUND TRUTH · a live HK process (PID 75428, another lane's run) was observed **read-only** with
  `vmmap` — never started, signalled or stopped. 10,281 regions parsed. This turned every estimate into a
  measurement. `grep -c translation-cache-open run.log` = 115 == the 115 128-MiB `rwx` regions in vmmap: 1:1.
13:20 · size-class oracle written (`tools/mem_sizeclass_probe.c`) — proves `sizeof(hb_block_cache_t)`
  41,943,088 → `malloc_good_size` 41,959,424 == the observed MALLOC_LARGE size ×112, and that a 2,944 B
  IR-block array packs 1,365-per-4-MiB-magazine. (First build was wrong: clang -O2 deleted the dead
  mallocs and it reported 81 MB for 700 MB of requests; fixed by making the pointers escape.)
13:35 · REPORT · `reports/research/HK-MEMORY-48GB-ATTRIBUTION-20260728.md`

## Answers

1. **48 GB attributed** (non-coalesced vmmap, GiB): malloc-small zone 14.79 / HB JIT arenas 14.38 (115 ×
   128 MiB, **1.3 % resident**) / VM_ALLOCATE misc 11.91 / **HB translation-cache `entries[]` 9.05** /
   HB block caches 4.38 (112 × 40 MiB) / reserved 4.00 / other 3.91.
   **Proven HB share: 27.8 GB virtual, 7.4 GB swap, 637 regions.** Swap is a different story from address
   space: malloc-small 13.76 GB (59 %) + cache arrays 7.04 GB (30 %) = 89 % of all 23.2 GB swapped.
2. **Region count** is not driven by VM_ALLOCATE — 3,775 of the regions are libmalloc's **4 MiB** small
   magazines holding ≈5.15 M live objects. The IR blocks are the one genuine arena candidate (they already
   share a lifetime with their `hb_ir_func_t`). The cache arrays and block caches are already single large
   allocations — their problem is being duplicated 115×, not being fragmented.
3. **The refused bases are our own furniture.** Of 197 refused `(base,size)` pairs ever logged, **152 (77 %)**
   land inside a region present in a later independent process and **16 land exactly on a region base** —
   including a 128 MiB JIT arena at `0x4d2150000+0.00` and a 72 MiB cache array at `0x57d400000+0.00`.
   Placement is **incidental**: `mmap(NULL,…)`/`calloc` and the guest's allocator draw from the same
   kernel cursor in the same address space. The refusal band (4.4–48.0 GB) is 63.8 % occupied with its
   15.81 GB of free space shattered into **1,684 holes**.
4. **`VirtualAlloc` placement is correct** — `MEM_RESERVE` is honoured, `anon_mmap_tryfixed` uses
   `mach_vm_map(VM_FLAGS_FIXED)` (never clobbers), we never return an unreserved base, and the absence of
   an aligned search on the placed path **matches Windows**. The gap is that our address space contains
   27.8 GB of emulator structures Windows would never have, so the guest sees phantom conflicts.
5. **Cheapest wins** — two are env vars the shipping code already parses:
   `MACRUNNER_HB_TRANSLATION_CACHE=0` → −9.05 GB, −7.04 GB swap (the on-disk file is **16 bytes**: the
   cache is built 115× and persisted zero times); `MACRUNNER_HB_JIT_BUFFER_SIZE=0x2000000` → −10.8 GB;
   then `instr_cap` 16→4 in `hb_ir.c:37`, then `HB_BLOCK_CACHE_SIZE` 524288→65536.

## Corrections to the brief's premises

- **`find_region_normalized` is O(log n), not linear** (`hb_memory.c:1598`, treap + MRU cache). Its ~5 %
  profile share is call frequency, not list length; shrinking the region list will not help it. The real
  superlinear code is `hb_memory_sync_live_range` (`hb_memory.c:718-731`), **O(N²)**, reached from every
  guest `VirtualAlloc` commit.
- **The refused bases do not climb.** Observed order in the live run: `0x11d520000 → 0x17fb00000 →
  0x4d2150000 → 0x16af30000 → 0x57d400000` — down, up, down, up. Independent placements, not a retry ladder.
- **"19 refusals and climbing" is a whole-run aggregate**, not a rate; the counter is uncapped by design.
  Runs that stop at 20 `translation-cache-open` show **0** refusals — our runtime count causes them — but
  a handful of refused 64 KiB chunks is not "cannot compile at any speed", and memory exhaustion was
  already refuted as the cause of the `newobj` NULL.
- Two comments in-tree are stale and understate current cost: the block cache is **80 B/entry (40 MiB)**,
  not "~56B => ~29MB", and the IR cache is **10.03 MiB**, not "4MB" — the 2026-07-27/28 SMC fields grew
  both, costing ~+1.4 GB across 115 runtimes with no note anywhere.

## Not established

Identity of the 14.8 GB small-zone occupant is by **size class**, not call site (`hb_ir_block_create`'s
fixed 16×184 B is the leading candidate; settle with `MallocStackLogging=1` + `vmmap -stacks`, or a counter
pair). Also open: why 410 cache arrays exist for 115 open caches; how many of the 115 runtimes belong to
dead threads.

LOOP-STATUS: GOAL
