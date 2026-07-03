# TRIAGEFIX-HANDOFF — triage classifier false-positive fixes

Branch: `codex-hk-graphics-prep` (worktree `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-graphics-prep`)
Lane: triagefix · Date: 2026-07-04

## What was wrong

Two documented false positives, both reproducible on saved runs:

1. **IAT-bind marker counted as a real call (historical, HK rung 9).**
   `_marker_hit` in `classify_run.py` did a bare-substring match, so a
   load-time IAT-binding diagnostic line
   (`macrunner-hb-iatentry: ...!CreateDXGIFactory1`) matched the
   `dxgi-factory` ladder rung (rung 9) even though no real graphics call ran.
   Already fixed in **main**; the graphics-prep worktree was missing the fix.

2. **ABZU rc=53 (2026-07-04) → `WINED3D_FALLBACK` (confidence 0.70).**
   Real failure: `LdrLoadDll c0000135` for `dxgi.dll`/`d3d11.dll` +
   `err:virtual:map_fixed_area out of memory` + a cascade of `c0000005`
   across 6 wine helpers (control.exe, iexplore.exe, svchost.exe, plugplay.exe,
   winedevice.exe, …) all at the **same** faulting PC `0x87FFF89C61C` during
   `loader_init Initializing system dll`. The boot died at DLL load/init,
   before any graphics call. `analyze_d3d_gate` saw a stray
   `wined3d.dll` **import-prefix** line (`ntdll.dll imports __chkstk from
   wined3d.dll`) and read it as a WineD3D fallback → wrong class, wrong owner,
   wrong next-action ("Ensure Metal/DXMT backend selected").

## What changed

Two commits on this branch:

### Commit `93ec66f` — sync from main (BRANCH-ONLY, do NOT cherry-pick to main)
Main already has all of these; this just brings the worktree up to main so
the classifier is runnable here and FP #1 is fixed.
- `tools/triage/classify_run.py`: `_marker_hit` now rejects IAT-binding
  diagnostic lines (`iatentry`/`cpdecision`/`cpresult`/`iatfinal`). Fixes FP #1.
- `tools/triage/analyze_window_gate.py`: synced to current main.
- Restored missing analyzers + `triage_common.py` so the worktree classifier
  runs: `analyze_d3d_gate.py`, `analyze_waits.py`, `analyze_arm64ec_callbacks.py`,
  `analyze_smc.py`, `analyze_pe32_wow64_handoff.py`, `regression_tracker.py`.

### Commit `4c7a02c` — new classes (CHERRY-PICKABLE to main)
- **NEW** `tools/triage/analyze_dll_load.py` — two blocker classes that fire
  before the graphics pipeline starts (registered first in `ANALYZERS`):
  - `DLL_LOAD_FAILURE` (priority 96): a graphics DLL (`dxgi`/`d3d11`) fails to
    load with `c0000135` AND `map_fixed_area out of memory` is present → guest
    address-space/layout exhaustion, **not** a backend-selection issue.
  - `SYSTEM_DLL_INIT_CASCADE` (priority 94): ≥3 distinct wine helpers take
    `c0000005` at the **same** faulting PC inside
    `loader_init Initializing system dll` → one system DLL in the dist is
    corrupted/ABI-incompatible (binary-graft class).
- `tools/triage/classify_run.py`:
  - `analyze_dll_load.py` added first in `ANALYZERS`.
  - Priority scores: `DLL_LOAD_FAILURE` → 96, `SYSTEM_DLL_INIT_CASCADE` → 94
    (above graphics 70 and window ≤95, because they precede both in the boot
    ladder).
  - **Suppression**: when `DLL_LOAD_FAILURE` or `SYSTEM_DLL_INIT_CASCADE` is
    `BLOCKED`, demote downstream false positives to `*_SUPERSEDED_BY_DLL_LOAD`:
    - the d3d-blocker set (`WINED3D_FALLBACK`, `D3D11_DLL_NOT_LOADED`,
      `DXGI_DLL_NOT_LOADED`, `CREATE_DXGI_FACTORY_MISSING`,
      `D3D11_CREATE_DEVICE_MISSING`, `PRESENT_MISSING`) — **always**, because
      graphics never ran if the boot died at DLL load/init;
    - `WINDOW_SERVER_ERROR` / `GENERIC_ACCESS_VIOLATION` /
      `GENERIC_WOW64_FAULT` / `WINDOW_NCCREATE_ABORTED` — **only when their
      evidence is itself load/init noise** (`c0000135`/`c0000005` from
      `LdrLoadDll`/`loader_init`/`dispatch_exception`), so a genuine
      window/AV class with real evidence is preserved.

## How to cherry-pick to main

```
# In main MacRunner worktree:
git cherry-pick 4c7a02c     # the new classes (analyze_dll_load.py + classify_run.py hunks)
```
`4c7a02c` is additive on top of main's `classify_run.py` (the diff vs main is
purely the new-class hunks — verified). Do **not** cherry-pick `93ec66f`;
main already has the IAT fix + window-gate + all analyzers.

## Regression matrix (my classifier vs current main baseline)

Method: each run dir copied to /tmp (originals kept pristine); both the
current main classifier and the triagefix classifier run on identical
copies. "==" = same primary class (no regression).

### Saved-run regressions
| Run (under main `reports/`, read-only) | main baseline | triagefix | result |
|---|---|---|---|
| ABZU rc=53 (`MacRunner-abzu/.../abzu-dxmtpoll-singlelookup-900`) | `WINED3D_FALLBACK` (FP) | `DLL_LOAD_FAILURE` | **FIXED** |
| `phase4-hollow-knight/laneA-reconcile-head-gated-default-try1-122126` | `PRESENT_MISSING` | `PRESENT_MISSING` | == |
| `phase5-hollow-knight/run-20260607-105041-hk-dxmt-arm64` (real wined3d fallback) | `WINED3D_FALLBACK` | `WINED3D_FALLBACK` | == (real fallback preserved) |
| `hk/run-seh-234225` | `CREATE_DXGI_FACTORY_MISSING` | `CREATE_DXGI_FACTORY_MISSING` | == |
| `hk/run-205148-unwind-fix` | `GENERIC_ACCESS_VIOLATION` | `GENERIC_ACCESS_VIOLATION` | == |
| `hk/run-msvcrt-trace-013037` | `PE32_WOW64CPU_NOT_LOADED` | `PE32_WOW64CPU_NOT_LOADED` | == |
| `hk/run-clean-001153` | `GENERIC_WOW64_FAULT` | `GENERIC_WOW64_FAULT` | == |
| `phase4-hollow-knight/laneA-fpzerofix-try1-222618` | `CREATE_DXGI_FACTORY_MISSING` | `CREATE_DXGI_FACTORY_MISSING` | == |
| `floor-reach/latest` | `CREATE_DXGI_FACTORY_MISSING` | `CREATE_DXGI_FACTORY_MISSING` | == |
| `phase4-hollow-knight/laneA-hk-glm-latest-postswap-132055-try1-132055` | `PRESENT_MISSING` | `PRESENT_MISSING` | == |

### Fixture regressions (main `tools/triage/fixtures/`, 16 dirs)
All 16 match baseline: `gfxdevice_c0000026_seh_detail_run`,
`gfxdevice_seh_before_d3d11_run`, `hk_window_init_exit_run`,
`pe32_i386_executing_run`, `pe32_wow64cpu_not_loaded_run`,
`translator_stack_overflow_run`, `window_atom_mismatch_run`,
`window_createstruct_bad_run`, `window_handle_create_failed_run`,
`window_instance_mismatch_run`, `window_nccreate_aborted_run`,
`window_nccreate_return_false_run`, `window_no_desktop_run`,
`window_server_error_run`, `window_thread_desktop_zero_run`,
`window_unity_success_no_fp_run`.

Key non-regression checks:
- `window_server_error_run` stays `WINDOW_SERVER_ERROR` — suppression is inert
  (no DLL_LOAD_FAILURE/SYSTEM_DLL_INIT_CASCADE blocker present).
- `window_unity_success_no_fp_run` stays `WINDOW_GATE_SUCCESSFUL` (PASS) — the
  "no false positive" fixture is not broken.
- `run-20260607-105041-hk-dxmt-arm64` (a **real** wined3d fallback where
  graphics actually ran) stays `WINED3D_FALLBACK` — suppression does not fire
  because no earlier DLL-load/init blocker is present.

## IAT fix #1 verification

`_marker_hit` unit-checked directly:
- iatentry line `macrunner-hb-iatentry: import=dxgi.dll!CreateDXGIFactory1`
  → not counted (was True before the fix → false rung 9).
- real boundary line `macrunner-hb-d3d-boundary: ...CreateDXGIFactory`
  → counted (True).
- counter line `D3D11CreateDevice=0` → rejected (original behavior preserved).
- `ladder_rung` on an IAT-only log → `(-1, None)` (no false dxgi-factory rung).

## Known follow-ups (out of scope for this lane)

- `analyze_d3d_gate.py` (main) sets `wined3d_loaded = True` on **any** line
  containing `wined3d`, including import-prefix lines like
  `macrunner-hb-arm64x-native-import-prefix: module=L"wined3d.dll"`. This is
  the root of FP #2's `WINED3D_FALLBACK`. The suppression in
  `classify_run.py` neutralises it when a DLL-load blocker is present, but a
  tighter fix in `analyze_d3d_gate.py` (require a real `module_map`/load
  event, not an import-prefix line) would remove the loose trigger entirely.
  File is in main, not this worktree's editable set — left for the CPU/lane
  owner.
- `analyze_window_gate.py` (main) can latch onto a `load_dll c0000135` line
  and report `WINDOW_SERVER_ERROR`; the suppression handles it when a DLL
  blocker is present. A tighter evidence filter in the window gate is a
  separate follow-up.
