# CODEX MEGA-PROGRAM — living status (resume anchor)

**Program:** `docs/CODEX-MEGA-PROGRAM-x64-engine-to-real-games-6month.md`
**Rule:** at the START of every run, read this file, resume the FIRST phase not marked DONE, and
chain forward through all remaining phases without stopping for approval (see AUTONOMY section of
the program). Update this file as each gate is passed. **NEXT** below is always the resume point.

---

## NEXT → Phase 3 formal Tier-1 gate in progress — Hollow Knight x64 bring-up

**Tier-1 game present (GOG, DRM-free):** Hollow Knight 1.5.12620 (64-bit) at
`/Users/timurtoby/Documents/MacRunner/Main/game-hollow.knight-(89718)/setup_hollow_knight_1.5.12620_(64bit)_(89718).exe`
(624M GOG InnoSetup installer + `Bonus/`). It's a sibling of the repo (outside `MacRunner/`) — use
the absolute path; do NOT copy 600M into the repo.
- **Extraction done:** `reports/phase4-hollow-knight/innoextract-20260530-200352.log` extracted to
  `…/game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe` (Unity engine:
  `UnityPlayer.dll`, `Hollow Knight_Data`, MonoBleedingEdge).
- **2026-05-30/31 bring-up progress:** `scripts/mr-run.sh` now has macOS `timeout` fallback. Hollow
  Knight reaches and returns successfully from `UnityPlayer.dll` x64 `PROCESS_ATTACH` under
  HyperBridge. Cleared blockers: API-set target resolution/module map, dynamic guest
  `GetProcAddress`, ARM64X `.hexpthk` wrapping, localization/fibers/winrt API-set fallbacks,
  heap/TLS/FLS/LastError, startup/stdout/file type/command line/NLS/LCMap/environment strings,
  dynamic kernel semantic thunks, local heap ownership tracking, critical-section semantics,
  time/SystemTime conversions, and UnityPlayer SIMD families (`sqrt`, `rsqrt/rcp`, packed
  `mul/div`, `shufps/shufpd`, XMM high/low qword lane moves). Additional 2026-05-31 blockers cleared:
  TEB stack bounds for x64 `__chkstk`, SList, virtual memory, event/error/debug families,
  WinRT init, `CommandLineToArgvW`, file attributes, local file handles, `GetNativeSystemInfo`,
  `GlobalMemoryStatusEx`, and x64 `CMPXCHG8B/CMPXCHG16B` (`0F C7 /1`, trigger
  `f0 48 0f c7 4e 40`). HyperBridge test runner is green at `315 passed, 0 failed`; fast validation
  `phase1_core` PASS.
- **Current blocker:** latest validated run
  `reports/phase4-hollow-knight/run-20260531-032224-cmpxchg16b-family/` no longer hits
  `0xc000001d` or unsupported opcodes; it reaches Unity memory configuration output and times out
  (`rc=143`) after 709 successful x64 import handoffs. Screenshots are still desktop-only and no
  Hollow Knight window/rendered frame is visible. Follow-up probe
  `reports/phase4-hollow-knight/run-20260531-033055-xtajit64-import-loop-probe/` shows no crash or
  unsupported opcode; imports repeat around `EnterCriticalSection`/`LeaveCriticalSection`/`WriteFile`
  /`OutputDebugStringW` after Unity memory config. Hot-path xtajit64 logging is now gated behind
  `MACRUNNER_HB_TRACE_XTAJIT64=1`; next action is a longer low-noise run to distinguish slow startup
  from a real import/logging loop.
- **Phase 3 gate still NOT passed:** no main menu, input, audio, or rendered frame yet; latest
  screenshots are desktop-only with no game window.
- **Run hygiene:** `scripts/mr-run.sh` for runs, `scripts/mr-clean.sh --prune` after each batch.

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
  x64 `PROCESS_ATTACH` under HyperBridge; gate remains open until main menu + input + audio +
  rendered frame are captured under the spike build.

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
