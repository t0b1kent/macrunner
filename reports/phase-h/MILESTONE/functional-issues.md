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

## Next Priority

1. Establish reliable UI smoke harness:
   - Windows-side helper that sends/reads Scintilla messages for editor content.
   - Separate native event-route probe for actual mouse/keyboard clicks.
2. Re-test menu/buttons with evidence.
3. If clicks fail, sample/log exact Win32 route: hit-test -> mouse message -> window proc -> command dispatch.
4. If fonts remain rough, isolate text path: `TextOutW` / `ExtTextOutW` / `GetTextExtentPoint*` / FreeType glyph metrics.

## Rule

No broad fixes from screenshots alone. Each failing item becomes a narrow bug with evidence, then family audit if the class is architectural.
