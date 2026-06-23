# Lane D Needs

Updated: 2026-06-06

This file records cross-lane needs only after Lane D has exhausted in-scope graphics workarounds. Lane D continues with DXMT coverage using the isolated smoke prefix and strict x64 guest binding path where available.

## Native `winemetal=n` Unix-func registration

Status: resolved for the x64 guest binding path on 2026-06-03; no active Lane A/C need for this path.

Evidence:

```text
engine/graphics/scripts/run_dxmt_x64_binding_smoke.sh
WINEDLLOVERRIDES=d3d11,dxgi,d3d10core,winemetal=n
artifacts/dxmt-x64-binding/run-20260605-030159/dxmt-x64-binding.log
dxmt_sync=PASS
load_winemetal=PASS
load_dxgi=PASS
load_d3d11=PASS
unixlib_binding=PASS
present_reached=NO
```

Lane D attempts completed:

- Installed built `d3d11.dll`, `dxgi.dll`, `winemetal.dll`, and `winemetal.so` into `artifacts/dxmt-smoke-prefix/drive_c/windows/system32`.
- Added prefix-local app-dir copies and `WINEDLLPATH` / `WINESYSTEMDLLPATH` bindings.
- Added a prefix-local builtin overlay with `MACRUNNER_DXMT_ROOT`.
- Built raw native DXMT DLLs and separately postprocessed `winemetal.dll` as builtin.
- Landed mixed binding: raw native `d3d11.dll`/`dxgi.dll` plus builtin `winemetal.dll`, all from the Lane D prefix overlay.
- Re-probed strict native on 2026-06-03:
  `artifacts/dxmt-smoke-logs/lane-d-strict-native-20260603-215627.outer.log`
  still reports `winemetal_init_unix_call status=0xc0000135`.
- Manual `winebuild --builtin winemetal.dll` postprocess removes the Unix-call
  init status but then strict native load fails as `LoadLibraryExW(winemetal)
  module=0 gle=126`, confirming the postprocessed PE is only viable via
  builtin/mixed binding, not `winemetal=n`.
- Patched the x64 binding smoke to skip automatic prefix-update wineboot for the
  focused binding probe, default to strict `winemetal=n`, and fail on any return
  of the old Unixlib status/load signatures.

Resolution:

The x64 guest path now binds the raw x64 `winemetal.dll`, `DXGI.DLL`, and
`d3d11.dll` from `engine/graphics/dist/dxmt/x86_64-windows` under strict native
overrides. The smoke parser explicitly rejects `winemetal_init_unix_call`,
`__wine_init_unix_call`, `c0000135`, and `LoadLibraryExW(winemetal)` regressions.

## Real Hollow Knight DXMT→Metal call blocked by arm64ec-syscall-data-repair

Status: **ACTIVE BLOCKER** — Lane A fix needed in `signal_arm64.c:208`. Lane D continues other graphics backlog.

SEH blocker was fixed 2026-06-07 (Lane A). Post-SEH run 2026-06-07 shows NEW blocker.

Evidence from `reports/phase5-hollow-knight/run-20260607-104129-hk-dxmt-5min/`:

```text
# DXMT DLLs load correctly as x86_64 native:
0024:trace:loaddll:build_module Loaded "C:\windows\system32\winemetal.dll": native
0024:trace:loaddll:build_module Loaded "C:\windows\system32\DXGI.DLL": native
0024:trace:loaddll:build_module Loaded "C:\windows\system32\d3d11.dll": native

# Unity Mono initializes, reaches graphics creation:
Initialize engine version: 6000.0.61f1 (74a0adb02c31)
GfxDevice: creating device client; kGfxThreadingModeThreaded

# ARM64EC tagged-PC callback starts (DXMT x64 calling Metal ARM64 via unixlib):
macrunner-hb-arm64ec-exception-context: code=80000002 flags=00000000 pc=000000010AEEF015 lr=000000010AEEF015 x64main=0
macrunner-hb-arm64ec-tagged-pc: exception=80000002 pc=000000010AEEF015 fixed=000000010AEEF014
macrunner-hb-arm64ec-exception-context: code=80000002 flags=00000000 pc=000000010AEEF014 lr=000000010AEEF014 x64main=1

# Repair triggers — skips entire DXMT→Metal call:
macrunner-hb-arm64ec-syscall-data-repair: tid=0024 pc=000000010AEEF014 lr=000000010AEEF014
  frame=000000010AEEED70 bad_frame_pc=0000087FFF989F38 bad_frame_lr=0000087FFF989F38
  prev=000000010AEEFCD0 prev_pc=0000087FFF94C630 prev_lr=0000087FFF94C630
  prev_sp=000000010BEEFB80 resume=continue-prev-frame

# After repair: game loop continues but GfxDevice creation silently fails, no more Unity output
```

Root cause analysis (signal_arm64.c:208 `macrunner_hb_fix_syscall_data_boundary_exception`):

1. DXMT x86_64 PE calls Metal ARM64 via ARM64EC tagged-PC mechanism (STATUS_DATATYPE_MISALIGNMENT = 0x80000002)
2. `macrunner_hb_fix_tagged_arm64ec_misalignment` removes tag bit: pc=0x10AEEF015 → fixed=0x10AEEF014
3. Exception re-processed. Fixed PC (0x10AEEF014) happens to fall inside syscall frame [0x10AEEED70, +0x330]
4. `macrunner_hb_fix_syscall_data_boundary_exception` triggers (STATUS_DATATYPE_MISALIGNMENT AND pc-in-frame)
5. Restores prev frame context, skips the DXMT→Metal call entirely (`resume=continue-prev-frame`)
6. DXMT's CreateDXGIFactory returns null/failure. Unity sees no graphics device.

Bug: the syscall-data-repair should NOT trigger when the PC-in-frame match is the artifact of a just-fixed tagged ARM64EC call. The fix in HyperBridge should distinguish legitimate ARM64EC entry calls from broken syscall frames.

Also confirmed from x64 binding smoke (2026-06-07):
```
artifacts/dxmt-x64-binding/run-20260607-103907/dxmt-x64-binding.log
run_rc=0  (SEH gone — no more timeout!)
dxmt_sync=PASS, load_winemetal=PASS, load_dxgi=PASS, load_d3d11=PASS
unixlib_binding=PASS (no c0000135 errors)
present_reached=NO  (Metal call skipped by repair)
```

Lane D attempts to work around this:
- Tried `SYSTEM32_ARCH=aarch64-windows` DXMT DLLs: aarch64 d3d11/dxgi loaded as builtin but Wine used its own wined3d builtin (not DXMT), game hit AV in aarch64 unrelated code. Not viable without additional loader changes.
- x64 DXMT DLLs are the correct path; the repair bug in signal_arm64.c must be fixed.

Need (Lane A):

Fix `macrunner_hb_fix_syscall_data_boundary_exception` in `engine/wine/dlls/ntdll/signal_arm64.c:208`:
Do NOT apply syscall-data-repair when the triggering exception is from a tagged-ARM64EC call that was just handled by `macrunner_hb_fix_tagged_arm64ec_misalignment`. A flag or re-entrancy guard can distinguish the two cases.

## VKD3D native D3D12 load bypassed by builtin dependency routing

Status: resolved for the Lane D strict prefix runtime probe on 2026-06-04; no active Lane A/C need for this path.

Evidence:

```text
reports/phase5-vkd3d/run-20260603-200745-arm64-winedllpath/d3d12-create-device-arm64-winedllpath.log
engine/graphics/dist/vkd3d/aarch64-windows/d3d12.dll copied into system32
macrunner_hb_open_native_builtin_dependency MacRunner HyperBridge builtin dependency "d3d12.dll"
  => engine/wine/dist-arm64ec-spike/lib/wine/aarch64-windows/d3d12.dll
artifacts/vkd3d-prefix-sync/run-20260604-165527/runtime-loader-aarch64-windows.log
WINEDLLOVERRIDES=d3d12,d3d12core=n
vkd3d_runtime_load_result=PASS
vkd3d_runtime_versioned_rootsig_result=PASS
vkd3d_runtime_device_result=SKIP reason=opt_in_disabled
artifacts/vkd3d-prefix-sync/run-20260604-205538/runtime-loader-aarch64-windows.log
VKD3D_RUNTIME_PROBE_DEVICE=1 diagnostic: D3D12CreateDevice hr=0x80004005
```

Lane D attempts completed:

- Built VKD3D after fixing the local glslang dylib IDs.
- Deployed `d3d12.dll` and `d3d12core.dll` into isolated aarch64/x64 graphics prefixes.
- Verified prefix byte matches with `engine/graphics/scripts/run_vkd3d_prefix_sync_smoke.sh`.
- Tried app-local/system32/WINEDLLPATH native-load variants.
- Added a strict aarch64 runtime loader gate that copies app-local native
  `d3d12.dll`/`d3d12core.dll`, overrides both DLLs as native, verifies native
  `d3d12core.dll` binding, and exercises legacy plus versioned root signature
  serialization/deserialization.
- Added an opt-in `VKD3D_RUNTIME_PROBE_DEVICE=1` diagnostic for
  `D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0)`. The current diagnostic
  fails with `hr=0x80004005` after loading builtin `winevulkan.dll`, so full
  D3D12 device bring-up is confirmed as backend work rather than the old native
  DLL binding problem.

Resolution:

The graphics-owned prefix/app-local path now binds native vkd3d DLLs cleanly when
`d3d12,d3d12core=n` is set. Full D3D12 device creation remains separate from this
loader gate because it depends on the host graphics/device backend (`winevulkan`
today, future Metal path), not on the previous builtin dependency misrouting
signature.

## 2026-06-06 — Lane D continuation

Status: continue.

Evidence:

```text
Lane D continues under owned graphics scope.
DXMT and vkd3d local binders are stable in owned prefixes.
Unity/D3D11 marker path is still blocked before `CreateDXGIFactory`
by external HyperBridge/SEH callback state.
```

Current in-scope actions:

- Maintain and extend owned DXMT smoke coverage only.
- Keep `DXMT-D3D11-COVERAGE.md` as living matrix, add entries only if local gaps
  appear.
- Add new backlog-ready notes for `DXMT-D3D8/D3D9-PATH-NOTE.md` only (no native D3D8/9 runtime edits here).
- Continue logging each iteration in `lane-loop-logs/D-driver.out` and escalate only
  when a new external blocker appears.

Lane D in-loop cadence (operational):

- Next: continue through owned smoke-suite refreshes (headless, live-window, fullscreen)
  and Phase 7 readiness notes only.
- Keep blocker triage explicit: if new real-x64 Unity/DXGI crash signatures appear,
  record exact marker and timestamps in `LANE-D-NEEDS.md` before changing scope.

## 2026-06-06 — Hot continuation pass A

Status: in-scope-only.

Progress:

- Owned D3D11/DXGI matrix remains green in this lane scope.
- No new local graphics-code gaps opened by this pass.
- Next actions remain: keep smoke-suite in known-good state and record only externally sourced blockers.

## 2026-06-06 14:18:22 — Hot continuation pass B

Status: continue (owned scope only).

- Smoke family remains green in owned DXMT harnesses (headless/live/fullscreen).
- No local graphics implementation gap opened in this pass.
- External blocker remains `HyperBridge/SEH` before `CreateDXGIFactory`/`D3D11CreateDevice` in real x64 Unity reach checks.

## 2026-06-06 14:25:08 — Hot continuation pass C

Status: continue (owned scope only).

- Ownership checks remain green across owned DXMT smoke-suite and coverage tracking.
- No local Gap-band opens in the in-scope scripts/tests; only external blocker remains.
- Next step remains to keep headless/live/fullscreen refresh cadence and log markers only.

## 2026-06-06 14:29:43 — Hot continuation pass D

Status: continue (owned scope only).

- Owned verification commands rerun with bounded scope: D3D9 translation test file, D3D9 Metal request-only smoke, D3D9 DXMT headless smoke, and D3D11 headless (aarch64).
- Result: owned checks are green after one local test fix in
  `engine/graphics/tests/test_d3d9_translation.py`.
- `run_dxmt_d3d11_headless_smoke.sh x86_64` fails in current local environment because `engine/wine/dist` does not provide:
  - `lib/wine/x86_64-unix/winemac.so`
  - `lib/wine/x86_64-unix/ntdll.so`
- Real Unity/D3D11 path in x64 remains blocked by external `HyperBridge/SEH`
  `EXCEPTION_INVALID_DISPOSITION` before `CreateDXGIFactory`/`D3D11CreateDevice` markers.

## 2026-06-12 21:34 — Real HK x64 DXMT reach check after rpcss fix

Status: Lane D RPC/DXMT gate verified; current blocker is external pre-DXGI.

- `reports/phase4-hollow-knight/laneA-laneD-rpcss-services-try1-212030/`:
  services/rpcss start under `mr-run`; `RPC_S_SERVER_UNAVAILABLE=0`;
  `CreateDXGIFactory=0`, `D3D11CreateDevice=0`, `UnityWndClass=0`,
  `GfxDevice=0`.
- `reports/phase4-hollow-knight/laneD-rpcss-sample2-212855/`:
  valid wine-process sample shows execution under
  `__wine_unix_call_dispatcher -> macrunner_hb_x64_thread_entry ->
  macrunner_hb_run_x64 -> macrunner_hb_call_import_thunk ->
  macrunner_hb_try_kernel32_handle_semantic -> NtWaitForSingleObject/server_wait`.
- `reports/phase4-hollow-knight/laneD-wait-trace-213215/`:
  `MACRUNNER_HB_TRACE_WAIT_SEMANTIC=1` classifies as `WAIT_DEADLOCK`
  at rung `mono-init`; no RPC/epmapper fault, no DXGI/D3D11/window markers.

Conclusion: Lane D graphics remains ready; real HK is blocked before the
graphics stack by HyperBridge/kernel32 handle-wait semantics. Do not iterate
DXMT for this symptom until HK reaches `CreateDXGIFactory` or WineMetal HWND
markers again.
## 2026-06-12 22:55 — Pending validation: original PE exec ranges for x64 builtins

Status: implemented in source + rebuilt/signed `dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`, but final HK validation is blocked by active `x18waittrace2` owning HK/mr-run.

Evidence run dirs:
- `reports/phase4-hollow-knight/laneA-laneD-lift-probe-strict-try1-221900` — hot loop in DXGI `.rdata` RVAs.
- `reports/phase4-hollow-knight/laneA-laneD-section-probe-try1-222622` — runtime `DXGI.DLL .text sec_size=0xbaef6`, chars `0x60000020`.
- `reports/phase4-hollow-knight/laneA-laneD-wide-transition2-try1-223746` — first logged run PC already in DXGI `.rdata`, CFG fast path likely involved.

Next when HK runner clears:
1. Run `MACRUNNER_HB_TRACE_EXEC_GUARD=1 MACRUNNER_GRAPHICS_BACKEND=dxmt MACRUNNER_MR_RUN_START_SERVICES=1 MACRUNNER_FLIGHT_RECORDER=1 WINEDEBUG=-all scripts/laneA-run-hk.sh laneD-original-exec-validate 70 1`.
2. Expected: DXGI `.rdata` should no longer report `strict=1`; either `nonexec-section` fires at the bad target or execution progresses to real DXGI markers.
3. If `nonexec-section` fires, use logged `from_pc`/CFG data to fix the producer target, not a band-aid.

## 2026-06-13 01:38 — Lane A handoff boundary: original exec ranges patch only

Status: NOT landed by Lane D. `macrunner_hb.c`/`loader.c`/`ntdll.so` dist are Lane A owned while x18 Part 2 is active.

Patch artifact for Lane A:
- `reports/research/patches/LANE-D-dxgi-original-exec-ranges-for-lane-a.patch`

Important: this is a raw candidate patch captured from the current dirty ntdll diff. It contains the original executable-section side table plus diagnostic probe hunks. Lane A should review/cherry-pick only the side-table fix after x18 Part 2 lands, not apply the patch blindly.

Lane D will not:
- deploy to shared Wine dist,
- edit `engine/wine/dlls/ntdll/**`,
- rerun `laneD-original-exec-validate`,

until Lane A lands x18 Part 2 and explicitly coordinates validation on Lane A state.

## 2026-06-13 01:58 — External: arm64 game-torture engine missing

Status: D3D9 x64/x86 game-torture now runs as real mock-replay workloads and passes. arm64 cannot run because the configured native arm64 engine path is absent:

- missing: `engine/wine/dist-pure-arm64/bin/wine`
- present but not substituted by Lane D: `engine/wine/dist/bin/wine`

Lane D did not create, copy, or retarget Wine dist artifacts. Owner decision needed: restore/provide `dist-pure-arm64` for arm64-native fixtures, or explicitly retarget arm64 game-torture to an approved existing dist.

## 2026-06-13 06:55 — External: Rosetta Wine SIGKILL blocks PE app validation

Status: current Rosetta Wine lanes fail before any app/D3D code runs.

Evidence:
- `hello_x64.exe` and `hello_x86.exe` through `scripts/run-windows-app.sh` return `rc=247`, empty stdout/stderr.
- `d3d9_triangle_x64/x86` and `d3d11_create_device_x64/x86` show the same `rc=247`, empty stdout/stderr, no D3D trace.
- `/usr/bin/arch -x86_64 engine/wine-x86_64/bin/wine --version` returns `wine-11.0`.
- `/usr/bin/arch -x86_64 engine/wine-x86_64/lib/wine/x86_64-unix/wine --version` returns `137`/SIGKILL.

Lane D classified this in game-torture as `ROSETTA_WINE_SIGKILL` infrastructure blocker so D3D reports do not mislabel it as `D3D_SHIM_NOT_LOADED`.

Owner decision needed: repair/replace the Rosetta Wine unix loader or route the affected lanes through an approved working launcher. Lane D did not edit Wine dist, loader, or ntdll files.
2026-06-13 18:46 · D3D9 blocker уточнение: DXVK backend **есть** в main (engine/graphics/dist/dxvk/aarch64-windows/d3d9.dll, engine/dxvk source exists); уже скопирован в frozen engine/wine/dist/lib/wine/aarch64-windows. Однако on-screen harness `run_d3d9_onscreen_present_harness.sh` отсутствует в текущем HEAD, поэтому on-screen re-run временно не выполняется (нужен restore данного раннера для PASS/FAIL).
