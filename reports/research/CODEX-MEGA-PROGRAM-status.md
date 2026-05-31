# CODEX MEGA-PROGRAM — living status (resume anchor)

**Program:** `docs/CODEX-MEGA-PROGRAM-x64-engine-to-real-games-6month.md`
**Rule:** at the START of every run, read this file, resume the FIRST phase not marked DONE, and
chain forward through all remaining phases without stopping for approval (see AUTONOMY section of
the program). Update this file as each gate is passed. **NEXT** below is always the resume point.

---

## NEXT → Phase 3 Hollow Knight — bulk JIT codegen coverage / hot Mono-Unity helper elimination

**Resume checkpoint (2026-05-31 22:23 local):** Hollow Knight remains loader-gated explicit-JIT
fallback/fault/unsupported-zero after scalar `MOV`, stack-control, extend, packed XMM move, near
conditional-PC, and zero-test Jcc native promotion. Current blocker is still throughput/no-window in hot
Mono/Unity code. Next evidence-backed finite target: inspect rank3 prologue/local-store block
`0x87ef2ba32d4` or rank1 memory-test/RMW loop fusion after rank8 was reduced. Do not keep widening
zero-test without new hot evidence. Keep
`reports/research/HB-JIT-CODEGEN-COVERAGE-matrix.md` and
`reports/research/HB-X64-ISA-COVERAGE-matrix.md` current, validate JIT-vs-interpreter/oracle per family,
run Hollow Knight with `scripts/mr-run.sh`, and prune with `scripts/mr-clean.sh --prune`.

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
  then idle on companion waits. Current codegen pass is promoting the hot scalar loop family
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

Status 2026-05-31 19:48: native ARM64 peephole for the hot scalar scan-loop family
(`ADD reg,1; CMP mem8/mem16,reg-or-imm; Jcc self`) is under validation. Unit coverage now has byte
and word scan-loop JIT-vs-runtime checks; `engine/hyperbridge/tests/hb_test_runner` => `327 passed,
0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS. Next: rebuild/relink the
spike `ntdll.so`, run loader-gated Hollow Knight, and compare hot-block/fallback counters.

Status 2026-05-31 20:08: scalar native JIT family expanded beyond the first scan-loop peephole:
plain-GPR/imm `ADD/SUB/AND/OR/XOR`, `CMP/TEST`, and adjacent `E/NE Jcc` pairs now emit native
ARM64 and record lazy flags without condition-helper fallback. Coverage:
`engine/hyperbridge/tests/hb_test_runner` => `332 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` => PASS; spike `ntdll.so` relinked. Hollow
Knight probes:
`run-20260531-195003-phase3-scanloop-jit/`,
`run-20260531-200058-phase3-scalar-native-jit/`, and
`run-20260531-200447-phase3-scalar-jcc-pair-jit/` all timeout cleanly with cleanup/prune `0` and
zero `macrunner-hb-jit-fallback`, `JIT codegen failed`, `JIT helper fault`, `UNSUPPORTED_OPCODE`,
`MEMORY_FAULT`, `runtime-fail`, or `JIT buffer exhausted`. Pair path improves the top counted-loop
guard blocks (`SUB/JNE` 372→292, `CMP/Jcc` 352→272 versus scalar-native-only) but remaining hot
copy/scan bodies are still large; next target is loop fusion or a smaller lazy-flag record path for
the `LOAD/STORE/INC/TEST/JE` + `SUB/JNE` two-block copy-loop family.

Status 2026-05-31 20:19: compact immediate emission added for JIT mask/lazy-flag recording so hot
native scalar blocks no longer materialize every small constant with four ARM64 instructions.
Coverage remains `engine/hyperbridge/tests/hb_test_runner` => `332 passed, 0 failed` plus
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; tests now assert code-size caps for the
scan-loop and scalar-branch peepholes. Spike `ntdll.so` relinked. Hollow Knight
`run-20260531-201543-phase3-compact-lazy-jit/` timed out cleanly (`MR_RUN_RC=143`) with
cleanup/prune `0` and zero JIT fallback/fault/unsupported counters. At the same 23k hot-block
sample, JIT buffer use improved `260384 -> 203664`; top hot blocks shrank: rank1 `292 -> 208`,
rank2 `508 -> 328`, rank3 `272 -> 188`, rank4 `520 -> 328`. Next evidence-backed target remains
real loop fusion for the two-block byte copy/scan family (`LOAD/STORE/INC/TEST/JE` plus `SUB/JNE`).

Status 2026-05-31 20:26: block-local native copy-scan body peephole added for
`LOAD byte; STORE byte; INC index; TEST byte; JE/JNE`, skipping dead `INC` lazy-flag recording that
is immediately overwritten by `TEST`. Coverage: `engine/hyperbridge/tests/hb_test_runner` =>
`334 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike
`ntdll.so` relinked. Hollow Knight `run-20260531-202253-phase3-copy-scan-jit/` timed out cleanly
(`MR_RUN_RC=143`) with cleanup/prune `0` and zero JIT fallback/fault/unsupported counters. Rank-2
copy-scan body shrank `328 -> 220` versus compact-lazy-only at the same 23k hot-block sample. JIT
buffer used is roughly flat (`203664 -> 203552`) because this optimized one hot compiled body, not
block count; remaining throughput work should fuse the rank1 `SUB/JNE` guard with the rank2 body or
add direct block chaining for the backedge.

Status 2026-05-31 20:45: persistent JIT block-cache promotion now fuses the real one-block-at-a-time
x64 loader shape for `LOAD/STORE/INC/TEST/JE` body + `SUB/JNE` count guard. The first CFG-only
attempt validated locally but did not trigger in Hollow Knight because `hb_lift_func_x64` still emits
single-block functions; the cache promotion regenerates the body cache entry once the guard and body
are both present. Correctness fix included: the copy-scan peephole now writes the loaded `AL/AX` back
to the guest register file. Coverage: `engine/hyperbridge/tests/hb_test_runner` => `337 passed, 0
failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked.
Hollow Knight `run-20260531-204546-phase3-cache-promote-copy-scan-jit/` timed out cleanly
(`MR_RUN_RC=143`) with cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and 2
`macrunner-hb-jit-fusion` events including body `0x87ef2bdf604` + guard `0x87ef2bdf612`. Hot-block
dispatch sample dropped from 23k to 12k at the same cap; the old rank1/rank2 copy loop disappeared.
NEXT: apply the same two-block promotion to the new top bounded byte-scan family:
`CMP rax,rdx; JE exit` guard plus `INC rax; CMP byte [rax+rcx],0; JNE guard` body.

Status 2026-05-31 21:05: bounded byte-scan and store/count-loop hot families promoted. Added
persistent-cache two-block fusion for `CMP rax,rdx; JE exit` + `INC rax; CMP byte [rax+rcx],0; JNE
guard`, plus block-local native emit for `STORE byte [ptr],src; INC counter; INC ptr; CMP
counter,limit; JB self` (both immediate and register limits, including source byte = counter low
byte). Coverage: `engine/hyperbridge/tests/hb_test_runner` => `340 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow
Knight `run-20260531-205913-phase3-bounded-scan-jit/` and
`run-20260531-210549-phase3-store-count-loop-jit/` both timed out cleanly (`MR_RUN_RC=143`) with
cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and 4 fusion events. Hot dispatch
sample moved `23k -> 12k -> 11k -> 10k`; top copy/bounded/store loops are no longer dominant. NEXT:
inspect the remaining top branchy memory-test/prologue blocks (`0x87ef2bf915f`,
`0x87ef2ba32f8`, `0x87ef2ba32d4`) and promote only evidence-backed finite families.

Status 2026-05-31 21:16: committed hot-loop promotion batch at `a41565b` and ran longer
post-hotloop validation: `run-20260531-211135-phase3-post-hotloop-300s-jit/` (`MR_RUN_RC=143`,
cleanup/prune `0`). Fallback/fault counters remain zero; log reaches Unity memory config and Mono
paths (`Hollow Knight_Data/Managed`, `MonoBleedingEdge/etc`) but still no game window. With lower
hot-block tracing overhead the sample remains around 10k dispatches; current top is branchy
memory-test/control code (`TEST byte [rdx],imm; JE`, RIP-relative `CMP/Jcc`, and nearby prologue /
epilogue blocks), so continue with evidence-backed native promotion for finite memory-test/Jcc and
small branch/control families rather than widening speculative patches.

Status 2026-05-31 21:29: direct-memory logical RMW and memory-immediate branch pairs promoted.
Native JIT now covers `AND/OR/XOR r/m,reg-or-imm` direct-memory read/modify/write and the hot
`TEST/CMP direct-mem,imm; E/NE Jcc` pair. Coverage:
`engine/hyperbridge/tests/hb_test_runner` => `343 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow
Knight `run-20260531-212945-phase3-memimm-jcc-jit/` timed out cleanly (`MR_RUN_RC=143`) with
cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot block
sizes improved for the evidenced memory branch family: `TEST byte [rdx],1; JE` `220 -> 200`, and
RIP/absolute `CMP dword [abs],0; JNE` `276 -> 264`; total dispatch remains around 10k. NEXT:
continue from the still-hot branch/prologue bodies (`0x87ef2ba32d4`, `0x87ef2bf98b4`) and only
promote finite families that reduce helper calls or dispatch count without growing the common path.

Status 2026-05-31 21:55: scalar `MOV` and stack-control hot families promoted. Native JIT now covers
8/16/32/64-bit scalar `HB_IR_MOV` for GPR reg/imm plus gated direct-memory load/store siblings, and
gated direct-stack `PUSH`, `POP`, direct `CALL` return pushes, and `RET`/`RET imm16` while retaining
helper fallback outside `MACRUNNER_HB_JIT_DIRECT_MEM`. Coverage:
`engine/hyperbridge/tests/hb_test_runner` => `346 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow Knight
`run-20260531-214111-phase3-scalar-mov-jit/` and
`run-20260531-214911-phase3-stack-control-jit/` timed out cleanly (`MR_RUN_RC=143`) with cleanup/prune
`0`, zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot prologue/epilogue
native bodies shrank: `0x87ef2bf98b4` `500 -> 404 -> 304`, `0x87ef2ba32d4` `440 -> 388`,
`0x87ef2ba335c` `336 -> 284`, `0x87ef2bf9919` `328 -> 276`. NEXT: native
`HB_IR_ZERO_EXTEND`/`HB_IR_SIGN_EXTEND` for hot `MOVZX` blocks (`0f b6 d3`, `0f b7 04 51`), then
rerun the same fallback-zero Hollow Knight gate.

Status 2026-05-31 22:01: `HB_IR_ZERO_EXTEND`/`HB_IR_SIGN_EXTEND` promoted for GPR and gated
direct-memory operands. A first test run caught and fixed a native sign-extend bug where 32-bit
destinations wrote a full 64-bit negative value instead of truncating through destination width like
`hb_context_write_reg_value_sized`. Coverage: `engine/hyperbridge/tests/hb_test_runner` =>
`347 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so`
relinked. Hollow Knight `run-20260531-215737-phase3-extend-jit/` timed out cleanly (`MR_RUN_RC=143`)
with cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot
`MOVZX` block `0x87ef2bf98d8` shrank `268 -> 244`; `0x87ef2bf98e7` remained size-neutral. NEXT:
inspect packed XMM move/direct-memory feasibility for `0x87ef2ba3301` or fuse the rank1
memory-test/RMW loop; do not broaden scalar extend without new evidence.

Status 2026-05-31 22:09: packed XMM move hot family promoted. Native JIT now covers 128-bit XMM
`HB_IR_LOAD`/`HB_IR_STORE` plus XMM reg-reg/direct-memory `HB_IR_MOV` using paired 64-bit ARM loads and
stores against `ctx->regs.x64.xmm[n][2]`; direct memory remains gated by `MACRUNNER_HB_JIT_DIRECT_MEM`.
Coverage: `engine/hyperbridge/tests/hb_test_runner` => `348 passed, 0 failed`;
`tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so` relinked. Hollow Knight
`run-20260531-220527-phase3-xmm-move-jit/` timed out cleanly (`MR_RUN_RC=143`) with cleanup/prune `0`,
zero JIT fallback/fault/unsupported counters, and Mono paths reached. Hot packed move block
`0x87ef2ba3301` shrank `236 -> 148`. NEXT: rank1 memory-test/RMW loop fusion or smaller branch/control
code for `0x87ef2bf915f`; keep fallback-zero gate.

Status 2026-05-31 22:17: near conditional-PC emit compressed for shared E/NE Jcc pair paths. For close
targets, codegen now materializes fallthrough PC once and applies a small taken delta; far branches keep
the old two-PC materialization. Coverage: `engine/hyperbridge/tests/hb_test_runner` =>
`348 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so`
relinked. Hollow Knight `run-20260531-221130-phase3-near-branch-pc-jit/` timed out cleanly
(`MR_RUN_RC=143`) with cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and Mono paths
reached. Hot branch blocks shrank broadly: rank1 `200 -> 180`, rank2 `264 -> 244`, rank3 `388 -> 368`,
rank6 `196 -> 176`, rank7 `208 -> 188`, rank8 `316 -> 296`, rank9 `208 -> 188`, rank10 `244 -> 224`.
NEXT: inspect rank8/rank3 for remaining helper or fusion opportunities; continue preserving
fallback-zero gate.

Status 2026-05-31 22:23: zero-test Jcc block promoted for the hot rank8 family. Native JIT now fuses
`XOR r,r; TEST same-r,same-r; J(E/NE)` into one block with known branch result, while still recording
TEST lazy flags and preserving partial-register writes. Coverage: `engine/hyperbridge/tests/hb_test_runner`
=> `349 passed, 0 failed`; `tools/hb_oracle/fast_validate_family.sh phase1_core` PASS; spike `ntdll.so`
relinked. Hollow Knight `run-20260531-221923-phase3-zero-test-jcc-jit/` timed out cleanly
(`MR_RUN_RC=143`) with cleanup/prune `0`, zero JIT fallback/fault/unsupported counters, and Mono paths
reached. Hot rank8 `0x87ef2bf98fb` shrank `296 -> 160`. NEXT: inspect rank3 prologue/local-store block
`0x87ef2ba32d4` or rank1 memory-test/RMW loop fusion.

### ★ BULK ISA COVERAGE — MOVED TO LANE B (MacBook Air M1, separate machine) 2026-05-31
**This main-mac Codex (Lane A) no longer does bulk-ISA — it's on the Air now.** Lane A stays on
JIT perf / Hollow Knight window. To avoid a cross-machine merge collision, FILE-LEVEL split:
- **Lane A (this mac) MUST NOT edit** the decoder/lifter: `hb_decode_x64.c`, `hb_lift_x64.c`,
  `hb_decode_x86.c`, `hb_lift_x86.c` — those are Lane B's files on the Air.
- **Lane B (Air) owns** decode/lift + ISA matrix; it MUST NOT touch `hb_arm64_codegen.c`/`hb_jit*`
  (Lane A's files). Kit lives at `_air-bulk-isa-kit/` (see README-AIR-LANE-B.md).
- Lane B returns a patch via external disk; operator (Claude) reconciles it into main. If Lane A
  needs a decoder change for JIT work, flag it for the operator instead of editing the decoder.

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
