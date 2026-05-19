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
