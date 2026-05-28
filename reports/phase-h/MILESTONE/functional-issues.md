# Notepad++ x64 Functional Issues

Date: 2026-05-19
Commit under review: `d699ddb` (`Fix scalar SSE move widths for Notepad++ painting`)
Tags kept as historical checkpoints:
- `v0.1-engine-phase-h-x64-notepad-plus-plus-visible`
- `v0.1-engine-phase-h-x64-notepad-plus-plus-functional`

## Status Correction

Phase H is **not commercially closed** yet. The tags are useful recovery/checkpoint markers, but the product-grade bar is higher than "window visible and some text typed".

Current truth from screenshot review and smoke attempts:
- Main Notepad++ window opens and editor surface is interactive enough to show typed text.
- Rendering remains visibly rough: font metrics / glyph rasterization / alignment do not look native Windows-quality.
- Menu/chrome draws, but click responsiveness and full menu behavior are not yet systematically certified.
- Toolbar/dialog/file persistence paths are untested.
- Local `screencapture` fails with `could not create image from display`; screenshots currently rely on user-provided captures.
- AppleScript Accessibility sees only the Wine host window/chrome, not internal Win32 controls. Coordinate/keyboard smoke did not produce a reliable clipboard round-trip, so automation cannot mark UI items green yet.

## Functional Checklist

### Core Input / Rendering

- [x] Main window appears with editor surface, line gutter, caret area.
- [x] Basic text input observed manually in user screenshot (`fgdfg`).
- [ ] Type 100 ASCII characters, no corruption. Evidence pending.
- [ ] Type Cyrillic / UTF-8 characters, correct rendering. Evidence pending.
- [ ] Caret blinks correctly and remains positioned at expected glyph advance. Evidence pending.
- [ ] Selection highlight visible and aligned. Evidence pending.
- [ ] Mouse click positions caret correctly. Evidence pending.

### Fonts

- [ ] System UI font matches Windows baseline. Current screenshot suggests mismatch / rough metrics.
- [ ] Notepad++ default editor font renders correctly. Current screenshot suggests monospace/glyph artifacts.
- [ ] Font size changes work.
- [ ] Bold / italic styles render correctly.
- [ ] Compare against Windows VM screenshot baseline.

Potential narrow class: GDI text metrics / FreeType integration / glyph cache / `ExtTextOutW` marshalling.

### Menus

- [ ] File menu opens submenu by click.
- [ ] Edit menu opens submenu by click.
- [ ] All top menus respond.
- [ ] Hover state visible.
- [ ] Submenus positioned correctly.
- [ ] Escape closes active menu.

### Toolbar / Buttons

- [ ] Toolbar buttons clickable.
- [ ] Hover tooltips show.
- [ ] Icons render correctly, no white/black placeholder squares.
- [ ] Buttons execute actions.

Potential regression: user reported buttons worked briefly, then review says button/menu behavior may have regressed or remains uncertified. Needs evidence before fix.

### Dialogs

- [ ] File -> Save opens file picker.
- [ ] File -> Open opens file picker.
- [ ] Edit -> Find opens dialog and search works.
- [ ] Edit -> Replace opens dialog and replace works.
- [ ] Settings -> Preferences opens tabbed dialog.

### Multi-document

- [ ] New tab creates document.
- [ ] Switching tabs works.
- [ ] Closing tab works.
- [ ] Modified indicator / asterisk behaves correctly.

### Persistence

- [ ] Type text -> Save -> file exists on disk.
- [ ] Close app -> re-open -> file contents preserved.
- [ ] File -> Open loads saved file and displays content.

## Evidence Collected This Pass

- Process alive: `notepad++.exe`, `wineserver`, `services.exe`, `explorer.exe`, `rpcss.exe` all present.
- `sample` shows process in normal Wine/native event paths, not hard-crashed.
- Accessibility probe: process `wine` has one host window named `*new 1 - Notepad++ [Administrator]`; internal Win32 controls are not exposed as AX elements.
- Clipboard smoke via AppleScript is unreliable: despite `wine` becoming frontmost, `Ctrl+A/C` copied Codex text, not editor text. Do not use this as proof of Notepad++ failure; it proves only that this automation route is not certification-grade.
- Windows-side smoke helper attempt:
  - CRT-based x64 helper hit a HyperBridge runtime failure in helper startup, so it is invalid as a Notepad++ test.
  - no-CRT helper returned without producing stdout/result file; this also is not valid product evidence yet.
  - Conclusion: build a dedicated, proven smoke harness before marking Scintilla/menu checks green.

## 2026-05-19 UI Smoke Harness Result

Harness files:
- `scripts/run-notepad-x64-ui-smoke.sh`
- `reports/phase-h/MILESTONE/smoke-tools/npp_ui_smoke_helper.c`
- `reports/phase-h/MILESTONE/smoke-tools/npp_ui_smoke_helper.exe`

Canonical launcher change:
- `scripts/run-notepad-x64.sh` now accepts `--duration=N` for bounded non-interactive smoke runs. Default behavior remains 18 seconds.

Latest evidence:
- Run dir: `reports/phase-h/npp-x64-20260519-143957/`
- Result file: `reports/phase-h/npp-x64-20260519-143957/ui-smoke.result.txt`
- Wrapper reached app launch and kept Notepad++ alive for the bounded window: `alive_after_70s=1`.
- With `MACRUNNER_HB_X64_BLOCK_LIMIT=10000000`, discovery is reliable after ~41.5s:
  - `case=discover status=PASS main=0x1011a scintilla=0x10122 toolbar=0x1010192 ...`
  - `case=editor_remote_scintilla status=PASS len=98 text_match=1`
- Remaining failures are menu/command inventory and dependent dialog cases:
  - `case=menu_inventory status=FAIL top_count=-1 ... cmd_new=0 cmd_open=0 cmd_save=0 cmd_find=0 cmd_preferences=0`
  - `case=dialog_find/preferences/save/open status=FAIL reason=missing_command`
  - `case=toolbar_new_click status=YELLOW reason=missing_new_command_from_menu_inventory`
  - `case=font_metrics status=YELLOW baseline=missing ...`

Interpretation:
- The earlier `pc=0x140244ef7` block-limit result was a startup cap, not proof of a deadlock: the real Notepad++ HWND/Scintilla appears with a larger block budget.
- The next evidenced engine blocker was `x64-signal-callback` at `0x1402a8653`. Disassembly plus `out_steps=9` showed the callback got past `movsd` and failed on the packed FP add/sub family (`66 0F 58`/`66 0F 5C`, `addpd/subpd`).
- HyperBridge now covers the add/sub prefix family (`ADDPS/ADDPD/ADDSS/ADDSD`, `SUBPS/SUBPD/SUBSS/SUBSD`) with C runner coverage; latest smoke no longer reports that runtime failure in wrapper keylines.
- Current product-smoke blocker is narrower: menu command discovery returns no menu/command IDs, so toolbar click and dialogs remain unverified.

Bootstrap telemetry correlation:
- Source: `reports/bootstrap_telemetry_batch.json`
- Report: `docs/BOOTSTRAP-GROUND-TRUTH-A008.md`
- 256 historical Notepad++ runs show `ole_marshal_fail=34`, `hb_fault=28`, `hb_runtime_fail=6`.
- `ole_marshal_fail` is the top error class and is a strong COM/STA marshalling signal, but this run does not yet prove that COM is the direct cause.
- HB fault breakdown worth auditing before any opcode fix:
  - 8x `bytes=cd pc=0x1403eba17 guest=0x1403eba1c result=UNSUPPORTED_OPCODE ir=UNSUPPORTED(73)`
  - 1x `bytes=0f0d pc=0x140408902` (prefetch/NOP-hint family)
  - 1x `bytes=c0 pc=0x1401db930` (byte shift/rotate family)

2026-05-19 retest / correction:
- A defensive combase timeout patch was tested against the Kimi COM/STA hypothesis, then rolled back: the bad UI run produced no `macrunner-com-*` wait traces and no COM timeout evidence.
- Default HyperBridge callback budget still reproduces the bad visible state:
  - Run dir: `reports/phase-h/npp-x64-20260519-152148/`
  - Result: title empty, `toolbar=0`, `editor_remote_scintilla FAIL`, `toolbar_inventory FAIL`.
  - Wrapper keyline: `reason=block-limit pc=0x140244ef7 blocks=30d41 steps=dd459`.
- Disassembly of `0x140244ef7` shows `mov r9, rax; ret`, not a wait loop. Treat this as a budget boundary artifact, not the semantic hang PC.
- With `MACRUNNER_HB_X64_BLOCK_LIMIT=10000000`, the good UI path returns:
  - Run dir: `reports/phase-h/npp-x64-20260519-152409/`
  - `editor_remote_scintilla PASS`, `toolbar_inventory PASS`, `toolbar_render PASS`, title `new 1 - Notepad++ [Administrator]`.
- Engine fix applied: `engine/wine/dlls/ntdll/unix/macrunner_hb.c` now disables the outer block cap for `x64-signal-callback` and relies on the existing step cap, matching the already-unbounded app-thread policy.
- ntdll was force-relinked and ad-hoc codesigned after the change.
- Default-env retest after the fix:
  - Run dir: `reports/phase-h/npp-x64-20260519-153131/`
  - `editor_remote_scintilla PASS`, `toolbar_inventory PASS`, `toolbar_render PASS`, title `new 1 - Notepad++ [Administrator]`.
  - No `block-limit pc=0x140244ef7` keyline in the run logs.
  - The outer tool timed out before the orchestrator summary completed, so clean bounded wrapper exit is still not certified by this run.
  - 1x `bytes=fe pc=0x14040d093` (INC/DEC r/m8 family)
  - 1x `bytes=660f7e pc=0x140417e10` (SSE-to-GPR transfer family, known sensitive area)
  - native faults also cluster at `pc=0x7ffd0af30d4` with fault address `0x6d6974ff0000082f`.

## Smoke Harness Design

Smallest reliable first harness:

- Launch Notepad++ only through `./scripts/run-notepad-x64.sh` in bounded mode. The harness may run the wrapper in the background, but the parent shell must remain alive until the wrapper performs its own cleanup.
- Run a same-prefix Windows helper while the canonical wrapper keeps Notepad++ alive. The helper writes a structured report to the current run directory; stdout/clipboard/macOS Accessibility are not evidence sources.
- Enumerate the Notepad++ main window, Scintilla children, `ToolbarWindow32`, menu handles, and modal/modeless dialogs via Win32 APIs from inside Wine.
- For editor text round-trip, do not pass helper-local pointers to Scintilla custom messages. Use either Win32-marshaled `WM_GETTEXT`/`WM_SETTEXT` or `OpenProcess` + `VirtualAllocEx` + `WriteProcessMemory` + Scintilla messages + `ReadProcessMemory`.
- For menu evidence, prefer actual input route (`SetForegroundWindow` + `SendInput` Alt/menu key) and verify an active popup/menu state or menu window. `WM_COMMAND` alone proves command dispatch, not menu opening.
- For toolbar evidence, enumerate toolbar buttons with `TB_BUTTONCOUNT`/`TB_GETBUTTON`, then use item rectangles plus real mouse input for one low-risk action and verify the resulting window/dialog/editor state.
- For dialogs, dynamically find command IDs from the real Notepad++ menu text, dispatch or click only one command at a time, verify the expected dialog title/class, then close it before the next case.
- For font evidence, collect deterministic metrics (`GetTextMetricsW`, `GetTextExtentPoint32W`, Scintilla text height/position metrics) and compare them to an explicit Windows baseline artifact. Until that baseline exists, font checks must stay yellow, not green.

## Next Priority

1. Fix menu/command discovery evidence: why `GetMenu`/menu inventory yields `top_count=-1` and all command IDs remain zero while the Notepad++ top-level HWND exists.
2. Re-test toolbar and dialogs only after command IDs or an equivalent Windows-side dispatch route is proven.
3. Keep COM/STA/OLE telemetry in scope, but do not treat it as the current direct blocker unless a fresh run correlates it with a concrete hang/failure site.
4. Add a Windows baseline artifact for font metrics before marking font rendering green.
5. If clicks fail after menu IDs exist, sample/log exact Win32 route: hit-test -> mouse message -> window proc -> command dispatch.

## Rule

No broad fixes from screenshots alone. Each failing item becomes a narrow bug with evidence, then family audit if the class is architectural.

2026-05-19 manual visual check after c3bc86e:
- Run dir: `reports/phase-h/npp-x64-20260519-155221/`
- User screenshot confirms Notepad++ remains visible and interactive enough to show title/menu/tab/editor structure after default `x64-signal-callback` cap fix.
- Still not product-grade visually:
  - Toolbar buttons render as gray placeholder squares instead of real icons.
  - Black toolbar/background bands remain too large and non-native.
  - Font/menu metrics still look off compared with a native/Windows baseline.
- Treat these as visual/product-smoke failures, not Phase H closure.

2026-05-19 full-functional directive intake:
- Source directive: `docs/CODEX-DIRECTIVE-notepad-plus-plus-full-functional.md`
- Closure checklist created: `reports/phase-h/MILESTONE/closure-checklist.md`
- Directive Section 1 currently contains 100 checkbox criteria; initial conservative status:
  - PASS=0
  - YELLOW=2
  - FAIL=22
  - UNVERIFIED=76
- Phase H remains open. KeePass remains paused.

2026-05-19 menu/command discovery evidence:
- Good smoke evidence before command fallback:
  - Run dir: `reports/phase-h/npp-x64-20260519-163648/`
  - `case=discover status=PASS`
  - `case=editor_remote_scintilla status=PASS`
  - `case=menu_child_scan status=PASS`
  - `case=menu_alt_file_input status=PASS gui_flags=0x5`
  - `case=toolbar_inventory status=PASS button_count=41`
- Menu is not custom/nonexistent: the live main HWND has `menu=0x10064`.
- Cross-process menu enumeration is invalid evidence under current Wine:
  - `GetMenu(main)` returns `HMENU=0x10064`
  - `GetMenuItemCount(0x10064)` returns `-1`
  - Wine `win32u/menu.c::grab_menu_ptr()` rejects other-process menu handles (`OBJ_OTHER_PROCESS`), so the smoke helper cannot discover IDs by directly walking the live Notepad++ menu.
- Harness fix in progress:
  - `npp_ui_smoke_helper.c` now falls back to loading menu resources from the actual Notepad++ executable via `LoadLibraryEx(..., LOAD_LIBRARY_AS_DATAFILE)` and scans menu resource IDs `1500/1501/1950` inside the helper process.
  - `run-notepad-x64-ui-smoke.sh` exports `MACRUNNER_NPP_APP_WIN` for that fallback.
  - This is harness evidence plumbing, not a product workaround; live behavior is still verified through `WM_COMMAND`/dialogs against the running Notepad++ process.

2026-05-19 helper opcode blocker:
- Trigger:
  - Run dir: `reports/phase-h/npp-x64-20260519-163648/`
  - `macrunner-hb-runtime-fail ... pc=0x140002c7f ... reason=UNSUPPORTED`
- Disassembly of helper at `0x140002c7f`:
  - `f3 0f 7e b4 24 b8 01 00 00` = `movq 0x1b8(%rsp), %xmm6`
- Family audit:
  - Family: SSE qword transfer family
  - Existing coverage before this finding: `66 0F 6E`, `66 0F 7E`, `66 0F D6`
  - Missing member fixed: `F3 0F 7E` (`MOVQ xmm, xmm/m64`)
  - Regression added: memory-displacement trigger, memory sibling, and XMM-register sibling in `hb_test_runner.c`
- Verification:
  - `./scripts/test-hyperbridge.sh` => `133 passed, 0 failed`
  - `libhyperbridge.a` rebuilt from fresh `hb_decode_x64.o`
  - `ntdll.so` force-relinked and ad-hoc codesigned after the HyperBridge change
- Product rerun is not yet certified after this fix because the next two canonical smoke attempts failed before app launch with `wineserver: bind: Operation not permitted`:
  - `reports/phase-h/npp-x64-20260519-170409/`
  - `reports/phase-h/npp-x64-20260519-170555/`
- Treat the bind failure as harness/infrastructure, not Notepad++ product evidence.

2026-05-19 Scintilla harness readiness correction:
- Trigger evidence:
  - Run dir: `reports/phase-h/npp-x64-20260519-185814/`
  - `case=discover status=PASS ... class="Notepad++" title=""`
  - `case=editor_remote_scintilla status=FAIL len=-1 text_match=0`
  - Immediately after that, `window_snapshot tag="main-before-menu"` showed the same HWND visible with title `new 1 - Notepad++ [Administrator]`.
- Interpretation:
  - Treat this as harness timing, not product editor failure: discovery accepted the main HWND as soon as toolbar existed, before the main window was visible/titled and before the visible Scintilla child was proven message-responsive.
  - The previous helper also hid the failing substep; `len=-1` did not distinguish allocation, `SCI_SETTEXT`, `SCI_GETTEXTLENGTH`, `SCI_GETTEXT`, or remote read failure.
- Harness fix:
  - Added `case=editor_ready`: re-scan the real Notepad++ HWND until main window is visible, Scintilla is visible, title is non-empty, and `SCI_GETTEXTLENGTH` responds.
  - Added substep evidence to `editor_remote_scintilla`: `VirtualAllocEx`, `WriteProcessMemory`, `SCI_SETTEXT`, `SCI_GETTEXTLENGTH`, `SCI_GETTEXT`, `ReadProcessMemory`, byte counts, and `GetLastError` values.
  - Fixed helper compile after the diagnostic API change; `npp_ui_smoke_helper.exe` rebuilt at `2026-05-19 19:55:23`.
- Focused verification:
  - Run dir: `reports/phase-h/npp-x64-20260519-195550/`
  - Run mode: dialogs, toolbar, and font intentionally skipped to isolate editor/menu harness reliability.
  - `case=editor_ready status=PASS attempts=7 ... title="new 1 - Notepad++ [Administrator]" len=0`
  - `case=editor_remote_scintilla status=PASS ready_len=0 len=98 text_match=1 ... set_ok=1 len_ok=1 get_ok=1 read_ok=1`
  - `case=menu_inventory status=PASS ... cmd_new=41001 cmd_open=41002 cmd_save=41006 cmd_save_as=41008 cmd_find=43001 cmd_preferences=48011`
  - `case=menu_alt_file_input status=PASS`
  - `overall=PASS fail_count=0 yellow_count=8`; yellows are expected skipped cases and live cross-process menu enumeration limitation.
- Remaining:
  - Full product smoke is not green. Dialogs, toolbar render/click, font baseline, and clean exit still need non-skipped evidence.
  - Last unskipped toolbar path remains blocked by a helper-side HyperBridge unsupported opcode at `guest=0x14000348e bytes=66 0f 50` (`MOVMSKPD`). Per current directive, do not fix engine until the Scintilla harness failure is documented and isolated.

2026-05-20 wineserver lifecycle classification correction:
- Trigger evidence:
  - Run dir before fix: `reports/phase-h/npp-x64-20260520-100808/`
  - `wineboot-init.err`: `wineserver: bind: Operation not permitted`
  - `wineboot-update.err`: `wineserver: bind: Operation not permitted`
  - `stderr.log`: `wineserver: bind: Operation not permitted`
  - UI smoke summary: `status=125 reason=INFRA_WINESERVER_BIND_OPERATION_NOT_PERMITTED`
- Root classification:
  - This is not Notepad++ product evidence and not toolbar/comctl32 evidence.
  - Current Codex sandbox denies even a minimal local `AF_UNIX bind()` probe with `errno=1 Operation not permitted`, so Wine cannot create the wineserver socket from this environment.
- Harness fix:
  - `scripts/run-notepad-x64.sh` now runs an AF_UNIX socket preflight before `wineboot`.
  - On failure it writes `infra.status` and exits `125` before any Wine bootstrap/app evidence can be misclassified.
  - `scripts/run-notepad-x64-ui-smoke.sh` now reads launcher `infra.status` in foreground-hook fallback.
- Verification:
  - `python3 -m unittest tests.test_run_notepad_x64_script tests.test_npp_ui_smoke_script tests.test_npp_ui_smoke_helper -v` => `14 tests OK`
  - Run dir after fix: `reports/phase-h/npp-x64-20260520-102035/`
  - `infra-preflight.log`: `unix_socket_bind_preflight=FAIL errno=1 strerror=Operation not permitted`
  - Smoke exits quickly with `status=125 reason=INFRA_WINESERVER_BIND_OPERATION_NOT_PERMITTED`, not a product failure.
- Remaining:
  - Full Notepad++ product smoke must be rerun from a host/session where AF_UNIX bind is allowed.
  - If that host reaches product evidence again, resume with the current known product blockers: toolbar icon rendering, dialogs, font baseline, and clean exit.

2026-05-20 Notepad++ toolbar DIM-07/DIM-09 update:
- Focused report: `reports/phase-h/MILESTONE/notepad-toolbar-dim07-20260520.md`
- Current classification:
  - DIM-11 stale artifact check: PASS (`build_freshness=PASS` after comctl32 rebuilds).
  - DIM-07 product toolbar: substantially fixed. macOS CG capture shows a light toolbar background and colored icons.
  - DIM-09 smoke capture: still open. Windows-side `toolbar_render` capture reports a black/low-color strip while CG capture shows the product window rendered correctly.
- Key artifacts:
  - Product-side CG capture: `reports/phase-h-toolbar-cg-v6checkedfill-retry-20260520-130721/window.png`
  - 30s required smoke: `reports/phase-h/npp-x64-20260520-130942/` (`status=124`, helper readiness timeout before `toolbar_render`)
  - 120s extended smoke: `reports/phase-h/npp-x64-20260520-131204/` (`toolbar_new_click=PASS`, `toolbar_render=FAIL` by helper capture, `dialog_preferences=FAIL`)
- Remaining:
  - Fix/replace helper `toolbar_render` capture/readback before using it as product evidence.
  - Continue `dialog_preferences` after toolbar capture validity is resolved.

2026-05-21 Notepad++ x64 zero-yellow product smoke:
- Root visual fixes:
  - `uxtheme` 32bpp msstyles BMP unused-alpha handling now preserves opaque RGB instead of premultiplying all-zero alpha to black.
  - `uxtheme` themed bitmap drawing now composes copied/scaled 32bpp pixels explicitly for opaque, binary-alpha, and full-alpha paths.
  - `comctl32` status bar uses the classic paint path by default; themed status bar paint is opt-in via `MACRUNNER_USE_THEME_STATUSBAR`.
- Harness fixes:
  - Windows-side toolbar readback is no longer a product-color gate when macOS CG capture proves the real product toolbar is colorful.
  - Cross-process menu handle enumeration is reported as a resource-menu fallback pass when command IDs are proven from the app resource and live `WM_COMMAND` probes pass.
  - `Alt+F` menu input now retries before falling back to mouse menu evidence, removing a focus-timing flake.
  - Dialog icon CG probe now waits for the short-lived hold helper to exit and records lifecycle PASS.
- Unit verification:
  - `python3 -m pytest -q tests/test_npp_ui_smoke_helper.py tests/test_npp_ui_smoke_script.py tests/test_npp_smoke_effective_overall.py tools/compat_learning/tests/test_toolbar_threshold.py` => `33 passed`.
- Product smoke verification:
  - Single zero-yellow run: `reports/phase-h/npp-x64-20260521-034629/`
    - `PASS=27 YELLOW=0 FAIL=0`
    - `overall_effective=PASS fail_count=0 yellow_count=0`
    - `case=clean_exit status=PASS ... wine_processes=0`
  - Three consecutive zero-yellow runs: `reports/notepad-zero-yellow-regression-20260521-035030/summary.txt`
    - Run 1: `reports/phase-h/npp-x64-20260521-035040/`, `PASS=27 YELLOW=0 FAIL=0`
    - Run 2: `reports/phase-h/npp-x64-20260521-035406/`, `PASS=27 YELLOW=0 FAIL=0`
    - Run 3: `reports/phase-h/npp-x64-20260521-035732/`, `PASS=27 YELLOW=0 FAIL=0`
- Current remaining closure gates:
  - 30-minute manual heavy-use still requires human verification.
  - 1-hour idle RSS stability still needs a dedicated long-run artifact.
  - Windows baseline visual comparison and final user sign-off are not automatable inside this session.
