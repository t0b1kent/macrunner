# LANE HK-INTERP — why execution sits in the interpreter, and what it costs

Date: 2026-07-28. Source: `reports/phase4-hollow-knight/interp-stall-sample-20260728.txt`
(sample(1), 1 ms interval, pid 91129, 57 threads, 3714 ticks/thread, 211 698 thread-samples total).
All percentages below are self-time (leaf) unless marked `incl`. Parser cross-checked:
sum of per-thread leaf samples == sum of thread totals == 211 698 (no samples lost).

## 0. Correction to the brief's premise

"0 JIT/translation frames" is an artifact of symbolication, not of dispatch. The generated
ARM64 blocks appear as `??? (in <unknown binary>)` frames — on the interpreter-heavy thread
(27296671) 95.9 % of samples sit **under** `hb_jit_runtime_run`, and the interpreter helpers
(`hb_jit_helper_exec_interp_ir`, `hb_jit_helper_exec_two_block_loop`) have `???` as their
parent, i.e. **they are called from generated code**. Translation during the window is
negligible: `hb_arm64_codegen_*`/`hb_codegen_buffer_*` ≈ 4 samples total (0.02 %).
So: JIT dispatch was active the whole time. The cost is not "interpreter instead of JIT" —
it is **JIT blocks whose bodies are mostly calls back into C helpers**, plus per-block
scaffolding in the dispatch path. Details below.

## Q1. What selects interpreter vs translated execution

Three distinct selection points, all quoted.

### 1a. Function level (Wine side) — `engine/wine/dlls/ntdll/unix/macrunner_hb.c:38539-38579`

```c
if (jit_rt)
{
    macrunner_hb_jit_fault_trace_begin( block_pc );
    ret = hb_jit_runtime_run( jit_rt, func, &out );
    macrunner_hb_jit_fault_trace_end( ctx, ret, &out );
    if (ret == HB_OK && (out.result == HB_ERR_UNSUPPORTED_OPCODE ||
                         out.result == HB_ERR_UNSUPPORTED_FEATURE ||
                         out.result == HB_ERR_INTERNAL))
    {
        ...
        ret = hb_runtime_run( ctx, func, HB_BACKEND_INTERP, &out );
    }
}
else
    ret = hb_runtime_run( ctx, func, HB_BACKEND_INTERP, &out );
```

JIT first; the whole function re-runs under the interpreter only on explicit
UNSUPPORTED_OPCODE / UNSUPPORTED_FEATURE / INTERNAL from the JIT run. The same site logs
`macrunner-hb-jit-fallback:` with 16 guest bytes for ranking (rate-limited to 8000 prints).

### 1b. Block level (dispatch loop) — `engine/hyperbridge/src/hb_runtime.c:4395-4461, 4536-4574`

```c
if (dispatch_fastpath) {
    cached = block_cache_find(rt->block_cache, ctx->pc);
    cached = smc_reverify_entry(rt, cached);
    if (cached && cached->block)
        block = (hb_ir_block_t*)cached->block;
}
...
hb_result_t run_result = run_jit_block_with_signal_guard(rt, cached, out,
                                                          steps, blocks_executed);
```
Cache hit → native. Miss → compile immediately (no interpret-first tier):

```c
r = hb_arm64_codegen_block_with_cfg(cg, compile_block, func->cfg, code_buf);
...
if (r != HB_OK) { ... out->fault_reason = "JIT codegen failed"; return HB_OK; }
```
Codegen failure, code-cache full, or signal-quarantine → `set_jit_interp_fallback_result(...)`
(hb_runtime.c:1854) → the 1a fallback above.

### 1c. Instruction level — the dominant one in this sample

Codegen **deliberately emits calls to C interpreter helpers** for IR it does not lower
natively — `engine/hyperbridge/src/hb_arm64_codegen.c:3303-3309`:

```c
static hb_result_t emit_interp_ir_helper(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_interp_ir);
    emit_return_if_helper_failed(buf);
    return HB_OK;
}
```
Used at 14 sites in the file. And whole recognized loop idioms are emitted as helper calls
(`emit_two_block_loop_helper`, :3348-3361; four-block, unity-sort, mono-string-hash, …).
These helpers bottom out in the interpreter switch: `hb_jit_helper_exec_ir_block_once`
(:8073) and `hb_jit_helper_exec_interp_ir` (:6765) → `exec_instr` / `exec_instr_unlocked`
in `hb_interpreter.c`.

**So "execution sits in the interpreter" = generated blocks whose hot instructions are
individually delegated to the C interpreter, not a dispatch decision gone wrong.**

### 1d. The four knobs

All four are **codegen-time** gates in `hb_arm64_codegen.c` — they never select interp vs
JIT; they select whether a memory operand inside a JIT block becomes an inline host
LDR/STR or a C helper call:

```c
:453  static bool jit_direct_mem_enabled(void)      { ... "MACRUNNER_HB_JIT_DIRECT_MEM", 0 ... }
:505  static bool jit_direct_xmm_mem_enabled(void)  { ... "MACRUNNER_HB_JIT_DIRECT_XMM_MEM", 1 ... }
:510  static bool jit_direct_scalar_mem_enabled(void) { ... default = jit_direct_mem_enabled() ... }
:528  static bool jit_direct_stack_enabled(void)    { ... "MACRUNNER_HB_JIT_DIRECT_STACK", 0 ... }
```
With them at 0, every scalar/stack/XMM guest access in generated code goes through
`hb_jit_helper_exec_load_operand_lazy` / `hb_memory_read` / `hb_memory_write` — the C path
with region lookup, trace hooks and TLV lookups. The sampled run forced all four to 0
(observability profile). Note `scripts/mr-run.sh:104-107` **defaults**
`DIRECT_MEM=0`, `DIRECT_SCALAR_MEM=0`, `DIRECT_STACK=0` for the arm64ec lane even outside
diagnostics — i.e. the helper-heavy memory path is the standing production default, not a
one-off profiling choice. (Why the lane keeps them off — presumably correctness/hook
coverage of the C path, e.g. SMC reverify and trace hooks see every access — is a policy
question for the engine lane. [HYPOTHESIS])

Verdict for Q1: the interpreter-heavy profile is **not caused** by the knobs (JIT dispatch
was active and hot), but the knobs **maximize the C-helper share** inside JIT blocks, and
the run's trace profile (`trace_mem_watch_bytes` was live — 57 TLV samples under it) adds
its own tax.

## Q2. Cost split, with numbers

Two threads matter; all other 53 are parked (`__ulock_wait2` 86 % of total process samples,
`mach_msg2_trap` 7 %, `__workq_kernreturn` 3.5 %).

### Thread 27296671 — the interpreter-heavy one (3714 samples = 100 %)

Self-time grouped by mechanism:

| Mechanism | samples | % | composition |
|---|---|---|---|
| **Per-block signal-guard snapshot** | 601 | **16.2 %** | `_platform_memmove` 292 + `_platform_memset` 239 under `run_jit_block_with_signal_guard`, + 70 self |
| **Interpreter machinery** | ~745 | **~20 %** | `exec_instr_unlocked` 311, `exec_instr` 68, `exec_ir_block_once` 121, `exec_interp_ir` 98, `exec_two_block_loop` 61, load/store_operand_lazy 44, context_read/write_reg 42+ |
| **Scalar memory helpers** | ~490 | **~13 %** | `hb_memory_read` 195+72(memmove), `hb_memory_write` 92+28, `resolve_addr` 103 |
| **Vector/XMM operand access** | ~382 | **~10 %** | `read_vec_reg_bytes` 81+90(memmove)+38(memset), `read_xmm_operand_bytes` 57+44, `write_vec_reg_bytes` 39, `read_scalar_float_bits` 33 |
| **TLS lookups (`_tlv_get_addr`)** | 218 | **5.9 %** | `trace_mem_watch_bytes` 57, `hb_memory_read` 38, vec/scalar readers ~55, misc |
| **Flag emulation** | ~190 | **5.1 %** | `hb_flags_read_operand_value` 76, `exec_binop` 47, `write` 33, `lazy_materialize` 28 |
| Generated code itself (`???`) | 100 | 2.7 % | the actual native guest work |
| Promotion attempts | 76 | 2.0 % | `try_promote_hot_block_families` (called at hit_count<4 and powers of 2 per cached block) |
| Idle in this thread | 103 | 2.8 % | `__ulock_wait2` |

Read: **actual instruction semantics (`exec_instr_unlocked` self) = 8.4 %; generated native
code = 2.7 %. Everything else is scaffolding** — per-block checkpointing, per-access helper
calls, per-access TLS, flags.

The 16.2 % signal-guard item is mechanical and precisely explained by
`hb_runtime.c:2593-2648`: every block dispatch does

```c
memset(&frame, 0, sizeof(frame));   // frame ≈ 3.3 KB
...
frame.snapshot = *ctx;              // sizeof(hb_context_t) = 2616 B (measured)
if (sigsetjmp(frame.env, 0) == 0) { exec(ctx); ... }
```
≈ 5.9 KB of memset+memcpy **per JIT block dispatch** (sizeof measured by compiling against
`engine/hyperbridge/include/hb_context.h`, `tools/hb-sizeof/sizeof_ctx.c`). The snapshot
exists only for the fault path (quarantine/interp-resume). ~1.8 KB of the context is
documented in the header as interpreter-only AVX/AVX-512 storage (`ymm_hi`, `zmm_hi`, `k`,
`xmm_ext`, `ymm_hi_ext`, `zmm_hi_ext` — hb_context.h:194-207) that generated ARM64 code
never touches.

### Thread 27295721 — the Unity producer thread (3714 = 100 %)

Not interpreter-bound at all:

| Item | samples | % | parent |
|---|---|---|---|
| `hb_memory_protect` (self 780) | 782 incl | **21.1 %** | `macrunner_hb_sync_virtual_region` 780/782 — an mprotect storm from Wine↔HB region syncing |
| `macrunner_hb_run_x64` self | 758 | 20.4 % | the outer per-block orchestration loop incl. its probe-call fanout (macrunner_hb.c:38490-38538 calls ~20 probe functions per block) |
| `macrunner_hb_call_import_thunk` | 860 incl | 23.2 % | import thunk traffic |
| `find_region_normalized` | 196 self | 5.3 % | `hb_memory_read` 175, `hb_memory_host_ptr` 23 |
| `try_promote_hot_block_families` | 160 self | 4.3 % | promotion retries |
| `exec_instr_unlocked` | 228 incl | 6.1 % | minor here |

(Thread 27322648 was also listed as "has interp frames" by my filter but is 99 % parked in
`NtWaitForSingleObject → inproc_wait → __ulock_wait2` — a legitimate wait, not a stall.)

## Q3. `find_region_normalized` — not an algorithmic problem (anymore)

Already fixed 2026-06-17. `engine/hyperbridge/src/hb_memory.c:1571-1609`:

```c
static hb_region_t* find_region_normalized(hb_memory_t* mem, hb_gva_t addr) {
    ...
    hb_region_t* hot = hot_cache_lookup(mem, addr);
    if (hot) return hot;
    /* MacRunner (2026-06-17, HK first-frame perf): O(log n) treap walk instead of an O(n)
     * linear scan of mem->regions. ... became the #1 main-thread hotspot (~33% during Mono
     * ReloadAssembly) ... */
    for (hb_region_t* n = mem->region_tree; n; ) { ... }
}
```
O(log n) treap + hot cache in front, and `hb_memory_read` (hb_memory.c:1139) checks
`hot_cache_lookup` **before** even calling it. On the interpreter-heavy thread it does not
register at all; the 132-sample figure from the brief is producer-thread traffic
(5.3 % there), downstream of the mprotect/sync storm and import-thunk memory reads —
fixing the storm removes most of it. **Do not spend an algorithmic fix here.**

## Q4. What a hotspot-hybrid would look like here

Mihocka's regret (separate interpreter + JIT, no promotion) is only half our story — the
tiering skeleton already exists:

- **Compile-on-first-touch** per block (no interpret-first tier): cache miss →
  `hb_arm64_codegen_block_with_cfg` → `block_cache_put` → run. Persistent translation
  cache on top (`MACRUNNER_HB_TRANSLATION_CACHE=1` default, mr-run.sh:108).
- **Pattern-based hotspot fusion**: `try_promote_hot_block_families`
  (hb_runtime.c:3931-3940) retries 7 hand-written loop families
  (copy-scan, bounded-scan, byte-compare, null-qword-scan, i32-less tiebreaker,
  unity-sort-inner, self-loop) at `hit_count < 4` and every power of two
  (`should_retry_cached_promotion`, :3942-3946). This *is* hotspot promotion — but
  pattern-keyed, not heat-keyed.
- **Fallback ranking log**: the `macrunner-hb-jit-fallback:` dump (macrunner_hb.c:38548-38571)
  is a manual version of a promotion work queue.
- **Block heat histogram**: `hb_jit_helper_block_hist_record` (:7715, env-gated) already
  counts executions per block guest_addr — but note it takes a `pthread_mutex_lock` per
  block when enabled, so it cannot be left on in production as-is.

What is missing vs a true hybrid, in order of leverage:

1. **Shrink the per-block checkpoint (biggest single win, see below).** Not a tiering
   change, but it is 16.2 % of the hot thread for a mechanism that only exists to make
   JIT↔interp fallback safe.
2. **Heat-keyed re-promotion of helper-heavy blocks.** Today `emit_interp_ir_helper` is a
   static codegen decision per IR op. The hybrid move is: let the block run with helpers,
   count heat via the existing `hit_count` (already maintained on the cache entry), and at
   a threshold re-translate the block with the specific hot IR ops forced through native
   lowering — which in practice means extending codegen coverage for the ranked Mono op
   set (the `NEXT-CODEX-ORDER.md` family pipeline already operationalizes exactly this
   work list; the missing piece is only the *automatic* ranking, which the fallback log +
   block histogram nearly provide).
3. **Direct-memory codegen as the default tier** (the four knobs). A hotspot interpreter
   that promotes to *helper-laden* native code buys little; the sampled run shows the
   memory helpers are ~23 % of the hot thread (scalar 13 % + vector 10 %). Enabling
   DIRECT_MEM/SCALAR/STACK/XMM codegen is the single largest "promotion quality" lever,
   gated by whatever correctness concern keeps them at 0 in mr-run.sh — that gate reason
   needs to be named by the engine lane before flipping. [HYPOTHESIS: SMC-reverify and
   trace hooks lose visibility of direct accesses.]
4. **Repeated-codegen-failure retry.** On codegen reject the block is not cached, so every
   re-entry re-attempts codegen (hb_runtime.c:4554-4564) — in this sample that cost is
   invisible (≈0 codegen samples), i.e. codegen-rejected blocks are not hot here. A
   negative-cache entry would be hygiene, not a performance fix. [HYPOTHESIS: worth
   confirming with a fallback-count trace before building.]

## The single change that buys the most

**Remove the per-block full-context snapshot in `run_jit_block_with_signal_guard`
(hb_runtime.c:2623-2628).** 16.2 % of the interpreter-heavy thread (601/3714 samples) is
memset+memcpy of a ~3.3 KB frame done on *every* JIT block dispatch to serve a fault path
that fires essentially never. Two engine-local options, in increasing ambition:

- snapshot only the state interp-resume actually needs at block-entry granularity
  (GPRs + pc + flags + XMM low halves ≈ 700 B; skip the ~1.8 KB of interpreter-only
  AVX/AVX-512 fields the JIT never touches), and replace the full-frame `memset` with
  selective field init;
- or capture lazily: keep only the tiny architectural checkpoint per block and rebuild
  vector state from the signal handler's `ucontext` on the (rare) fault.

Second lever (bigger ceiling, but a policy decision, not a code tweak): flip the four
direct-mem knobs on — the run's own profile shows ~23 % of the hot thread is the C memory
path they gate. Both are engine-lane territory; this lane changes nothing.

## Caveats / things I did not prove

- Thread names/roles (27296671 = game thread, 27295721 = Unity producer) inferred from
  stack content, not from Wine thread ids. [HYPOTHESIS]
- The run had live trace machinery (`trace_mem_watch_bytes`, HK lockstep probes in
  `hb_jit_helper_exec_ir_block_once`, ~20 per-block probe calls on the producer thread).
  An uninstrumented run will shift the percentages down for TLV and producer-thread
  orchestration, but not for the signal-guard snapshot or the memory helpers, which are
  unconditional code.
- `hb_jit_helper_block_hist_record`'s mutex only matters when its env gate is on; I did
  not verify it was off in this run. Searched: sample contains no
  `pthread_mutex_lock` self-time above noise — so it was off or cold.
- Why mr-run.sh defaults DIRECT_MEM/SCALAR/STACK to 0: not stated anywhere I searched
  (scripts/, prompts/, AGENTS.md). `not found != not there`.
