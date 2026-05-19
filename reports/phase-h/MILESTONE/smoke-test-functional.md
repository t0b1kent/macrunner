# Notepad++ x64 Functional Smoke Test

Date: 2026-05-19
Commit: `d699ddb` (`Fix scalar SSE move widths for Notepad++ painting`)
Tags:
- `v0.1-engine-phase-h-x64-notepad-plus-plus-visible`
- `v0.1-engine-phase-h-x64-notepad-plus-plus-functional`

Screenshot: `/Volumes/MacOS/MacRunner/reports/phase-h/MILESTONE/notepad-plus-plus-FUNCTIONAL-20260519.png`

## 2026-05-19 Correction

This report is now reclassified as a **functional checkpoint**, not commercial closure.

The tags remain useful recovery/history markers, but screenshot review shows the product is still rough: font metrics are off, native appearance is not yet correct, and menu/button/dialog behavior has not been systematically certified. The authoritative issue tracker for the remaining Phase H work is:

`/Volumes/MacOS/MacRunner/reports/phase-h/MILESTONE/functional-issues.md`

## Confirmed Pass

- Main window opens with title bar, menu bar, editor surface, line number gutter, caret, and client area.
- Text input works in the editor. Confirmed visible typed text: `fgdfg`.
- Buttons / basic UI activation was observed manually once after scalar SSE width fix, but is no longer treated as fully certified until the functional checklist is green.
- Paint path progressed beyond blank-window state; editor content and chrome render.
- Process remains alive without HyperBridge fault / unsupported opcode / Program Error in the canonical run.
- `sample` showed normal Win32 message/event activity (`send_hardware_message`, `peek_message`, `dispatch_win_proc_params`) rather than a hard crash.

## Engine Regression Coverage

- `./scripts/test-hyperbridge.sh` passed after the fix.
- C runner: `130 passed, 0 failed`.
- Python suite: `41 passed, 0 failed`.
- New regression: `interp_x64_scalar_sse_move_store_width_notepadpp_paint`.
- Covered family: `MOVSD` 8-byte store, `MOVSS` 4-byte store, `MOVUPS` 16-byte sibling.

## Extended Smoke Queue

These are Phase-H functional checks, not post-closure polish. KeePass should not start until this checklist is either green or the remaining failures are explicitly accepted as post-Phase-H scope.

- File -> New -> type text -> Save via UI dialog.
- File -> Open -> load saved file -> verify content.
- Edit -> Find -> search typed string.
- View / Style / font rendering checks.
- Settings -> Preferences -> tabs/panels render and accept input.

## Notes

The earlier suspected PUSH/RDI bug was disproven. Evidence showed `push %rdi` saved the correct value; the real overwrite was scalar SSE `MOVSD/MOVSS` being decoded/lifted as a 128-bit XMM move and writing across an adjacent saved register slot. The committed fix keeps packed SSE moves 16-byte while preserving scalar widths.
