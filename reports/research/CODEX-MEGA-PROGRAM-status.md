# CODEX MEGA-PROGRAM — living status (resume anchor)

**Program:** `docs/CODEX-MEGA-PROGRAM-x64-engine-to-real-games-6month.md`
**Rule:** at the START of every run, read this file, resume the FIRST phase not marked DONE, and
chain forward through all remaining phases without stopping for approval (see AUTONOMY section of
the program). Update this file as each gate is passed. **NEXT** below is always the resume point.

---

## NEXT → split: (1) operator provides a game asset; (2) MEANWHILE do all game-independent work.

**Operator action (the ONE external blocker):** drop a real DRM-free game into the workspace —
GOG offline-installer / install folder preferred (Stardew Valley, Hollow Knight, Hades), then the
formal Phase 3/4 gate (boot → main menu w/ input+audio+rendered frame) and the Phase 4 ladder
resume with NO approval stop.

**Codex — do NOT idle while waiting for the game. A blocked-on-asset state means: do every
remaining game-INDEPENDENT task first, yield only after those are exhausted.** Specifically:
1. **Real heavy-app gate (proxy for a game, assets already present):** bring up **Notepad++ x64**
   (`./npp.8.9.5.Installer.x64.exe` → install under a prefix) and/or **KeePass** / Win7 `calc.exe`
   — real Win32/GUI x64 apps, far beyond hello-world. Drive to: launches, window renders, basic
   interaction. This shakes out GUI/USER32/GDI/DX paths a game also needs. Log evidence.
2. **Phase 5 (no game needed):** differential fuzz decoder/lifter vs golden oracle; broaden the
   ISA torture suite; crash-RIP→module+offset symbolication; perf-regression gate; soak tests.
3. **Bring-up harness:** a script that, given a game path, sets up the prefix + launches under the
   spike build with capture, so a dropped-in game runs in one command.
Only escalate again once 1-3 are done AND no game asset exists.

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

### Phase 3 — runtime/Win32 surface (input/audio/DXMT integration) — ⚠ SURROGATE PASS / FORMAL GATE BLOCKED (2026-05-30)
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
- Formal gate blocker: Phase 3 requires FIRST Tier-1 game (Hades/Stardew) at main menu with input
  + audio + rendered frame screenshot/log. Local asset scan found only badge SVGs:
  `reports/compat-public/badges/{hades,stardew-valley,hollow-knight,dead-cells,celeste}.svg`;
  no game executable/install asset is present in this workspace.

### Phase 4 — green-list bring-up ladder (Hades/Stardew/…) — ⛔ BLOCKED on same Tier-1 game assets
Gate requires ≥5 Tier-1 games playable start→gameplay for ≥30 min each. Cannot execute without
real game assets/install paths.

### Phase 5 — hardening / fuzz / regression / CI — ⬜ partial continuous checks passed; full gate pending
Available post-Phase3 regression checks are green (see Phase 3 regression evidence). Formal gate
still requires green CI across the full game/regression suite after Phase 4 assets exist.

---

## Milestone protection (do not regress)
Plain x86_64 PE runs end-to-end (run_exit=0) via ARM64EC. Archived:
`archives/milestone-arm64ec-x64-e2e-20260530.tar.gz` (local + external MacRunner-ARCHIVES,
sha ccb34a25…). If any phase risks regressing this, STOP and escalate.
