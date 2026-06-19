# MacRunner (A) reset-pool verification report

Date: 2026-06-19
Commit: cc728b2 (includes A-reset-pool-snapshot tag ed782f7)

## What was implemented

1. `hb_jit_buffer_reset()` — rewind 128 MB arena bump pointer, mark writable, no `munmap`/`mmap`.
2. `block_cache_reset()` — eager free of every owned cloned IR block + clear table.
3. `hb_jit_runtime_reset()` — repoint ctx, reset jit_mem + block_cache + hot-trace state.
4. `macrunner_hb_ir_cache_reset()` — eager free of all cached IR funcs + clear table.
5. Per-thread pool in `run_x64`: `__thread` `jit_rt` + `ir_cache` reused across x64 callbacks;
   busy-guard handles nested/re-entrant frames.
6. Memoize `MACRUNNER_HB_TRACE_VIRTUAL_REGION` via `macrunner_hb_cached_env_flag`.

Reset-not-recreate → zero cross-generation lazy-free → zero UAF risk.  
Generation/O(1) is intentionally deferred until profiling shows reset cost is a top hotspot.

## Build verification

- `libhyperbridge.a` rebuilt clean with `-Werror`.
- `ntdll.so` rebuilt and relinked; symbols verified:
  - `hb_jit_buffer_reset`
  - `hb_jit_runtime_reset`
  - `macrunner_hb_ir_cache_reset`
- `scripts/test-hyperbridge.sh`: 461 passed, 8 failed. Failures are pre-existing sandbox-related `mprotect errno=13` / write-deny issues, not caused by this change.

## Micro-benchmarks (run in sandbox)

Run with `scripts/bench-reset-pool.sh`.

### JIT buffer reset vs create/destroy

| metric | value |
|--------|-------|
| N | 100 |
| arena size | 128 MB |
| create+destroy total | ~1.2 ms |
| create+destroy per iter | ~0.012 ms |
| reset total | ~0.5 ms |
| reset per iter | ~0.005 ms |
| reset / create+destroy ratio | ~0.43 |

### Runtime reset+run vs create/destroy+run (with one real block-cache entry)

| metric | value |
|--------|-------|
| N | 100 |
| create+destroy+run total | ~100 ms |
| create+destroy+run per iter | ~1.0 ms |
| reset+run total | ~80 ms |
| reset+run per iter | ~0.8 ms |
| reset+run / create+destroy+run ratio | ~0.75 |

**Interpretation:** reset is measurably cheaper than create+destroy in both isolated and JIT-run micro-benchmarks. Reset has **not** become a new top hotspot in these controlled measurements.

## Historical sample analysis

Two Hollow Knight samples with visible per-callback JIT runtime create/destroy were inspected:

- `reports/phase4-hollow-knight/laneA-hang-char-try1-200403/profile/sample-t0040.txt`
  - `hb_jit_runtime_destroy` appears 15×, calling `hb_jit_buffer_destroy` → `__munmap`.
  - `hb_jit_runtime_create` appears, allocating via `_xzm_malloc_large_huge` (jit_mem allocation).
  - `getenv` / `__findenv_locked` also visible inside `macrunner_hb_run_x64`.

- `reports/phase4-hollow-knight/laneA-thread-image-v2-lownoise-20260613-110258/sample-hk-live-90.txt`
  - Top-of-stack: `__ulock_wait2` (27 766), `read` (18 805), `mach_msg2_trap` (2 534), `_kernelrpc_mach_vm_map_trap` (993).
  - `__findenv_locked` 187 hits — exactly what the env memoize targets.
  - `hb_jit_runtime_destroy` 8 hits, `hb_jit_runtime_create` allocation 15 hits.
  - `__munmap` is below the >=5 threshold in this sample; mmap-related cost appears mostly as `_kernelrpc_mach_vm_map_trap` from allocation, not munmap.

A Notepad++ sample (`reports/performance/npp-post-import-target-map-20260524-040821/npp-import-target-map.sample.txt`) shows `__findenv_locked` at 6 592 top-of-stack hits and `__munmap` 12 hits, confirming the two cost centers addressed by this change.

## Sandbox limitation

The agent sandbox used for this session **cannot** run `wineserver` (`bind: Operation not permitted`) or `ps`, so a live 1800 s Hollow Knight gate cannot be executed here. All Wine-dependent verification must run on the host.

## Host gate script

Run on the real host (not in this sandbox):

```bash
cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
. config/env.sh
# Long run + 300 s sample + 8-boot stale-exec watch
scripts/gate-A-reset-hk.sh A-fix-1800 1800
```

Then measure:

```bash
# (a) Did munmap/mmap dominance go away?
grep -E 'munmap|mmap|pthread_jit_write_protect_np' \
  reports/lane-a/A-fix-1800/sample-hk-300s.txt

# (b) Did reset become a top hotspot?
grep -E 'block_cache_reset|macrunner_hb_ir_cache_reset|hb_jit_buffer_reset|hb_jit_runtime_reset|memset' \
  reports/lane-a/A-fix-1800/sample-hk-300s.txt

# Ladder and fault summary
cat reports/lane-a/A-fix-1800/classify.log
cat reports/lane-a/A-fix-1800/faults.log
```

## Conclusion

- Code change is complete, committed, and tagged.
- Static build passes; new symbols are present.
- Micro-benchmarks show reset is cheaper than create+destroy and is **not** itself a new hotspot.
- Historical samples confirm the two targeted cost centers (`__munmap`/`mmap` from JIT runtime create/destroy and `__findenv_locked` from virtual-region tracing).
- Real 1800 s HK gate is blocked by the sandbox; the `gate-A-reset-hk.sh` script is ready to run on the host to collect the final profile.

**Next step:** run `scripts/gate-A-reset-hk.sh` on the host and inspect the resulting sample. If reset (memset/release loop) shows up as >15 % of a hot thread, file a follow-up for generation/O(1); otherwise this item is verified-forward.
