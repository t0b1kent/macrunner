# ENGINE-QUEUE — ranked integration plan for #1 (codegen/CPU owner)

> Single source of truth distilled from 12 handoff docs. #1 executes this queue;
> it replaces reading the 7+ input specs. All file:line refs are canonical main
> MacRunner (`/Users/timurtoby/Documents/MacRunner/Main/MacRunner`) unless marked
> `(graphics-prep)` or `(dualdata)`.
>
> **Owner:** #1 (Codex, CPU engine). **Graphics-dist pulls** are routed to the
> graphics lane (Kimi) — listed for awareness, not for #1 to implement.
>
> Compiled by lane `engineque` (READ-ONLY). Date: 2026-07-04.

---

## 2026-07-04 CODEGEN OWNER UPDATE

Post-macOS-update floor-guard is clean on the current deploy / clean source
baseline: rung 11 still holds, real `CreateSwapChainForHwnd` reaches `rc=0`,
Mono and DXMT/Metal initialization pass, and no new error class appeared. This
closes the old rc=137 "clean HEAD might need region-fusion" confound for the
floor-guard baseline: region fusion is no longer treated as load-bearing for
boot.

Updated order from codegen ownership:
1. Run the prepared 1800s observation only when the host is assigned to Lane A:
   `scripts/laneA-run-hk-1800-ready.sh`. This answers whether wall time alone
   reaches GetBuffer/RTV/Present and produces the post-swapchain 770s heartbeat
   histogram plus `MACRUNNER_HB_NATIVE_MEMMOVE=1` hit counter.
2. If rung 12/13/14 is not reached, use that heartbeat histogram as HK-HOTLIST
   for the next lowering target. Do not land codegen levers before this run.
3. Region fusion remains Q1 as a default-OFF perf feature after the observation
   run, not as a drift fix. Trampoline-chain remains deferred until its design
   diff is available.

---

## 0. WHAT IS ALREADY IN MAIN HEAD (do NOT re-propose)

Verified against `git log --oneline -15` + `git grep` on HEAD:

| Item | Commit(s) | Status |
|------|-----------|--------|
| Block chaining (Change A: `patch_block_tail`, chain-slot epilogue, `block_cache_chain_meta`) | e8f7503, 81669cf, 8b03e7f, 51fe589 | LANDED, default-on (`MACRUNNER_HB_BLOCK_CHAIN`) |
| Dispatch fast-path infra + SINGLE_LOOKUP default + dispatch-rate stats | e8f7503, 81669cf, 8b03e7f | LANDED |
| macOS msync enabled by default | 44720ce | LANDED |
| HK sync-semantic kill switches + trace counters | a89eb07, 44f34d3, dfdec1a, a89eb07, 66d93c6, e5c3e24, 0cc0489 | LANDED |
| Gated native memmove helper (`MACRUNNER_HB_NATIVE_MEMMOVE`, default OFF) | e180fae | LANDED — HKBOOST merge recommendation #1 is DONE |
| HK lane live-run guard scoped to own Wine dist | e5ab613 | LANDED |
| mr-clean kill scoped to own worktree | d09a252 | LANDED |
| Mono JIT fusions gated to Mono modules | 0cc0489 | LANDED |

**Not in HEAD (the actual open engine work):** region fusion, dual-.data
coalescer, SEMSHORT stub routing, indirect inline cache, graphics dist refresh.

---

## 1. INVENTORY — every handoff, status + file:line

### Engine-queue items (engine/** — #1's scope)

#### 1.1 DISPATCH-ENGINE-HANDOFF (block chaining + indirect IC)
- **Change A (block chaining): LANDED** in main (`patch_block_tail` @
  hb_runtime.c:1462/1703, dispatch call @ hb_runtime.c:3412, chain slot in
  epilogue hb_arm64_codegen.c:564/573, `hb_block_chain_meta_t` @
  hb_runtime.h). Confirmed by UTFFUSION §0 + `git grep`.
- **Change B (indirect inline cache): NOT DONE.** Spec in handoff §4.2.
  Files: `emit_native_indirect_jmp` (hb_arm64_codegen.c:1607) /
  `emit_native_indirect_call` (hb_arm64_codegen.c:1616) — reserve 16-byte IC
  slot, patch on first dispatch resolution. Env: `MACRUNNER_HB_INDIRECT_IC`.
- **Change C (gate/remove two_block_loop C-helper): NOT DONE**, deferred until
  A+B confirmed. `try_promote_hot_block_families` @ hb_runtime.c:2240/2615,
  `hb_jit_helper_exec_two_block_loop` @ hb_arm64_codegen.c:8075.

#### 1.2 UTFFUSION-ENGINE-HANDOFF (region fusion) — NOT in HEAD
- Spec status: **detailed, implementable** (§3-7). A WIP implementation exists
  in `stash@{0}` (`laneA-region-fusion-wip-launchkill`, 568 ins / 6 files) — see
  §2 conflict map. NOT committed to HEAD.
- Files: hb_runtime.h (add `region_id`+`native_offset` to entry, `hb_region_meta_t`,
  `region_meta`/`next_region_id` on cache), hb_context.h
  (`HB_CONTEXT_CODEGEN_REGION_FUSION`), hb_codegen.h (`region_superblock` decl),
  hb_arm64_codegen.c (+221: `hb_arm64_codegen_region_superblock`,
  `emit_region_terminal/jcc_terminal/side_exit`, `region_grow`,
  `emit_region_branch_to_label`, `patch_region_branches`),
  hb_runtime.c (+313: `runtime_region_fusion_enabled`, `try_promote_hot_region`,
  `block_cache_region_meta`, `block_cache_reset` region clear, dispatch call),
  macrunner_hb.c (+18: `macrunner_hb_pc_allows_region_fusion` module gate).
- Env knobs: `MACRUNNER_HB_REGION_FUSION` (master, default OFF),
  `MACRUNNER_HB_REGION_WARMUP=64`, `MACRUNNER_HB_REGION_MAX_BLOCKS=8`,
  `MACRUNNER_HB_REGION_MAX_SPAN=1024`, `MACRUNNER_HB_REGION_MAX_INSTRS=64`,
  `MACRUNNER_HB_TRACE_REGION_FUSION`.
- A/B counter (non-placebo): `macrunner-hb-region-fusion-entry` (>0 and grows
  with ABZU runtime); `trace_dispatch_stats` header block's
  `block_cache_find` count must drop by ~trip count on a 7.5K-dynamic-count bin
  (proves the backedge no longer dispatches). Lockstep correctness vs C
  interpreter oracle required before any perf claim (§7).

#### 1.3 SEMSHORT-ENGINE-HANDOFF (swallowed-side-effect stubs) — NOT in HEAD
- Audit of `macrunner_hb.c` `macrunner_hb_try_*_semantic` sinks that return
  success without the real WinAPI side effect. Dispatch sink @ L18007-18045.
- **P0 (critical, route to real Wine):**
  - #1 `com_apartment` @ macrunner_hb.c:14141 — fakes CoInitializeEx; ABZU
    msctf `x8=0x24` garbage-vtable crash root. Fix: ROUTE (drop from classify
    list L3380-3390, or BRIDGE: call real CoInitializeEx after counters).
  - #2 `winrt` @ macrunner_hb.c:14102 — RoInitialize faked; RoActivate returns
    REGDB_E_CLASSNOTREG + inconsistent STATUS_SUCCESS. Fix: ROUTE RoInit.
  - #3 `registry` @ macrunner_hb.c:16396 — writes dropped, reads "not found",
    fake HKEY handles unsafe if passed to non-stubbed Reg*. Fix: ROUTE all.
- **P1 (high):** #4 `vectored_exception` @ macrunner_hb.c:16730 (separate VEH
  table, guest VEH bypassed on real exceptions); + sync primitives
  (keyed_event @ ~P2: cross-world wait/wake). See handoff for full P1/P2 list.
- **P2/P3 (low):** etw @ macrunner_hb.c:9878 (fake provider handle); get_module_handle
  PIN flag @ L7271; user32 pseudo-hwnd @ L14268 (rundll32-scoped, mostly LEAVE).
- **CLEAN (no action):** kernel32 handle sync primitives, security_token, bcrypt,
  ntdll_version, system_info, crt_*, synthetic_d3d — all route to real Nt* or
  are pure queries.
- Env: none per-item (routing changes are unconditional correctness fixes); gate
  behind existing `MACRUNNER_HB_*_SEMANTIC` knobs if a perf regression appears.
- A/B: ABZU `x8=0x24` crash count must drop to 0 after #1; registry round-trip
  parity probe (write then read same value) for #3.

#### 1.4 DUALDATA (DVRTLINK + DUALDATA-SYSTEMIC) — NOT in HEAD (in dualdata worktree)
- DUALDATA-SYSTEMIC: tracked implementation in dualdata worktree (virtual.c
  +202, loader.c +12, unix/loader.c +4, unixlib.h). Blind 8-byte EC→native
  copy-forward, post-DllMain. Files: virtual.c:3701-3893
  (`build_arm64x_dualdata_pairs` @ 3770, `coalesce_arm64x_dualdata` @ 3876),
  loader.c:5512-5522 (post-DllMain call), unix/loader.c:1813/1829/1855/1874,
  unixlib.h:126-129.
- **DVRTLINK supersedes the blind copy.** Verdict: DVRT is NOT empty (parse
  error in checkpoint) — it patches .rdata/PE-header only, never .data. The
  blind 8-byte copy corrupts combase `apts` self-referential pointers. Refined
  fix = **pointer-aware coalesce**: parse base `.reloc` DIR64 set as
  pointer/scalar discriminator; scalars plain-copy, in-`.data`-range pointers
  rebase by view delta, external pointers copy-as-is. Same files (virtual.c
  build/coalesce functions); adds base-reloc parse to `build_arm64x_dualdata_pairs`.
- Env: `MACRUNNER_ARM64X_DATA_STOPGAPS` (legacy per-DLL stopgaps, to be removed
  only after coalescer green). Master gate on the coalescer itself TBD.
- A/B: abzu normal-prefix matrix with stopgaps OFF + coalescer ON →
  `systemic_only rc=0`, TSF `hr=0`, CoInitializeEx `hr=0`, `apts` head
  self-consistent before AND after CoInitializeEx.
- **Merge note:** take DVRTLINK's pointer-aware version, NOT the blind
  DUALDATA-SYSTEMIC copy. DUALDATA-SYSTEMIC is the tracked base; DVRTLINK is
  the required refinement layered on the same functions.

### Graphics-dist items (NOT engine code — routed to graphics lane / Kimi)

#### 1.5 DXMTPOLL-ENGINE-HANDOFF — staged in graphics-prep dist, NOT in main dist
- DXMT command-queue `Sleep(1)` poll → condvar wake (`dxmt::condition_variable`
  via `SleepConditionVariableSRW`/`WakeConditionVariable`). Files: 3 in
  `engine/dxmt/src/` (util_cpu_fence.hpp, dxmt_command_queue.hpp/cpp).
- main dist d3d11.dll SHA `6560e9f9…` ≠ staged `161d1577…` (dxmtpoll) /
  `6b3b6be1…` (presentpath). **Stale in main.** Pull staged dist into main.
- Build-verified (aarch64 + x86_64 clean). Runtime blocked by pre-existing HB
  loader crash (`code=80000002 pc=…EFB9930`) before PE main — off-lane.
- A/B: bins `d3d11.dll+0xee100/+0xee300` must vanish; command cadence <1ms;
  background `NtDelayExecution` from dxmt threads = 0.

#### 1.6 PRESENTPATH + GETBUFFER — staged, chain verified complete, NOT in main dist
- Full 12-step GetBuffer(0)→CAMetalLayer.present chain verified READY. GAP-A
  HWND→CAMetalLayer binding hardened (winemetal_unix.c, fail-loud not
  silent-null). GetBuffer single-buffer contract (d3d11_swapchain.cpp:283).
- Same dist SHA mismatch as 1.5. Runtime blocked by same HB loader crash.
- No engine code change; pull staged dist (d3d11.dll, dxgi.dll, winemetal.dll,
  winemetal.so) into main.

### Refutation / diagnostic-only items (see §4 DEAD)

#### 1.7 SYNCWAKE-ENGINE-HANDOFF — verdict: NO FIX (primitive race-free)
- address-wait (WaitOnAddress/WakeByAddress) race-free via latched
  NtAlertThreadByThreadId (macOS kqueue EVFILT_USER, per-tid pending event).
  HB-custom path (macrunner_hb.c:13590/13694/13760) AND real-wine path
  (sync.c:919/980/1015) both race-free. Lost-wakeup REFUTED.
- HK deadlock = dead/stuck owner under oversubscription (scheduling), NOT
  sync. DXMT = scheduling/priority. See §4.

#### 1.8 MULTIWAIT-ENGINE-HANDOFF — verdict: NO FIX (poll hypothesis falsified)
- 9.5ms is NOT a poll cadence. Wait is event-driven (ulock_wait / mach_msg
  blocking receive / ulock_wake), no fixed sleep anywhere. Original hypothesis
  falsified by code inspection. 9.5ms, if real, = legitimate contention (slow
  signal producer). See §4.

#### 1.9 HKBOOST (DRIFT + AB-REPORT) — NATIVE_MEMMOVE DONE, HK long-run measurement open
- NATIVE_MEMMOVE port: LANDED (e180fae, default OFF). HKBOOST merge rec #1 DONE.
- DRIFT-89361993: old deployed floor binary `89361993` had region-fusion WIP
  baked in by accident; keep `scripts/hkboost-driftcheck.sh` as the standard
  build-gate before perf claims.
- Current post-update floor-guard reaches Mono/DXMT/swapchain on clean HEAD, so
  HK is reopened for the 1800s observation run. Treat HK native-memmove hits as
  a presence/correctness signal; keep ABZU as the throughput validator.

---

## 2. CONFLICT MAP — where specs overlap in code

All three "dispatch-overhead reduction" approaches touch the SAME three
code regions: (a) the block epilogue in `hb_arm64_codegen.c` (emit_ret /
emit_epilogue / chain-slot @ ~33/424-428/564), (b) the block-cache entry
struct in `hb_runtime.h` (33-44), and (c) the dispatch-loop post-block
resolution in `hb_runtime.c` (~2576 find_block-again / ~3412
patch_block_tail call). The table below is the safe-ordering decision.

| Approach | State | Epilogue (codegen.c) | block_cache_entry_t (runtime.h) | Dispatch loop (runtime.c) |
|----------|-------|----------------------|---------------------------------|----------------------------|
| **(A) Block chaining** (DISPATCH Change A) | LANDED in HEAD | chain slot @ 564/573 (4 NOP before LDP/RET) | `chain_meta` side-alloc | `patch_block_tail` @ 1462/1703, call @ 3412 |
| **(B) Region fusion** (UTFFUSION + stash@{0}) | WIP in stash@{0}, NOT in HEAD | `hb_arm64_codegen_region_superblock` (+221) reuses per-instr emitters, emits intra-region `Bcc`/`B` + side-exit epilogue | +`region_id`+`native_offset`; `region_meta`+`next_region_id` on cache | `try_promote_hot_region` call; `patch_block_tail` guarded `if (cur->region_id \|\| next->region_id) return false`; `run_jit_block_with_signal_guard` native_offset check |
| **(C) Trampoline-chain-design** (Fable, TBD) | NOT DELIVERED (TRAMPOLINE-CHAIN-DESIGN.md absent) | unknown — presumed rewrites epilogue/dispatch | unknown | unknown |

### Stacking vs mutual exclusion

**(A)↔(B): STACK — fusion is layered on top of chaining, not competing.**
UTFFUSION §3d designs this explicitly: fused entries skip chain patching
(region_id guard in `patch_block_tail`, already present in stash@{0}); a
chained block whose `next` is a region member chains to the superblock entry
(automatic if all member guest_addrs register to the superblock); the chain
slot and intra-region branches patch DIFFERENT bytes (slot is in the epilogue,
internal branches are in the body) so they cannot stomp. **Safe order: A
confirmed green → B on top.** B reuses A's chain-slot infra + dispatch
fastpath. This is already how stash@{0} is structured.

**(B)↔(C): UNKNOWN — cannot assess until Fable delivers the trampoline spec.**
If trampoline-design also rewrites the block epilogue or the dispatch
resolution point, it conflicts with B's superblock emission and the
region_id guard. If it only adds a separate trampoline page (no epilogue
rewrite), it may stack. **DEFER C until the doc exists; do NOT land B and C
simultaneously against the same epilogue without a diff comparison.**

**(A)↔(C):** depends on whether trampoline-design replaces the chain-slot
mechanism or augments it. Since A is already landed and default-on, C must
prove it does not regress A's fastpath.

### The rc=137 confound (Gate #0 — closed 2026-07-04)

The deployed "good" binary (`89361993`) contained BOTH chaining (A, committed)
AND region-fusion (B, from stash@{0} baked in), so the old "2.02x
SINGLE_LOOKUP" perf claim remains historically confounded. The post-macOS-update
floor-guard re-verify on the current clean deploy reaches rung 11 and real
swapchain without committed region fusion, so B is no longer considered
load-bearing for HK boot/floor preservation.

**Consequence:** keep `scripts/hkboost-driftcheck.sh` before perf claims, but do
not block new work on stash@{0}. Re-measure SINGLE_LOOKUP only on a clean,
driftchecked base, and only after the prepared 1800s observation run answers
whether time alone reaches GetBuffer/RTV/Present.

### Safe landing order

1. Gate #0 is closed by the 2026-07-04 clean floor-guard re-verify: A-alone
   boots to rung 11 and real swapchain on the new OS.
2. Run the 1800s observation before new codegen. If time alone reaches
   GetBuffer/RTV/Present, do not spend the next cycle on epilogue/codegen
   changes.
3. If the 1800s run stalls after swapchain, use its heartbeat histogram as the
   HK-HOTLIST and land one default-OFF lowering lever at a time.
4. Region fusion (B) may stack on top of chaining (A), but it still changes the
   block entry layout and side-exit/epilogue behavior. Land it as Q1 only after
   the observation run, with `MACRUNNER_HB_REGION_FUSION=0` default.
5. Indirect IC follows the reproducible A or A+B floor.
6. Trampoline-design: defer until Fable delivers; diff against B's epilogue
   rewrites before any landing decision. Do not land trampoline and fusion in
   the same cycle.

---

## 3. RANKED QUEUE for #1 — with gates

Ordered by (expected effect × readiness ÷ risk). Each item: why, files, env,
A/B non-placebo criterion, what it can break.

### GATE #0 — rc=137 disambiguation — CLOSED 2026-07-04
- **Verdict:** clean floor-guard on the new OS reaches rung 11 and real
  `CreateSwapChainForHwnd rc=0` without region-fusion source in HEAD. The old
  "region-fusion might be load-bearing for boot" hypothesis is no longer the
  ordering constraint for HK floor preservation.
- **Remaining rule:** `scripts/hkboost-driftcheck.sh` stays a standard
  build-gate before any perf claim, because it catches accidental region-fusion
  contamination in deployed `ntdll.so`.
- **Next gate:** 1800s observation run (`scripts/laneA-run-hk-1800-ready.sh`)
  on an assigned fresh host. It must report `time_to_swapchain=...s`,
  post-swapchain heartbeat histogram for the 770s window, and native-memmove
  hits/bytes.

### Q1 — Region fusion (UTFFUSION spec, default-OFF) — AFTER 1800s observation
- **Why:** removes 3 of 4 per-iteration dispatch hops for hot multi-block
  loops (header→B2→B3→B4 stay intra-superblock; only side-exits dispatch).
  Biggest lever for ABZU UTF bins (7.5K dynamic count) AND HK Mono multi-block
  metadata walks. Existing 2/4-block C-helpers are C-interpreter (throughput
  regression vs native); fusion emits real native ARM64.
- **Files:** hb_runtime.h, hb_context.h, hb_codegen.h, hb_arm64_codegen.c
  (+221), hb_runtime.c (+313), macrunner_hb.c (+18). See §1.2.
- **Env:** `MACRUNNER_HB_REGION_FUSION` (master OFF), `..._WARMUP=64`,
  `..._MAX_BLOCKS=8`, `..._MAX_SPAN=1024`, `..._MAX_INSTRS=64`,
  `MACRUNNER_HB_TRACE_REGION_FUSION`. Module gate
  `HB_CONTEXT_CODEGEN_REGION_FUSION` set per-module in macrunner_hb.c.
- **A/B (non-placebo):** `macrunner-hb-region-fusion-entry` counter >0 and
  grows with ABZU runtime (proves fused path taken, not stale-xtajit placebo);
  `trace_dispatch_stats` header block `block_cache_find` count drops by ~trip
  count on the 7.5K bin (proves backedge no longer dispatches); lockstep
  correctness vs C-interpreter oracle (all regs + pc + lazy_flags + relevant
  memory bit-identical after N iterations) BEFORE any perf claim. Target:
  further blocks/s delta on top of chaining baseline.
- **Can break:** flags-fragile regions (R1: a CMP in one block, Jcc in next
  after intervening instr — mitigate with MRS NZCV save across internal branch
  or refuse to fuse that region); side-exit PC correctness on fault
  (sigsetjmp frame is from header entry — verify fault handler restores
  ctx->pc to the side-exit PC the superblock would have written); SMC
  invalidation if any member's guest bytes change; overlap with existing
  2/4-block fused entries (`try_promote_hot_region` must refuse if any member
  `entry->fused` is already true). Test HK Mono walks for R1 before gating HK.

### Q2 — SEMSHORT P0 stub routing (com_apartment / winrt / registry)
- **Why:** correctness. #1 com_apartment is the ABZU msctf `x8=0x24` crash root
  (uninitialized COM apartment → bogus vtable). #3 registry fake-handles are
  unsafe across the whole app surface. These almost certainly cause live
  hangs/crashes; the perf cost the shortcuts were added to avoid is already
  solved by `HB_SC` classification.
- **Files:** macrunner_hb.c:14141 (com_apartment), :14102 (winrt),
  :16396 (registry), classify list L3370-3390.
- **Env:** none required (unconditional correctness); if a per-call strieq
  storm returns, gate behind existing `MACRUNNER_HB_*_SEMANTIC` knobs.
- **A/B:** ABZU `x8=0x24` crash count → 0 (com_apartment); registry
  write-then-read round-trip parity (value survives across a
  RegSetValueExW→RegQueryValueExW on the same key); WinRT RoActivate no longer
  returns REGDB_E_CLASSNOTREG-with-STATUS_SUCCESS mismatch.
- **Can break:** removing a shortcut surfaces real-Wine latency for those
  APIs (CoInitializeEx now creates the apartment — extra work per call). If
  an app calls CoInitializeEx in a hot loop, profile; the shortcut was
  originally a perf dodge. COM apartment threading model must match what the
  app expects (STA vs MTA) — verify ABZU's CoInitializeEx arg is respected.

### Q3 — DVRTLINK pointer-aware dual-.data coalesce (ARM64X)
- **Why:** fixes the ARM64X duplicate-writable-.data class that crashes
  msctf (tlsIndex wrong slot) and combase (apts self-referential pointer
  corrupted by blind copy). Unblocks ABZU TSF / CoInitializeEx with stopgaps
  OFF. General (metadata-driven, no per-DLL hardcode).
- **Files:** virtual.c:3770 (`build_arm64x_dualdata_pairs` — add base-reloc
  DIR64 parse + per-pair pointer flag), :3876 (`coalesce_arm64x_dualdata` —
  add rebasing copy), loader.c:5512-5522 (post-DllMain call), unix/loader.c,
  unixlib.h. Take DVRTLINK's pointer-aware version; do NOT merge the blind
  8-byte DUALDATA-SYSTEMIC copy as-is.
- **Env:** `MACRUNNER_ARM64X_DATA_STOPGAPS` (legacy per-DLL stopgaps — remove
  ONLY after coalescer green). Coalescer master gate TBD (recommend
  `MACRUNNER_HB_ARM64X_DUALDATA_COALESCE`, default ON for ARM64X, NOP for
  non-ARM64X).
- **A/B:** abzu normal-prefix matrix, stopgaps OFF + coalescer ON →
  `systemic_only rc=0`, TSF ThreadMgr/InputProcessorProfiles `hr=0`,
  CoInitializeEx `hr=0`, `apts` head self-consistent before AND after
  CoInitializeEx. `arm64x_dualdata_pairmap.py` extended to emit pointer/scalar
  classification + simulate rebasing → combase apts self-pointer rebases into
  native .data range, list self-consistent.
- **Can break:** blind copy already corrupted combase apts — the pointer-aware
  version fixes that, but a DIR64 slot with an uninitialized/stale pointer at
  copy time is copied as-is (same as blind copy, no new failure mode). Pair
  delta sign depends on Wine ARM64X link order (EC before native → positive
  delta); derive from pair map, not a constant. One-time copy handles
  DllMain-init writes only; runtime-mutated shared state after attach is NOT
  covered (needs the long-term build-side DVRT/hybrid-object fix).

### Q4 — Indirect inline cache (DISPATCH Change B) — AFTER A+B floor stable
- **Why:** vtable/import-thunk dispatch pays full find_block + block_cache_find
  + sigsetjmp + func-ptr every call. Monomorphic call sites (99% same target)
  pay full dispatch every time. Big win for vtable-heavy code (ABZU/HK).
- **Files:** hb_arm64_codegen.c:1607 (`emit_native_indirect_jmp`),
  :1616 (`emit_native_indirect_call`) — reserve 16-byte IC slot; hb_runtime.c
  dispatch loop patches after resolving indirect target; invalidation in
  `block_cache_reset`.
- **Env:** `MACRUNNER_HB_INDIRECT_IC` (default OFF until validated).
- **A/B:** monomorphic indirect call hit-counter >0 and grows; polymorphic
  site miss safely falls back to dispatch (no wrong-target execution);
  `trace_dispatch_stats` indirect-target `block_cache_find` drops on a
  vtable-heavy bin.
- **Can break:** stale cache after target eviction/recompile → must verify
  chain valid (LDR+CMP+B.NE fallback) before following cached target, same
  pattern as block chaining's guard. Polymorphic sites must not lock onto a
  wrong target. Depends on chaining infra (Gate #0) being confirmed.

### Q5 — SEMSHORT P1 (vectored_exception, keyed_event, etc.)
- **Why:** correctness. VEH table separate from Wine's → guest VEH bypassed
  on real exceptions. keyed_event cross-world wait/wake can deadlock.
- **Files:** macrunner_hb.c:16730 (vectored_exception); keyed_event see
  handoff P2 entry.
- **Env:** gate per-knob.
- **A/B:** guest VEH handler invoked on a real (non-intercepted) exception;
  cross-world WaitOnAddress→NtWaitForKeyedEvent wake reaches HB-side waiter.
- **Can break:** VEH bridging is subtle — merging HB's VEH table with Wine's
  real list risks double-invocation or ordering regressions. Probe before
  routing.

### Q6 — Gate/remove two_block_loop C-helper (DISPATCH Change C)
- **Why:** with chaining (A) + fusion (B) + IC (Q4), native code is faster
  than C-level IR interpretation. Gate the 2/4-block C-helpers for A/B.
- **Files:** hb_runtime.c:2240/2615 (`try_promote_hot_block_families`),
  hb_arm64_codegen.c:8075/8120 (helpers).
- **Env:** `MACRUNNER_HB_JIT_HELPER_LOOP_BLOCK_BUDGET` (existing, 16384).
- **A/B:** blocks/s with helpers ON vs OFF on the same fused baseline; only
  remove if A+B+Q4 confirmed faster.
- **Can break:** some loops may not be fusible (region too large / flags-
  fragile) and currently rely on the C-helper. Removing it regresses those.
  Keep gated, do not hard-remove until coverage audit.

### Graphics-dist pulls (route to Kimi / graphics lane — NOT #1's code)
- **G1 DXMTPOLL condvar dist** — pull staged d3d11.dll/dxgi.dll (aarch64) into
  main dist. Readiness high (build-verified). Risk low. Runtime A/B blocked
  by HB loader crash (off-lane). A/B: +0xee100/+0xee300 bins vanish, cadence
  <1ms, background NtDelayExecution from dxmt threads = 0.
- **G2 PRESENTPATH/GETBUFFER dist** — pull staged d3d11.dll/dxgi.dll/
  winemetal.dll/winemetal.so (aarch64) into main dist. Chain verified
  complete. Same HB-loader-crash runtime block. No engine code change.

---

## 4. DEAD / OBSOLETE — списано с обоснованием

| Spec / claim | Verdict | Justification |
|--------------|---------|---------------|
| **MULTIWAIT 9.5ms poll-cadence fix** | DEAD (no engine change) | Falsified by code inspection: the wait is fully event-driven (ulock_wait / mach_msg blocking receive / ulock_wake), NO fixed sleep interval anywhere in client single (msync.c:301), client multiple (msync.c:366), server pump (server/msync.c:467, MACH_MSG_TIMEOUT_NONE), or wake path (server/msync.c:343). The original "esync/msync polls WaitForMultipleObjects on ~10ms" hypothesis is wrong. 9.5ms, if it genuinely persists post-msync, is legitimate contention (the signal producer is slow), not a wait-mechanism bug. Keep ONLY the optional per-tid syncmeter diagnostic (msync.c:301/366, ~20 lines, `MACRUNNER_HB_TRACE_SYNCMETER`) as an investigation tool — it is not a fix. |
| **SYNCWAKE lost-wakeup primitive fix** | DEAD (primitive race-free) | Lost-wakeup window REFUTED. Both HB-custom (macrunner_hb.c:13590/13694/13760) and real-wine (sync.c:919/980/1015) address-wait paths are race-free: predicate-check + registration are atomic under the spinlock, and NtAlertThreadByThreadId is latched (macOS kqueue EVFILT_USER, per-tid pending event survives arrival-before-wait). HK deadlock = dead/stuck owner under oversubscription (OwningThread non-zero, scheduling-induced cascading block) — NOT a sync-primitive bug. DXMT wake = scheduling-priority (wineserver pump is LATENCY_QOS_TIER_0/importance 63 vs client kqueue-alert normal priority), not lost-wake. No sync-code change. The HK critsec deadlock is a scheduling/diagnostic item (quiet host or `MACRUNNER_HB_TRACE_CRITICAL_SECTION=1` + `WINEDEBUG=+sync,+server,+heap` to find where owner fe8f0a12 is blocked), not an engine fix. |
| **DUALDATA-SYSTEMIC "DVRT is empty" premise + blind 8-byte copy** | SUPERSEDED (take DVRTLINK) | The "BaseRelocSize=0x0" was a parse error; DVRT is NOT empty (15 fixups in msctf, 4 blocks) — but it only patches read-only .rdata/PE-header, never writable .data, so it structurally cannot fix the runtime-divergent writable-data class. The blind 8-byte EC→native copy in DUALDATA-SYSTEMIC corrupts combase `apts` self-referential pointers (LIST_INIT self-pointer). Use DVRTLINK's pointer-aware coalesce (base-reloc DIR64 discriminator + in-.data-range rebasing) instead. The loader copy-forward is a BRIDGE; the long-term retirement is clang ARM64X hybrid-object codegen (out of scope for #1). |
| **HKBOOST "2.02x SINGLE_LOOKUP" perf claim** | HISTORICALLY CONFOUNDED | The deployed floor binary 89361993 was e5ab613 + stash@{0} region-fusion WIP baked in by accident (fc1a6924-class drift, 2nd occurrence). The number was measured on a binary that ALSO contained region-fusion; it is not attributable to SINGLE_LOOKUP alone. Current clean floor-guard closes the boot-risk part of Gate #0. Re-measure only on a driftchecked clean base, after the 1800s observation run if the result is still useful. |
| **HK as throughput validator for DIRECT_MEM / NATIVE_MEMMOVE** | LIMITED (observe, benchmark on ABZU) | Post-update clean floor-guard reaches Mono/DXMT/swapchain, so HK is valid for the prepared 1800s observation run and native-memmove hit-counter collection. Do not use HK as the throughput benchmark unless `macrunner-hb-native-memmove: hits=` is non-zero and the post-swapchain heartbeat window shows a sustained relevant hot path. ABZU remains the benchmark for throughput claims. NATIVE_MEMMOVE is already merged (e180fae, default-OFF); HKBOOST merge rec #1 is DONE. |
| **DXMT atomic-notify (libc++ `__ulock_wake`) path under HB** | NOT an engine fix (DXMT-side) | DXMTPOLL already replaced `Sleep(1)` polling with Win32 condvar (`SleepConditionVariableSRW`/`WakeConditionVariable`) which IS delivered under HB (wineserver-wait path), unlike libc++ atomic-wait ulock. This is a graphics-side fix (staged, G1), not an engine-primitive change. If atomic-wait is ever wanted back, the issue is DXMT using a path bypassing the Win32 API (statically-linked libc++ `__ulock`), which no HB intercept catches — that is a DXMT build-config fix, not an HB-primitive fix. |

---

## 5. REFERENCES (source handoffs, for audit trail only — #1 need not re-read)

- DISPATCH-ENGINE-HANDOFF.md — chaining anatomy + Change A/B/C.
- UTFFUSION-ENGINE-HANDOFF.md — region fusion design (§3-7), ABZU evidence.
- SEMSHORT-ENGINE-HANDOFF.md — swallowed-side-effect stub audit (P0-P3).
- DUALDATA-SYSTEMIC-HANDOFF.md (MacRunner-dualdata) — tracked coalescer base.
- DVRTLINK-HANDOFF.md (MacRunner-dualdata) — pointer-aware refinement, supersedes blind copy.
- SYNCWAKE-ENGINE-HANDOFF.md — lost-wakeup REFUTED (race-free).
- MULTIWAIT-ENGINE-HANDOFF.md — 9.5ms poll hypothesis FALSIFIED.
- DXMTPOLL/PRESENTPATH/GETBUFFER-ENGINE-HANDOFF.md — graphics-dist (G1/G2).
- DRIFT-89361993-RECONCILIATION.md (MacRunner-hkboost) — Gate #0 root.
- HKBOOST-AB-REPORT.md (MacRunner-hkboost) — NATIVE_MEMMOVE DONE, HK can't validate.
- TRAMPOLINE-CHAIN-DESIGN.md (MacRunner-hkboost) — NOT DELIVERED by Fable; conflict (B↔C) unresolvable until it exists.
