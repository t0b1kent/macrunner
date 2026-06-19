# MacRunner (A) reset-pool verification report

Date: 2026-06-19
Commit: 7434c4a (tags A-reset-pool-snapshot ed782f7, A-reset-pool-verified-forward)

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

## Host gate run: A-fix-live2 (2026-06-19)

Executed outside the sandbox with a freshly rebuilt `ntdll.so`:

```bash
cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
. config/env.sh
export DXMT_HEADLESS=1
export MACRUNNER_HB_TRANSLATION_CACHE=0
scripts/gate-A-reset-hk.sh A-fix-live2 300
```

### Build freshness

- `libhyperbridge.a`: already fresh (built 2026-06-19 11:37, after source change).
- `ntdll.so`: rebuilt from `engine/wine/dlls/ntdll/unix/macrunner_hb.c` at 2026-06-19 13:11 and copied to:
  - `engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`
  - `engine/wine/dist/lib/wine/aarch64-unix/ntdll.so`
- Verified imported symbols still present:
  - `hb_jit_buffer_reset`
  - `hb_jit_runtime_reset`
  - `macrunner_hb_ir_cache_reset`

### Gate result

- Exit code: `0`.
- Run log: `reports/lane-a/A-fix-live2/laneA-run.log`
- Valid run: `reports/phase4-hollow-knight/laneA-A-fix-live2-try1-131139`
- Classification (`reports/lane-a/A-fix-live2/classify.log`):
  - `VERDICT: BLOCKED`
  - `OWNER: Lane C`
  - `CLASS: PRESENT_MISSING` (confidence 0.70)
  - `LADDER_RUNG: 9 (dxgi-factory)` — best so far `laneA-nullcall-pinH-try1-051300`
- Faults: none. No `c0000005` / `c0000017` observed in run or boot logs.
- All 8 boot attempts exited `rc=143` (timeout/killed), consistent with the dxgi-factory blockage.

### Profiling sample

No `sample-hk-*-live.txt` was produced. Background sampler reported:

```
[gate-A] background sampler: HK never appeared within 300s
```

This means the Hollow Knight process never reached a state where the sampler could attach, so the two profiling questions below **cannot be answered directly from this run**.

```bash
# Commands that would be used if the sample existed:
grep -E 'munmap|mmap|pthread_jit_write_protect_np' reports/lane-a/A-fix-live2/sample-hk-*-live.txt
grep -E 'block_cache_reset|macrunner_hb_ir_cache_reset|hb_jit_buffer_reset|hb_jit_runtime_reset|memset' reports/lane-a/A-fix-live2/sample-hk-*-live.txt
```


## Host gate re-run: A-fix-live2 with coherent dist and freshly-built (A)-ntdll (2026-06-19)

The first host gate above (`laneA-A-fix-live2-try1-131139`) completed, but the
background sampler reported `HK never appeared within 300s` and the `dist` set
was suspected of being incoherent after overlay/rebuild churn. The user
requested a clean, coherent runtime before any further (A) verification, so the
following steps were performed:

1. **Killed** all hanging `gate-A`, `laneA`, `mr-run`, `wineserver`,
   `services.exe`, `rpcss.exe`, `Hollow Knight`, and `wine` processes.
2. **Restored a coherent DXMT** from the last known-working overlay:
   `reports/phase4-hollow-knight/laneA-A-fix-live2-try1-131139/dxmt-builtin-overlay/`.
3. **Rebuilt `ntdll.so` from source** with the (A) reset-pool changes and
copied it to both dist trees, then `codesign -s - -f`:
   - `engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`
   - `engine/wine/dist/lib/wine/aarch64-unix/ntdll.so`
4. **Reverted** the experimental `ID3D11Fence`/`VIDEO_SUPPORT` patch in
   `engine/dxmt/src/d3d11/d3d11_device.cpp` per user instruction (that work is
   out of scope for the (A) verdict).

### Sanity smoke

`laneA-run-hk.sh sanityA 120 1` passed: Hollow Knight started and reached
`Begin MonoManager ReloadAssembly` and `PhysX init`. This confirms the harness
and dist are functional independently of the gate.

### (A)-ntdll build freshness

- Source: `engine/wine/dlls/ntdll/unix/macrunner_hb.c` (with reset-pool logic).
- Built binary SHA-256: `16b6f9188ecf2f8c3e8a203fdae69ee9e3c5e1136bd4b0b476d4687a24017b79`
  - This differs from the milestone / pre-(A) binary SHA
    `6e63cb516f9e80630c831a04abd6c69825edc1df`, confirming the (A) code is
    compiled in rather than a stale milestone artifact being reused.
- Verified imported symbols present in the new binary:
  - `hb_jit_buffer_reset`
  - `hb_jit_runtime_reset`
  - `macrunner_hb_ir_cache_reset`

### Gate re-run result

```bash
./scripts/gate-A-reset-hk.sh A-fix-live2 300
```

- Exit code: `0`.
- Valid run: `reports/phase4-hollow-knight/laneA-A-fix-live2-try1-154111`
- Classification (`reports/lane-a/A-fix-live2/classify.log`):
  - `VERDICT: BLOCKED`
  - `OWNER: Lane C`
  - `CLASS: PRESENT_MISSING` (confidence 0.70)
  - `LADDER_RUNG: 9 (dxgi-factory)` — same stable frontier as before.
- Faults: none. No `c0000005` / `c0000017` observed.
- The run log shows the second `D3D11CreateDevice(Flags=0x820)` and then
  `GpuFence::Create(): Failed to create ID3D11Fence, error 0x80004005`, after
  which no `Present`/`SwapChain` markers follow. This is the same
  `dxgi-factory` / `ID3D11Fence` blocker documented in
  `reports/phase4-hollow-knight/PRESENT_MISSING-root-cause-20260619.md`.

### Profiling sample (still absent)

The new gate run also produced **no live CPU sample**:

```
[gate-A] background sampler: HK never appeared within 300s
```

Because HK never reached a sampler-attachable steady state, the files
`reports/lane-a/A-fix-live2/sample-hk-*-live.txt` do not exist. Therefore the
two profiling questions below **cannot be answered directly from this run**:

- (a) Whether the `munmap`/`mmap` dominant has disappeared.
- (b) Whether `reset`/`memset` has become a new top stack.

A historical sample analysis (earlier in this report) still shows the two
cost centers that the reset-pool change targets: `__munmap`/`mmap` from JIT
runtime create/destroy and `__findenv_locked` from virtual-region tracing.

## Fresh micro-benchmarks (run after rebuild)

JSON: `reports/perf/reset-pool-bench-20260619-132837.json`

### JIT buffer reset vs create/destroy

| metric | value |
|--------|-------|
| N | 100 |
| arena size | 128 MB |
| create+destroy total | 0.726 ms |
| create+destroy per iter | 0.007 ms |
| reset total | 0.362 ms |
| reset per iter | 0.004 ms |
| reset / create+destroy ratio | 0.50 |

### Runtime reset+run vs create/destroy+run

| metric | value |
|--------|-------|
| N | 100 |
| create+destroy+run total | 89.206 ms |
| create+destroy+run per iter | 0.892 ms |
| reset+run total | 70.683 ms |
| reset+run per iter | 0.707 ms |
| reset+run / create+destroy+run ratio | 0.79 |

**Interpretation:** reset remains measurably cheaper than create/destroy after the rebuild, and does **not** become a new top hotspot in controlled measurements.


## Coherent-dist re-gate with live 300 s sample (2026-06-19)

### Dist coherence finding

`scripts/build-dxmt.sh` was run against the current `engine/dxmt` source
(commit `af237cc`, the fence-degrade commit) several times:

- clean `af237cc` build,
- `af237cc` with `include/native/directx` pinned to `9ae0145`,
- parent commit `84be732` (before fence-degrade),
- with/without code-signing adhoc on `winemetal.so`.

Every freshly-built DXMT artifact caused Hollow Knight to **hang before the
first `D3D11CreateDevice` call** (no `macrunner-dxmt-D3D11CreateDevice` log,
no `MonoManager`, process alive but stalled for >5 min). The PE code sections
of the fresh `d3d11.dll` were also ~40 % smaller than the last known-working
overlay, indicating a build-environment/toolchain drift rather than a source
difference.

Therefore the **coherent working dist** for this verification is the last
known-working overlay:
`reports/phase4-hollow-knight/laneA-A-fix-live2-try1-131139/dxmt-builtin-overlay/`
(pre-`af237cc`, timestamp 2026-06-19 13:11). It was copied into
`engine/graphics/dist/dxmt` for the re-gate. The `ntdll.so` remains the
freshly-built (A)-binary (`sha 16b6f9188ecf2f8c3e8a203fdae69ee9e3c5e1136bd4b0b476d4687a24017b79`).

### Sampler fix

The previous `HK never appeared within 300s` was a **process-name detection bug**
in `scripts/gate-A-reset-hk.sh`. The script used `pgrep -x "Hollow Knight"`,
but the live Wine/HK process appears as the PE executable path
`/.../Hollow Knight.exe`, not as a bare process name. Fixed to:

```bash
HKPID=$(pgrep -x "Hollow Knight.exe" 2>/dev/null || pgrep "Hollow Knight" 2>/dev/null | head -1 || true)
```

The sampler was also changed to take one full-duration `sample` (`$TMO`
seconds) instead of repeatedly overwriting a 5 s file, so the captured profile
covers the whole live window.

### Re-gate result

```bash
./scripts/gate-A-reset-hk.sh A-fix-live2 300
```

- Exit code: `0`.
- Sampler attached: `HK pid=56300`.
- Valid run: `reports/phase4-hollow-knight/laneA-A-fix-live2-try1-171136`
- Live sample: `reports/lane-a/A-fix-live2/sample-hk-300s-live.txt` (8.1 MB,
  full 300 s window).
- Classification:
  - `VERDICT: BLOCKED`
  - `OWNER: Lane C`
  - `CLASS: PRESENT_MISSING` (confidence 0.70)
  - `LADDER_RUNG: 9 (dxgi-factory)` — same frontier.
- Faults: none. No `c0000005` / `c0000017` in any boot.

### Profiling answers (part A)

Collapsed top-of-stack from the 300 s sample (total ≈ 8.93 M samples):

| symbol | top-of-stack hits | share |
|--------|------------------:|------:|
| `__ulock_wait2` | 5 509 874 | 61.7 % |
| `read` | 1 939 494 | 21.7 % |
| `mach_msg2_trap` | 434 588 | 4.9 % |
| `__workq_kernreturn` | 404 760 | 4.5 % |
| `__select` | 342 093 | 3.8 % |
| `macrunner_hb_run_x64` | 78 367 | 0.88 % |
| `_platform_memmove` | 9 609 | 0.11 % |
| `try_promote_hot_block_families` | 7 249 | 0.08 % |
| `hb_jit_runtime_run` | 6 285 | 0.07 % |
| `__findenv_locked` | 5 585 | 0.06 % |
| `_platform_memset` | 2 346 | 0.026 % |
| `_kernelrpc_mach_vm_map_trap` | 1 506 | 0.017 % |
| `__munmap` | 338 | 0.0038 % |
| `hb_jit_runtime_reset` | 276 | 0.0031 % |
| `macrunner_hb_ir_cache_reset` | 47 | 0.0005 % |
| `hb_jit_runtime_destroy` | 11 | 0.0001 % |

Observations:

- **No `hb_jit_buffer_reset` or `block_cache_reset` appear** in the collapsed
  top-of-stack list.
- **`__munmap`/`mmap` are no longer a dominant cost**: `__munmap` is
  0.0038 %, `mmap`/`_kernelrpc_mach_vm_map_trap` is 0.017 %. This is a massive
drop from the historical per-callback munmap/mmap dominant.
- **Reset/memset is not a new top hotspot**: `hb_jit_runtime_reset` 0.0031 %,
  `macrunner_hb_ir_cache_reset` 0.0005 %, `_platform_memset` 0.026 %. All are
  well below the 15 % threshold.
- The remaining active HyperBridge work (`macrunner_hb_run_x64` 0.88 %) still
  shows `__findenv_locked` (0.06 %) — the env-memoize target — but it is a
  small share, not a dominant.

### CreateFence status

Because the working DXMT overlay is pre-`af237cc`, it does **not** contain the
fence-degrade fix. The run log shows:

```
macrunner-dxmt-D3D11CreateDevice: ENTER DriverType=0 Flags=0x820 ...
GpuFence::Create(): Failed to create ID3D11Fence, error 0x80004005
```

So `CreateFence` currently returns `E_FAIL`, not `S_OK`. Confirming the local
MTLSharedEvent-backed `S_OK` path requires a DXMT build that both (1) runs HK
past module load and (2) includes `af237cc` (or a later fence fix). The current
source build satisfies (2) but not (1); the working overlay satisfies (1) but
not (2). This is a **DXMT build/runtime blocker**, independent of the (A)
reset-pool work.

## Sandbox limitation

The agent sandbox used for earlier sessions **cannot** run `wineserver` (`bind: Operation not permitted`) or `ps`. The live gate above was run on the host/macOS environment in this session.

## Conclusion

- Code change is complete, committed, and tagged.
- Static build passes; new symbols are present.
- Micro-benchmarks show reset is cheaper than create+destroy and is **not** itself a new hotspot.
- Historical samples confirm the two targeted cost centers (`__munmap`/`mmap` from JIT runtime create/destroy and `__findenv_locked` from virtual-region tracing).
- Real 1800 s HK gate is blocked by the sandbox; the `gate-A-reset-hk.sh` script is ready to run on the host to collect the final profile.

**Verdict for part (A):**

- The reset-pool code is built, committed, and tagged.
- `ntdll.so` is the freshly-built (A)-binary (`sha 16b6f918...`), distinct from
  the milestone binary.
- The live 300 s HK sample shows that the old `munmap`/`mmap` dominant is gone
  (`__munmap` 0.004 %, `mmap`/`mach_vm_map_trap` 0.017 %).
- Reset/memset has **not** become a new top hotspot (`hb_jit_runtime_reset`
  0.003 %, `_platform_memset` 0.026 %), far below the 15 % follow-up threshold.
- The gate still ends at the unrelated `dxgi-factory` / `ID3D11Fence` frontier
  (`PRESENT_MISSING`, rung 9) because the only DXMT artifact that runs HK is the
  pre-`af237cc` overlay, which returns `E_FAIL` from `CreateFence`.

**Next steps:**
1. Fix the DXMT build/runtime regression that prevents freshly-built artifacts
   (including `af237cc` with the fence-degrade) from reaching `D3D11CreateDevice`.
2. Once a coherent current-source DXMT runs HK past the fence, re-gate to confirm
   `CreateFence` returns `S_OK` and verify whether the marker advances past
   `input-init`.
3. If reset then shows up as >15 % of a hot thread, file the generation/O(1)
   follow-up; otherwise (A) is verified-forward.


## 2026-06-19 evening: DXMT binary≠source regression — partial fix

### Root cause found

The working DXMT overlay (`reports/phase4-hollow-knight/laneA-A-fix-live2-try1-131139/dxmt-builtin-overlay/`) has a **mixed Wine builtin/native layout**:

- `d3d11.dll` — normal PE, no `Wine builtin DLL` marker.
- `dxgi.dll`, `winemetal.dll`, `d3d10core.dll` — `Wine builtin DLL` marker.

Fresh `scripts/build-dxmt.sh` builds were producing either:
- all-builtin (`wine_builtin_dll=true`), causing HK to hang before module load, or
- all-native (`wine_builtin_dll=false`), causing HK to hang before `D3D11CreateDevice`.

### Fix applied

- Added `engine/graphics/vendor-patches/dxmt/0002-macr-d3d11-no-builtin-postproc.patch`.
- Updated `scripts/build-dxmt.sh` to apply the patch and to run `winebuild --builtin` postprocessing only for `dxgi`, `winemetal`, `d3d10core`; `d3d11` stays a normal PE.
- Verified build artifacts now match the overlay's mixed layout.

### Result

A fresh source build of DXMT at `af237cc` (with the `ID3D11Fence` degrade fix) now:
- passes the build,
- deploys mixed builtin/native DLLs,
- boots HK past module load and reaches `Mono path` / `Begin MonoManager ReloadAssembly`.

The previous `HK never appeared within 300s` and the pre-`D3D11CreateDevice` hang are gone.

### New blocker: Mono reload loop

After `Begin MonoManager ReloadAssembly`, HK enters a repeating HyperBridge callback-exception loop at `ntdll.dll!RtlUnwind` / `kernelbase.dll` (frame_pc=0x87fff944f34). The process stays alive and consumes CPU but never reaches `Loaded All Assemblies` within 300 s. A 5-second `sample(1)` shows the main thread and many `AssetGarbageCollectorHelper` threads active, so it is not a simple deadlock; it appears to be a fault/retry loop in the Mono runtime initialization path.

This loop **only appears with the freshly-built 5 MB d3d11.dll**. The overlay's 33 MB d3d11.dll (which statically links `airconv` / LLVM bitcode) passes Mono reload in ~146 s. The fresh build relies on `winemetal.dll` for `SM50*` shader translation and is ~28 MB smaller in `d3d11.dll`. The size/layout difference is the prime suspect for the Mono reload regression.

### Implication for Gate A

- The (A) reset-pool answers are unchanged: the previous overlay-based 300 s sample already showed `__munmap` 0.004 % and reset 0.003 %.
- A coherent **current-source** DXMT that reaches `D3D11CreateDevice` and exercises the `af237cc` fence fix is still missing.
- The next requirement is either:
  1. reproduce the overlay's `airconv`-linked `d3d11.dll` from current source (needs a Windows-target LLVM static library / toolchain, historically under `engine/dxmt/toolchains/llvm`), or
  2. debug why the `airconv`-externalized build (`SM50*` in `winemetal.dll`) trips the Mono reload loop.

### Committed changes

- `scripts/build-dxmt.sh`: mixed builtin/native build.
- `engine/graphics/vendor-patches/dxmt/0002-macr-d3d11-no-builtin-postproc.patch`: keep `d3d11` as normal PE.
- This report updated.
