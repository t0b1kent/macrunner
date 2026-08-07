# Why one game maps 48 GB and swaps 22 GB — attribution with numbers

Lane: HK-MEMORY (offline, read-only). Date: 2026-07-28.
Measured against a **live** Hollow Knight process, PID 75428, launched 12:32, sampled 12:45–12:51,
run dir `reports/phase4-hollow-knight/laneA-INPUT-TEST-manual-language-FAST4of5-LONG-123136/`.
That process belongs to another lane; this lane only *observed* it (`vmmap`), never started,
signalled or stopped anything.

Commands that produced every number in this report:

```
ps -Ao pid,%cpu,etime,rss,vsz,args | grep -iE 'game-hollow|wineserver|mr-run'
vmmap -summary 75428                       > vmmap-summary-75428.txt
vmmap -w -noCoalesce -interleaved 75428    > vmmap-nc-75428.txt      # 10,281 regions parsed
grep -c 'translation-cache-open' <rundir>/run.log        # 115
grep -h 'guest-alloc-fail' reports/phase4-hollow-knight/*/run.log    # 197 distinct (base,size)
ps -M 75428 | wc -l                        # 57  -> 56 threads
cc -I engine/hyperbridge/include probe.c   # exact sizeof() of every struct below
cc -O2 -o mem_sizeclass_probe tools/mem_sizeclass_probe.c && ./mem_sizeclass_probe
```

Raw captures are in the session scratchpad; the size-class oracle is committed as
`tools/mem_sizeclass_probe.c` so any of these identities can be re-derived offline.

---

## 0. Headline

The brief's framing — "48 GB of writable space, 22 GB to swap" — is confirmed, but the
**address-space problem and the swap problem are two different bugs with two different owners.**

| | virtual | swapped | owner |
|---|---|---|---|
| Address space (what breaks the guest's placed `VirtualAlloc`) | 14.4 GB JIT arenas + 4.4 GB block caches | ~0.4 GB | HyperBridge, **per-runtime**, 1.3 % utilised |
| Swap (what costs real money) | 14.8 GB malloc-small + 9.1 GB translation-cache arrays | **20.8 GB of 23.2 GB** | HyperBridge, **per-runtime duplication** |

The single fact underneath both: **115 JIT runtimes are alive at once**, each carrying a
private 128 MiB arena, a private 40 MiB block cache, and a private translation-entry array that
grows to as much as 72 MiB. `grep -c translation-cache-open run.log` = **115**;
`vmmap` shows exactly **115** 128 MiB `rwx/rwx` regions. One-to-one.

---

## 1. Where the 48 GB comes from — ranked

From `vmmap -w -noCoalesce` (10,281 regions), classified by exact byte size against
`sizeof()` measured from the real headers. Sizes are GiB.

| # | bucket | regions | virtual | resident | **swapped** | % of swap |
|---|---|---:|---:|---:|---:|---:|
| 1 | malloc SMALL zone (1 KiB–127 KiB objects) | 3,815 | **14.79** | 1.00 | **13.76** | 59.3 % |
| 2 | **HB JIT arena** — 128 MiB `MAP_JIT` | 115 | **14.38** | 0.19 | 0.21 | 0.9 % |
| 3 | VM_ALLOCATE other (guest / Wine / misc) | 1,356 | 11.91 | 0.41 | 0.69 | 3.0 % |
| 4 | **HB translation-cache `entries[]`** | 410 | **9.05** | 1.09 | **7.04** | 30.3 % |
| 5 | **HB `hb_block_cache_t`** — 40 MiB calloc | 112 | **4.38** | 0.46 | 0.19 | 0.8 % |
| 6 | VM_ALLOCATE (reserved, `---/rwx`) | 39 | 4.00 | 0 | 0 | 0 |
| 7 | MALLOC_LARGE other | 122 | 1.55 | 0.36 | 1.17 | 5.1 % |
| | everything else (`__TEXT`, dyld, Metal, stacks…) | 4,312 | 2.36 | 1.1 | 0.09 | 0.4 % |
| | **total parsed** | **10,281** | **62.42** | | **23.20** | |

`vmmap -summary` independently reports `Writable regions: Total=48.0G written=23.5G(49%)
resident=3.3G(7%) swapped_out=23.2G(48%)` — the 62.42 G above includes non-writable
`__TEXT`/`__LINKEDIT` and the 4 GB reserved band.

**Proven HyperBridge share: 27.8 GB of virtual address space, 7.4 GB of swap, in 637 regions.**
Add bucket 1 (see §1.4) and it is ~42.6 GB virtual and ~21.2 GB of the 23.2 GB swap.

### 1.1 JIT arenas — 14.38 GB, and 98.7 % of it is never touched

```c
/* engine/hyperbridge/src/hb_jit.c:73-82 */
hb_jit_buffer_t* hb_jit_buffer_create(size_t size) {
    ...
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef __APPLE__
    flags |= MAP_JIT;
#endif
    buf->writable = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
```

```c
/* engine/hyperbridge/src/hb_runtime.c:1682 */
size_t jit_size = 128u * 1024u * 1024u;
```

115 live × 128 MiB = **14.38 GB**. Measured resident across all 115: **0.19 GB — 1.3 %**,
i.e. a mean of **1.7 MiB actually used per 128 MiB arena**. The arena is a bump allocator that
`hb_jit_runtime_reset` rewinds to offset 0 on every callback (`hb_jit.c:117-119`), so the
high-water mark per callback is small; the 128 MiB is pure reservation.

Why 115 of them: they are per-thread and pooled, and **nothing ever frees them**.

```c
/* engine/wine/dlls/ntdll/unix/macrunner_hb.c:12675, 12692-12694 */
static __thread hb_jit_runtime_t *macrunner_hb_tls_jit_rt = NULL;
#define MACRUNNER_HB_NESTED_RT_POOL_MAX 2
static __thread hb_jit_runtime_t *macrunner_hb_tls_nested_rt[MACRUNNER_HB_NESTED_RT_POOL_MAX];
```

Up to 3 runtimes per thread (1 outer + 2 nested-pool). 56 threads × up-to-3 ⇒ ≤168; observed
115. These are plain `__thread` pointers with **no thread-exit destructor** — searched
`macrunner_hb.c` for every use of the three symbols (`grep -n 'macrunner_hb_tls_jit_rt\|
macrunner_hb_tls_ir_cache\|macrunner_hb_tls_nested_rt'`): 12 hits, none of them a teardown on
thread exit. A thread that runs one x64 callback and dies leaves its 128 MiB arena + 40 MiB
block cache mapped for the life of the process. **[HYPOTHESIS]** that dead-thread retention is
a material share of the 115; proving it needs a create/destroy counter pair, which does not
exist today.

### 1.2 Translation-cache `entries[]` — 9.05 GB, 7.04 GB swapped, and it is 115 copies of the same thing

Every `hb_jit_runtime_create` opens its **own private** cache:

```c
/* engine/hyperbridge/src/hb_runtime.c:1708 */
rt->persistent_cache = hb_cache_open(cache_root && *cache_root ? cache_root : NULL, &options);
```

```c
/* engine/hyperbridge/src/hb_aot_cache.c:183-186   (hb_cache_put has the identical shape at :404) */
if (count >= cap) {
    cap *= 2;
    hb_disk_entry_t* n = realloc(entries, cap * sizeof(*entries));
```

`sizeof(hb_disk_entry_t)` = **72 B** (measured). Every observed size in this family is an
**exact** multiple of 72:

| region bytes | ÷72 = entries | count |
|---:|---:|---:|
| 2,359,296 | 32,768 | 173 |
| 4,718,592 | 65,536 | 32 |
| 9,437,184 | 131,072 | 36 |
| 18,874,368 | 262,144 | 28 |
| 37,748,736 | 524,288 | 27 |
| 73,138,176 | 1,015,808 | 75 |
| 75,497,472 | 1,048,576 | 18 |
| (+5 more) | | 21 |

410 arrays, **9.05 GB**, of which **7.04 GB is swapped out — 30 % of all swap in the process.**
That is the classic profile of write-once-read-rarely memory: filled during translation, then
untouched, so the pager evicts it.

Two things make this pure waste:

* **The on-disk file is 16 bytes.** `ls -la engine/hyperbridge/build/hyperbridge-cache/` →
  `translation-cache.bin  16` (header only). `write_entries_atomic` runs only from
  `hb_cache_close` (`hb_aot_cache.c`), and the runtimes are never destroyed, so **nothing
  is ever persisted**. The 9 GB buys no warm start.
* **All 115 caches point at the same path** (`MACRUNNER_HB_TRANSLATION_CACHE_ROOT`,
  `scripts/mr-run.sh:109`) and hold independent copies of overlapping content. The struct
  comment concedes the design constraint: *"Single-thread-per-cache (same posture as the
  unlocked stats)"* (`hb_aot_cache.c:60`).

There are 410 live arrays against 115 open caches. **[HYPOTHESIS]** the excess is
`realloc`-growth churn where libmalloc's large allocator does not return the predecessor
region promptly (`MALLOC_LARGE (empty)` shows 37 regions / 0.38 GB, so some is returned). Not
proven; a `hb_cache_open`/`hb_cache_close` counter pair would settle it.

### 1.3 Block caches — 4.38 GB, and the 2026-07-27 SMC fields silently grew it 43 %

```c
/* engine/hyperbridge/include/hb_runtime.h:66-70 */
#define HB_BLOCK_CACHE_SIZE 524288
typedef struct {
    hb_block_cache_entry_t entries[HB_BLOCK_CACHE_SIZE];   /* inline, not a pointer */
```
```c
/* engine/hyperbridge/src/hb_runtime.c:286-288 */
static hb_block_cache_t* block_cache_create(void) {
    return calloc(1, sizeof(hb_block_cache_t));
}
```

Measured `sizeof(hb_block_cache_t)` = **41,943,088 B**. `malloc_good_size(41943088)` =
**41,959,424** (`tools/mem_sizeclass_probe.c`) — which is **exactly** the size of the
MALLOC_LARGE region observed **112** times. Identity proven, not inferred.

The in-tree comment still says *"~56B/entry => ~29MB virtual/thread"*. That is stale:
`sizeof(hb_block_cache_entry_t)` is **80 B** today, because the SMC re-verify fields
(`smc_span_start`, `smc_hash`, `smc_span_len`, added 2026-07-27, `hb_runtime.h:44-50`) pushed
56 → 80. Per runtime that is **28 MiB → 40 MiB**; across 115 runtimes the SMC fix cost
**+1.4 GB** of address space that nobody has been told about. The sibling change in the IR
cache did the same thing: `struct macrunner_hb_ir_cache_entry` went 16 B → 40 B on 2026-07-28
(`macrunner_hb.c:253-265`), taking that cache from 4.00 MiB to 10.03 MiB per thread — the
comment at `macrunner_hb.c:12670` still says "4MB ir_cache".

### 1.4 The malloc small zone — 14.79 GB, 13.76 GB swapped: the biggest item, and the least certain

3,775 regions of exactly 4,194,304 B. That is macOS libmalloc's small-magazine granularity, and
it holds objects of **1,009 B – 127 KiB**. This bucket alone is **59 % of all swap in the process**.

The probe measures the packing density directly: 200,000 allocations of 2,944 B produced
**640.0 MB of MALLOC_SMALL across 160 regions** → **1,365 live objects per 4 MiB magazine**.
Applied to the live process: **≈ 5.15 million live objects** of that size class.

The leading candidate is the IR block instruction array:

```c
/* engine/hyperbridge/src/hb_ir.c:31-38 */
hb_ir_block_t* hb_ir_block_create(uint64_t id, uint64_t guest_addr) {
    hb_ir_block_t* block = calloc(1, sizeof(hb_ir_block_t));
    ...
    block->instr_cap = 16;
    block->instrs = calloc(block->instr_cap, sizeof(hb_ir_instr_t));
```

`sizeof(hb_ir_instr_t)` = **184 B** (measured; it is three ~48 B `hb_ir_operand_t` unions plus a
header). So **every basic block costs 16 × 184 = 2,944 B up front regardless of how many
instructions it really has**, and `malloc_good_size(2944)` = **3,072 B**, landing in exactly the
observed size class. Blocks are then *cloned* into the block cache and owned by it:

```c
/* engine/hyperbridge/src/hb_runtime.c:319-339 */
static hb_ir_block_t* block_clone_for_cache(const hb_ir_block_t* block) {
    copy = hb_ir_block_create(block->id, block->guest_addr);      /* another 2,944 B */
```

115 block caches × 524,288 slots = 60.3 M slots; 5.15 M live clones is **8.5 % occupancy**,
which is entirely plausible.

**[HYPOTHESIS] — this attribution is by size class, not by call site.** The size class is
proven identical and the volume is consistent, but `vmmap` cannot name the allocator without
`MallocStackLogging`, which must be set at launch and therefore cannot be retrofitted onto a
running process. The competing candidate is per-entry `native_code` blobs from
`hb_cache_store`; the header comment puts those at ~377 B average, which is the **tiny** class
(`MALLOC_TINY` is 4 MiB total in this process — one region), so they cannot account for 14.8 GB.
That asymmetry is why `hb_ir_block_create` is the leading candidate rather than a coin flip.
**To settle it:** relaunch once with `MallocStackLogging=1` and take `vmmap -stacks`, or add a
two-counter `__atomic` pair around `hb_ir_block_create`/`hb_ir_block_destroy`.

---

## 2. Why 1,121 regions instead of a few large ones

The brief's "1,121 VM_ALLOCATE regions" reproduces as 1,069 coalesced / 1,471 non-coalesced.
But region *count* is not driven by `VM_ALLOCATE` at all — the writable map has **10,281
regions**, and the top contributors are:

| source | regions | why it is many pieces |
|---|---:|---|
| malloc small magazines | 3,775 | libmalloc allocates the small zone in **4 MiB** units; ~5.1 M objects need ~3.8 k of them |
| `VM_ALLOCATE` misc | 1,356 | Wine views, guest commits, thread bookkeeping — mostly 16 KiB–64 KiB |
| MALLOC_LARGE | 600 | one region per allocation ≥ 128 KiB: 112 block caches + 410 cache arrays + 78 others |
| dyld `__DATA`/`__AUTH`/`__DATA_CONST` | 2,026 | one per loaded image — unavoidable |
| HB JIT arenas | 115 | one `mmap` each |

**Could they be arenas?** Yes, for the two that matter:

* The 410 translation-cache arrays and 112 block caches are *already* single large allocations —
  the fix there is not arena-ing, it is **not making 115 copies**.
* The ~5.1 M IR-block allocations are the genuine small-piece offender. They have a natural
  arena: every block in a lifted function has the same lifetime as the function
  (`hb_ir_cfg_destroy` frees them all in a loop, `hb_ir.c:66-72`). A bump allocator per
  `hb_ir_func_t` would replace ~5 M mallocs with a few thousand, and would collapse thousands of
  4 MiB magazines. This is a real refactor in the engine lane's file, not a knob.

Fragmentation is what the guest actually trips over. Across the band where refusals occur
(0x1185f0000–0xc01400000, i.e. **4.4 GB – 48.0 GB**): **63.8 % occupied**, 15.81 GB free — but
that free space is shattered into **1,684 separate holes**. A 64 KiB placed request has plenty
of total room and still loses.

---

## 3. What actually occupies `0x672f70000` / `0x6bb800000` / `0x6cb370000`

Those three bases were refused in run `laneA-smcrelift-ab-35A-try1-115629` (n=17,18,19).
In the *currently live* process they happen to be free — but that is not the interesting answer,
because the layout is reproducible enough to test statistically.

Taking **all 197 distinct refused `(base,size)` pairs** ever logged across
`reports/phase4-hollow-knight/*/run.log` and testing them against this process's region map:

* **152 of 197 (77 %) land inside a region that exists in this later, independent process.**
* **16 land exactly on a region *base*** — `VM_ALLOCATE` ×8 (the 128 MiB JIT arenas),
  `MALLOC_LARGE` ×4, `mapped file` ×2, `shared memory` ×1, `Stack` ×1.

Two concrete hits, resolved exactly:

```
0x4d2150000  IN  VM_ALLOCATE   0x4d2150000-0x4da150000  128.00 MiB  rwx/rwx  off=+0.00 MiB
0x57d400000  IN  MALLOC_LARGE  0x57d400000-0x581c00000   72.00 MiB  rw-/rwx  off=+0.00 MiB
```

The first is a **HyperBridge 128 MiB `MAP_JIT` arena**. The second is a **72 MiB
translation-cache `entries[]` array at cap 1,048,576**. In both cases the guest asked for
`+0.00 MiB` — the *exact base* the host allocator had handed us.

**The placement is incidental, and that is the bug.** Nothing places our structures there
deliberately: `hb_jit_buffer_create` passes `mmap(NULL, …)` and `calloc` picks its own address.
The guest's Windows address space and the host's C allocator are **the same address space**, and
both are being fed from the same kernel free-space cursor. So the address Mono computes as
"free, I'll take it" is precisely the address `mmap`/`malloc` is about to return to *us*.

Corroborating shape from the logs: every refused base is 64 KiB-aligned, size `0x10000` (twice
`0x20000`), `type=0x3000` (`MEM_COMMIT|MEM_RESERVE`), `protect=0x40` (`PAGE_EXECUTE_READWRITE`)
— a JIT code chunk. **[HYPOTHESIS]** the specific mechanism is Mono's aligned-allocation dance
(`VirtualAlloc(NULL, size+align)` → `VirtualFree` → `VirtualAlloc(aligned_base, …)`), where a
host allocation lands in the just-freed hole before the placed retry. This fits every observable
— 64 KiB alignment, exact-base collisions, scattered rather than climbing bases — but it is
inferred from the allocation shape, not read out of Mono's code, which is not in this tree.

**Correcting the brief on two points:**

1. *"The bases climb, so Mono is retrying upward."* They do not climb. In
   `laneA-INPUT-TEST-manual-language-FAST4of5-LONG-123136` the sequence is
   `0x11d520000 → 0x17fb00000 → 0x4d2150000 → 0x16af30000 → 0x57d400000` — down, up, down, up.
   These are independent placement attempts scattered across the space, not a retry ladder.
2. *"19 refusals and climbing… the game cannot compile the methods the opening sequence needs."*
   The counter is aggregate and uncapped (`macrunner_hb.c:997-999`), so 19 is **19 for the whole
   run**, and the live run stands at 10. The correlation that *is* real: runs that reach 115
   `translation-cache-open` show 13–19 refusals; runs that stop at 20 opens
   (`…FASTJIT-084929`, `…BISECT-nostack-095434`, `…FASTJIT-BARRIER-092635`) show **0**. Our
   runtime count causes the refusals. But a handful of refused 64 KiB code chunks is not
   plausibly "cannot compile at any speed" — and the pre-registered instrument already
   **refuted** memory exhaustion as the cause of the `newobj` NULL (see
   `project_hk_newobj_null_alloc_20260727`). Treat the refusals as a real defect worth fixing,
   not as the established cause of the boot failure.

---

## 4. Does our `VirtualAlloc` honour placement the way Windows does?

Yes for the guest-visible contract; the gap is one layer down.

The import goes straight to the NT call — there is no HyperBridge allocator in the path:

```c
/* engine/wine/dlls/ntdll/unix/macrunner_hb.c:29947 */
status = NtAllocateVirtualMemory( process, &base, 0, &size, type, protect );
```

Wine's placed path:

```c
/* engine/wine/dlls/ntdll/unix/virtual.c:2679-2688  (map_view) */
if (base)
{
    ...
    if ((status = map_fixed_area( base, size, unix_prot ))) return status;
    ptr = base;
}
```
```c
/* engine/wine/dlls/ntdll/unix/virtual.c:2544-2545, 2581  (map_fixed_area) */
if ((UINT_PTR)base & host_page_mask)   return STATUS_CONFLICTING_ADDRESSES;
if (find_view_range( base, size ))     return STATUS_CONFLICTING_ADDRESSES;
...
else if (errno == EEXIST) status = STATUS_CONFLICTING_ADDRESSES;
```

Point by point against the brief's questions:

* **`MEM_RESERVE` handling** — correct. `map_fixed_area` reserves through
  `anon_mmap_tryfixed`, which on macOS uses `mach_vm_map(..., VM_FLAGS_FIXED, ...)` and maps
  `KERN_NO_SPACE → EEXIST` (`virtual.c`, `anon_mmap_tryfixed`). It does **not** use bare
  `MAP_FIXED`, so it never silently clobbers an existing mapping. This is the safe behaviour and
  it is being done right.
* **Do we ever return a base we did not reserve?** No. `map_view` sets `ptr = base` only after
  `map_fixed_area` returns success, and `*ret` in the import shim is written only on
  `!status` (`macrunner_hb.c:29974`). On failure the printed `base` is the caller's request,
  unchanged.
* **Is there an aligned-region search?** Yes, but only on the `base == NULL` path
  (`map_reserved_area` → `map_free_area` → `anon_mmap_alloc` + `unmap_extra_space`,
  `virtual.c:2704-2733`). The placed path deliberately has none — **and that matches Windows**,
  where a placed `MEM_RESERVE` at a taken base fails and the caller is expected to retry.
  Adding a search here would be wrong: it would return an address the caller did not ask for.

**So the semantic gap is not in the retry logic — it is that our address space contains
allocations Windows would never have.** On Windows the only thing that can conflict with a
guest placed allocation is another guest allocation. Here, the same 64-bit space also holds
14.4 GB of JIT arenas, 9.1 GB of cache arrays, 4.4 GB of block caches and 14.8 GB of malloc
magazines — 27.8 GB of proven emulator structures scattered across the very band Mono places
into. The guest experiences phantom conflicts. **The fix is not in `virtual.c`; it is to stop
putting 42 GB of our own furniture in the guest's living room** (§5), and — as a durable
second step — to segregate host allocations into an address band the guest never places into.

---

## 5. Cheapest wins, ranked by GB saved vs risk

Two of the top three are **environment variables already parsed by the shipping code**, so they
need no engine-lane edit and are reversible per run.

| # | change | saves (virtual) | saves (swap) | risk |
|---|---|---:|---:|---|
| 1 | `MACRUNNER_HB_TRANSLATION_CACHE=0` | **9.05 GB** | **7.04 GB** (30 % of all swap) | **very low** |
| 2 | `MACRUNNER_HB_JIT_BUFFER_SIZE=0x2000000` (32 MiB) | **10.8 GB** | ~0.15 GB | low–medium |
| 3 | `hb_ir_block_create` `instr_cap` 16 → 4 | est. **8–11 GB** | est. **8–10 GB** | medium |
| 4 | `HB_BLOCK_CACHE_SIZE` 524288 → 65536 | **3.83 GB** | ~0.17 GB | medium (couple with #2) |
| 5 | one shared translation cache instead of 115 | (subsumes #1, keeps the benefit) | | high |
| 6 | free the pooled runtime on thread exit | **[HYPOTHESIS]** several GB | | medium |

**1 — turn the persistent translation cache off.** `hb_runtime.c:1702-1704` reads
`MACRUNNER_HB_TRANSLATION_CACHE`; `=0` disables regardless of the root. `scripts/mr-run.sh:108`
currently defaults it to `1`. As shown in §1.2 the on-disk file is **16 bytes** — the cache is
being built 115 times over and persisted zero times, so today it is **pure cost with no warm-start
benefit in this configuration**. This is the single best ratio in the table: one env var,
9 GB of address space and 30 % of all swap. `scripts/gate-A-reset-hk.sh:16` already runs with
`=0`, so the configuration is known-good. Verify with a time-to-menu A/B — if translation reuse
*within* a run matters, the number will show it.

**2 — shrink the JIT arena.** `hb_runtime.c:1687-1691` accepts
`MACRUNNER_HB_JIT_BUFFER_SIZE` in `[65536, 512 MiB]`. Measured mean residency is **1.7 MiB per
128 MiB arena** (0.19 GB across 115), so 32 MiB leaves ~19× headroom on the average and takes
115 × 128 MiB → 115 × 32 MiB, i.e. **14.38 → 3.60 GB**. The risk is the sticky
`code_cache_full` latch the 2026-06-17 comment warns about (`hb_runtime.h:59-65`): a single
callback whose translations exceed the arena disables the JIT. That is *observable* — watch
`macrunner-hb-jit-hot-blocks: … used=` (`hb_runtime.c:1663`) and
`rt->code_cache_full_reports`. Start at 32 MiB, not 16 MiB, and only tighten if `used=` stays low.
This is also the most direct relief for §3: it removes ~10.8 GB of our footprint from the exact
band Mono places into.

**3 — stop paying 2,944 B for a one-instruction block.** `hb_ir.c:37`. `hb_ir_emit` already
doubles on overflow (`hb_ir.c:113-115`), so lowering `instr_cap` to 4 is behaviour-preserving —
it trades a few reallocs for a 4× cut in the dominant small-zone object. 4 × 184 = 736 B →
`malloc_good_size` 768 B, which moves the allocation **out of the small zone into the tiny
zone entirely**. Largest single lever on the swap bill, but its size is an estimate because the
occupant identity is §1.4's `[HYPOTHESIS]`. **Do #1 and #2 first, then settle §1.4, then do this.**
Better still, and strictly larger: `sizeof(hb_ir_instr_t)` is 184 B for three operands that are
mostly immediates — shrinking the operand union would scale every IR structure at once.

**4 — resize the block cache.** `HB_BLOCK_CACHE_SIZE` is a compile-time constant
(`hb_runtime.h:66`) with no env override. Its sizing rationale is explicitly "so the 128 MB exec
buffer is the real limiter" — if #2 lands, a 32 MiB arena holds ~85 K blocks at the comment's
~377 B/block, so 65,536 slots is the matched size: 40 MiB → 5 MiB per runtime, **4.38 → 0.55 GB**.
These two must move together or the latch will fire.

**5 — the principled version of #1.** One process-wide cache behind a lock instead of 115
private ones. The struct comment (`hb_aot_cache.c:60`) documents the single-thread assumption,
so this is real work, and it should follow the #1 A/B rather than precede it.

**6 — release the pooled runtime when its thread exits.** No destructor exists today (§1.1).
Size unknown until a create/destroy counter says how many of the 115 belong to dead threads.

### On `find_region_normalized` — it is **not** linear

The brief infers a linear region list from the ~5 % profile share. That inference is wrong:

```c
/* engine/hyperbridge/src/hb_memory.c:1598-1607 */
for (hb_region_t* n = mem->region_tree; n; ) {
    if      (addr <  n->base)            n = n->tree_left;
    else if (addr >= n->base + n->size)  n = n->tree_right;
    else { hot_cache_insert(mem, n); return n; }
}
```

It is an **O(log n) treap walk** with an MRU hot cache in front (`hb_memory.c:1586`), converted
from a linear scan on 2026-06-17 precisely because it was the #1 hotspot. Its profile share is
*call frequency* — one lookup per emulated memory access — not list length. **Shrinking the
region list will not speed it up.**

The genuinely superlinear code is elsewhere, and it is on the hot path this lane's memory note
already flagged (`sync_virtual_region` per `HeapAlloc`):

```c
/* engine/hyperbridge/src/hb_memory.c:718-731  — hb_memory_sync_live_range */
for (hb_region_t* exact = mem->regions; exact; exact = exact->next) {
    ...
    for (hb_region_t* other = mem->regions; other; other = other->next) {
        if (other != exact && range_overlaps(base, size, other->base, other->size)) {
```

That is **O(N²) in the region count**, on a list that grows all run, reached from
`macrunner_hb_sync_virtual_region` (`macrunner_hb.c:14981`) — which has ~20 call sites including
every guest `VirtualAlloc` commit (`macrunner_hb.c:29982`). If region-list cost is worth
attacking, this is the function to attack, not `find_region_normalized`.

---

## 6. What this lane did not establish

* **The identity of the 14.8 GB small-zone occupant** is by size class, not call site (§1.4).
  Named next step: `MallocStackLogging=1` at launch + `vmmap -stacks`, or a counter pair
  around `hb_ir_block_create`/`hb_ir_block_destroy`.
* **410 cache arrays vs 115 open caches** — the 3.5× multiplicity is unexplained (§1.2).
* **How many of the 115 runtimes belong to dead threads** — no counter exists (§1.1).
* **Mono's exact allocation sequence** — inferred from the shape of 197 refused requests; the
  Mono source is not in this tree (§3).
* Searched and **not found**: any thread-exit teardown for the pooled runtimes
  (`grep` over all 12 uses of the three `__thread` symbols in `macrunner_hb.c`); any
  `MallocStackLogging` in `scripts/mr-run.sh`; any env override for `HB_BLOCK_CACHE_SIZE`
  or `MACRUNNER_HB_IR_CACHE_SIZE` (`grep -rn` over `scripts/*.sh` and the two headers).
