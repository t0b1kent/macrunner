# CODEX MEGA-PROGRAM — living status (resume anchor)

**Program:** `docs/CODEX-MEGA-PROGRAM-x64-engine-to-real-games-6month.md`
**Rule:** at the START of every run, read this file, resume the FIRST phase not marked DONE, and
chain forward through all remaining phases without stopping for approval (see AUTONOMY section of
the program). Update this file as each gate is passed. **NEXT** below is always the resume point.

---

## NEXT → Phase 3 formal Tier-1 gate in progress — loader-gated explicit-JIT fallback-zero; UnityPlayer post-Mono CPU/no-window blocker

**Tier-1 game present (GOG, DRM-free):** Hollow Knight 1.5.12620 (64-bit) at
`/Users/timurtoby/Documents/MacRunner/Main/game-hollow.knight-(89718)/setup_hollow_knight_1.5.12620_(64bit)_(89718).exe`
(624M GOG InnoSetup installer + `Bonus/`). It's a sibling of the repo (outside `MacRunner/`) — use
the absolute path; do NOT copy 600M into the repo.
- **Extraction done:** `reports/phase4-hollow-knight/innoextract-20260530-200352.log` extracted to
  `…/game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe` (Unity engine:
  `UnityPlayer.dll`, `Hollow Knight_Data`, MonoBleedingEdge).
- **2026-05-30/31 bring-up progress:** `scripts/mr-run.sh` now has macOS `timeout` fallback. Hollow
  Knight reaches and returns successfully from `UnityPlayer.dll` x64 `PROCESS_ATTACH` under
  HyperBridge and continues past Unity allocator startup. Cleared blockers: API-set target resolution/module map, dynamic guest
  `GetProcAddress`, ARM64X `.hexpthk` wrapping, localization/fibers/winrt API-set fallbacks,
  heap/TLS/FLS/LastError, startup/stdout/file type/command line/NLS/LCMap/environment strings,
  dynamic kernel semantic thunks, local heap ownership tracking, critical-section semantics,
  time/SystemTime conversions, and UnityPlayer SIMD families (`sqrt`, `rsqrt/rcp`, packed
  `mul/div`, `shufps/shufpd`, XMM high/low qword lane moves). Additional 2026-05-31 blockers cleared:
  TEB stack bounds for x64 `__chkstk`, SList, virtual memory, event/error/debug families,
  WinRT init, `CommandLineToArgvW`, file attributes, local file handles, `GetNativeSystemInfo`,
  `GlobalMemoryStatusEx`, x64 `CMPXCHG8B/CMPXCHG16B` (`0F C7 /1`, trigger `f0 48 0f c7 4e 40`),
  chunked x64 code fetch, SRW-lock and condition-variable host-boundary leaks, native fallback
  for ordinary `Heap*` imports, and the bit-scan family decoder bug where bare `0F BC` (BSF) was
  incorrectly treated as `F3 0F BC` (TZCNT). That BSF/TZCNT fix cleared the Unity small-allocator
  sentinel-bucket crash at `UnityPlayer.dll` RVAs `0x2afe1b`/`0x2b0069`; HyperBridge tests and
  `tools/hb_oracle/fast_validate_family.sh phase1_core` pass after the fix.
- **Current blocker (2026-05-31 18:35 local):** Phase 3 is still no-window/no-menu. Important
  evidence correction: this lane needs both gates: `MACRUNNER_HB_X64_LOADER=1` to route the AMD64
  PE through HyperBridge and `MACRUNNER_HB_BACKEND=jit` to select the JIT. Runs missing the loader
  gate exit early in ARM64/ARM64EC loader startup (`load_ntdll_functions` reports missing
  ARM64EC-only exports); runs missing the backend gate are interpreter-path evidence, not proof of
  the JIT backend. Loader-gated explicit-JIT probes showed zero codegen fallbacks but severe
  startup throughput loss from per-block whole-buffer W^X flips
  (`hb_jit_buffer_commit`/`hb_jit_buffer_make_writable` -> `__mprotect`). The current fix uses the
  macOS MAP_JIT thread write-protect API (`pthread_jit_write_protect_np`) plus dirty-range icache
  flushing, preserving the fallback safety net while removing the mprotect hot path. Cleared since
  `run-20260531-092812-cotaskmem-fix-phase3-probe/`: `CreateDirectoryW`, advapi/EventProvider ETW
  no-op semantics, current-process `VirtualAlloc/VirtualProtect/VirtualFree` replay into imported
  x64 contexts, xtajit64 Unix-side memory notifications, live VM write fallback for writable guest
  regions backed by RX Mach pages, `mr-run.sh` internal `config/env.sh` sourcing, JIT scalar load
  width/register semantics, FS/GS/RIP-relative scalar memory operand resolution, indirect
  `CALL/JMP` operand targets, persistent per-run JIT runtime fallback-to-interpreter handling, and
  explicit JIT codegen cases for every interpreter-supported `HB_IR_*` op (correctness-first helper
  route where native emit is not yet promoted), plus the MAP_JIT W^X fix. HyperBridge tests:
  `325 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS after the
  JIT fixes and spike `ntdll.so` relink. Latest loader-gated explicit-JIT evidence:
  `reports/phase4-hollow-knight/run-20260531-190238-phase3-loader-jit-blockmap-sampled/` ran 300s
  with `MACRUNNER_HB_X64_LOADER=1 MACRUNNER_HB_BACKEND=jit`, reached Unity memory setup and Mono
  paths, traced 7,955 JIT blocks, and had zero `macrunner-hb-jit-fallback`, `JIT codegen failed`,
  `JIT helper fault`, `JIT buffer exhausted`, `MEMORY_FAULT`, `UNSUPPORTED_OPCODE`, or
  `runtime-fail`. Samples show the main macOS thread in `CFRunLoop`, seven
  `AssetGarbageCollectorHelper` workers in `NtWaitForSingleObject`, and the active x64 guest stack
  dominated by UnityPlayer guest PC `0x7ffd07cc548` (module base `0x7ffd0340000`, RVA `0x48c548`,
  epilogue of a UnityPlayer helper). A follow-up hot-block probe
  (`run-20260531-192827-phase3-loader-jit-hotbytes/`) kept fallback-zero and identified the actual
  helper-heavy throughput pattern: dynamic Mono code heap blocks around `0x87ef...` running
  byte/word string-scan loops such as `inc rax; cmp byte/word [base+index], 0/value; jne self`.
  No game window yet. Do not patch wait semantics speculatively:
  `run-20260531-175000-phase3-wait-resume-trace/` showed suspended workers resume successfully and
  then idle on companion waits. Next codegen pass should promote the hot scalar loop family
  (`ADD/CMP-or-TEST/Jcc`, byte/word memory compare, self-branch) away from helper-heavy codegen
  while preserving loader-gated explicit-JIT fallback-zero.
- **Phase 3 gate still NOT passed:** no main menu, input, audio, or rendered frame yet; latest
  screenshots are desktop-only with no game window.
- **Run hygiene:** `scripts/mr-run.sh` for runs, `scripts/mr-clean.sh --prune` after each batch.

### ★ TOP PRIORITY (operator-directed 2026-05-31) — BULK JIT CODEGEN COVERAGE
This is the fastest route to the Hollow Knight window: it's throughput-bound on JIT fallbacks.
The IR-op set is finite (163 ops in `engine/hyperbridge/include/hb_ir.h`); the interpreter already
implements ALL of them; JIT codegen covers only ~65 → the rest fall back to the slow interpreter.
**Do it in bulk:** for every interpreter-supported IR op, add the ARM64 codegen in
`engine/hyperbridge/src/hb_arm64_codegen.c`, diff JIT-vs-interpreter/oracle per op, drive JIT
fallbacks on the Hollow Knight hot path to zero. Deliverable:
`reports/research/HB-JIT-CODEGEN-COVERAGE-matrix.md`. See "★ PRIORITY INSERT #2 — BULK JIT CODEGEN
COVERAGE" in the program doc.

Status 2026-05-31 19:30: first bulk pass published in
`reports/research/HB-JIT-CODEGEN-COVERAGE-matrix.md`; loader-gated explicit-JIT Hollow Knight run
(`run-20260531-192827-phase3-loader-jit-hotbytes/`) shows fallback-zero after the MAP_JIT W^X fix
and identifies hot dynamic Mono string-scan loops. Promote helper-backed scalar `ADD/CMP/Jcc`
loop blocks to native emit first; do not regress the
`MACRUNNER_HB_X64_LOADER=1 MACRUNNER_HB_BACKEND=jit` fallback-zero gate.

### ★ ALSO ACTIVE (parallel) — BULK ISA COVERAGE (operator-directed 2026-05-31)
Stop chasing one opcode per game-run. Proactively cover the whole x86-64 ISA using the
ALREADY-vendored `engine/wine/libs/capstone` as the decode reference + golden oracle for semantics.
See the "★ PRIORITY INSERT — BULK ISA COVERAGE" section in
`docs/CODEX-MEGA-PROGRAM-x64-engine-to-real-games-6month.md`. Deliverable:
`reports/research/HB-X64-ISA-COVERAGE-matrix.md` + capstone-diff/oracle-diff fuzz wired into CI.
Run in parallel with Hollow Knight bring-up.

---

## Phase ledger

### Phase 0 — close out residual fault — ✅ DONE (2026-05-30, verified)
- Fix: TLS-aware normalization of x64 callback targets in
  `engine/wine/dlls/ntdll/unix/macrunner_hb.c` (`macrunner_hb_dispatch_x64_callback`) +
  `signal_arm64.c` fallback. Reads `AddressOfCallBacks` from the AMD64 TLS dir, snaps near-padding
  PCs (0x14000150f → 0x140001510) to the real callback entry.
- Evidence (operator-verified): `reports/arm64ec-phase0-run-hello_x64-20260530-143933.log` →
  `hello from windows pe`, `run_exit=0`, `cleanup_exit=0`, ZERO runtime-fail/MEMORY_FAULT/c000007b;
  `stdout_stderr_x64` same. Trace: original=0x14000150f → target=0x140001510.

### Phase 1 — x86_64 ISA completeness & correctness — ✅ DONE (2026-05-30, verified)
Integer ISA + flags correctness (diff vs golden oracle), SSE→SSE4.2 + **AVX/AVX2**, x87 edge
cases, atomics + ARM weak-memory mapping (x86 TSO → ARM64 barriers), SEH across EC boundary.
Gate: expanded torture/fuzz suite passes vs golden oracle; 3+ non-trivial x64 console programs
(threads+SSE+exceptions) run correct.
- Evidence: `reports/phase1-gate-summary-20260530-155351.txt`:
  `hb_runner_rc=0`, `phase1_core_rc=0`,
  `phase1_sse_x64 rc=0`, `phase1_threads_x64 rc=0`, `phase1_exception_x64 rc=0`.
- Fixes in this pass: E0-E3 LOOP/JRCXZ family across decode/lift/interp/JIT/tests; x64 import
  semantics for thread creation/wait/handle APIs; x64 vectored exception semantic dispatch for
  `AddVectoredExceptionHandler`/`RaiseException`/`RemoveVectoredExceptionHandler`.

### Phase 2 — interpreter → JIT + translation cache — ✅ DONE (2026-05-30, verified)
- Gate evidence: `reports/HYPERBRIDGE-PHASE2-BENCH.md` from `scripts/bench-hyperbridge.sh`:
  cached-block JIT `29.07x` over interpreter (`960000` ops), per-block compile
  `on-demand`, block chaining `pc-target loop in hb_jit_runtime_run`.
- AOT/cache evidence: `reports/HYPERBRIDGE-TRANSLATION-CACHE.md`:
  `cold_cache_status=miss`, `warm_cache_status=hit`, `json_entries=1`.
- Verification: `reports/phase2-verify-hyperbridge-20260530-160301.log`:
  `Pass: 17`, `Errors: 0`, `HyperBridge verification PASSED`.

### Phase 3 — runtime/Win32 surface (input/audio/DXMT integration) — ⚠ FORMAL GATE IN PROGRESS (2026-05-30)
- Fixes in this pass:
  `ntdll/unix/macrunner_hb.c` x64 import semantics for `LoadLibraryA/W`,
  `LoadLibraryExA/W`, `FreeLibrary`, `GetProcAddress`, `Get/SetEnvironmentVariableA/W`;
  gated synthetic D3D mock modules for `d3d11.dll`/`d3d12.dll`/`dxgi.dll`;
  x64 UCRT `__stdio_common_vfprintf` semantic for guest va_list rendering.
  `ntdll/loader.c` semantic import stub list updated for dynamic loader/env APIs.
- Input/audio evidence: `reports/phase3-runtime/phase3-runtime-suite-20260530-170959-summary.jsonl`:
  keyboard rc=0 marker `input ok`; raw input rc=0 marker `input ok`;
  XInput marker `xinput probe ok`; waveOut marker `audio enum ok`.
- D3D evidence:
  `reports/phase3-runtime/direct-d3d11-vfprintf-final-20260530-170803-summary.json`
  (`rc=0`, marker `d3d11 triangle ok`, `trace_lines=11`);
  replay `artifacts/phase3/direct-d3d11-vfprintf-final-20260530-170803/replay/replay.json`
  (`status=PASS`, `present_count=1`, `non_background_pixels=1152`, `unsupported_calls=0`,
  PPM `d3d-trace.ppm`).
- Regression evidence:
  `reports/phase3-runtime/post-phase3-regression-20260530-171203.log`:
  `305 passed, 0 failed`, `FAST VALIDATION: PASS`;
  `reports/phase3-runtime/post-phase3-phase0-20260530-171220-summary.jsonl`:
  `hello_x64` rc=0, `stdout_stderr_x64` rc=0.
- Formal gate active on Hollow Knight x64. Current evidence includes successful `UnityPlayer.dll`
  x64 `PROCESS_ATTACH`, Unity memory configuration, Mono path/config startup, D3D module loads,
  cleared Mono code-heap memory writes, and a hardened JIT route through scalar memory and indirect
  branch families, all interpreter-supported `HB_IR_*` ops having explicit codegen cases, and the
  MAP_JIT W^X fix for explicit-JIT throughput. HyperBridge tests: `325 passed, 0 failed`;
  `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS after the W^X fix and spike
  `ntdll.so` relink. Latest explicit-JIT evidence:
  `reports/phase4-hollow-knight/run-20260531-190238-phase3-loader-jit-blockmap-sampled/` ran 300s
  with `MACRUNNER_HB_X64_LOADER=1 MACRUNNER_HB_BACKEND=jit`, reached Unity memory setup and Mono
  paths, traced 7,955 JIT blocks, and had zero
  `macrunner-hb-jit-fallback`, `JIT codegen failed`, `JIT helper fault`, `JIT buffer exhausted`,
  `MEMORY_FAULT`, `UNSUPPORTED_OPCODE`, or `runtime-fail`. No game window appeared and all window
  captures remain absent; next evidence pass is UnityPlayer/Mono post-bootstrap CPU ownership while
  preserving loader-gated explicit-JIT fallback-zero. Gate remains open until main menu + input +
  audio + rendered frame are captured under the spike build.

### Phase 4 — green-list bring-up ladder — ⏳ PENDING after Phase 3 Hollow Knight gate
Gate requires ≥5 Tier-1 games playable start→gameplay for ≥30 min each. Start after Hollow Knight
passes the Phase 3 menu/input/audio/rendered-frame gate.

### Phase 5 — hardening / fuzz / regression / CI — ⬜ partial continuous checks passed; full gate pending
Available post-Phase3 regression checks are green (see Phase 3 regression evidence). Formal gate
still requires green CI across the full game/regression suite after Phase 4 assets exist.

---

## Milestone protection (do not regress)
Plain x86_64 PE runs end-to-end (run_exit=0) via ARM64EC. Archived:
`archives/milestone-arm64ec-x64-e2e-20260530.tar.gz` (local + external MacRunner-ARCHIVES,
sha ccb34a25…). If any phase risks regressing this, STOP and escalate.
