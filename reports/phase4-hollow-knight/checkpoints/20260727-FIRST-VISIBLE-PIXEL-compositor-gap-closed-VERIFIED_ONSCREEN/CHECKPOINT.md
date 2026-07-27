# FIRST VISIBLE PIXEL — Hollow Knight on screen, native ARM64 (2026-07-27)

**Class:** `VERIFIED_ONSCREEN`. Hollow Knight's language-select menu is rendered in a real
macOS window, captured through ScreenCaptureKit — a path that does not touch our readback,
DXMT or Metal. This is the project's first visible pixel. The game is **not** playable: it
sits on that menu.

## The evidence

`evidence/FIRST-VISIBLE-PIXEL-language-select.png` (and the raw `.bmp`) — run10
`sck-watch-16`, +16 min: the language menu with English, Français, Deutsch, Español,
Italiano, Português (Brasil), Русский, 日本語, 简体中文, 繁體中文, 한국어, ornaments flanking
the selected item. Latin, Cyrillic and CJK glyphs all correct.

- window capture: `nonblack=12933` by full scan, `max=255`; watcher JSON `"nonblack":13924`
- capture mode `ScreenCaptureKit-window`, `wid=20774` — the real window
- stable, not a flash: 23 of 36 watcher samples report `nonblack=13924`, 13 report 0

## What was wrong, and how it was found

The magenta control settled it. run8 injected `MTLClearColorMake(1,0,1,1)` into the
presented drawable's own texture (`winemetal_unix.c:1557-1584`), with both gates honoured —
`c1-inject fired` in the log, and the capture path proven live by capturing Safari at
1.43M non-black pixels. Result: **readback fully magenta 786432/786432, window fully black
786432/786432.** An lldb probe found the visible `WineWindow` `contentView` had ZERO
subviews; the Metal view was never parented (`superview=nil hidden=NO window=nil`).

Root cause: at swapchain creation `my_get_win_data → macdrv_client_surface_create` saw
`GA_ROOT(hwnd) = 0`, so `client_surface_update` took an early return and never parented the
view. Present then dutifully unhid a view attached to nothing.

Fix: `fix/winemac-toplevel-fallback.patch` — 41 lines, 19 insertions across
`winemac.drv/window.c` and `macdrv.h`, a toplevel fallback in `macdrv_client_surface_update`.
Live proof in run10: `client_surface_update toplevel_fallback hwnd=0x2002e reason=GA_ROOT_null`.

This retroactively vindicates every graphics layer cleared earlier: C2/C3 magenta at
655360/655360, 152/152 shaders translating clean, backbuffer RTV bound 210/210
last-before-`Present1`. All of it was true. The image was being produced correctly and lost
at the very last step, past D3D, in the Cocoa-window ↔ Metal-view attachment.

## What this does NOT claim

- **The game does not advance.** `Performing automatic level start.` = 0,
  `Loaded saved language code` = 0, `Making UI menu lean.` = 0, `Opening_Sequence` = 0.
  It sits on the language menu; in run8 frames 200-1800 were byte-identical.
- Parenting the view did **not** by itself unblock the language gate, so **input is a
  separate gap**: the window is visible now, but a click apparently still does not reach it.
  The language gate holds `allowSceneActivation=false` pending `ConfirmLanguage`.
- 13 of 36 watcher samples were black. That alternation is unexplained and should be looked
  at rather than waved away.
- The SMC/stale-translation work (`fix/engine-smc-reverify.patch`, preserved here too)
  remains a separate, real fix — but the remaining managed stall was already shown NOT to be
  stale translation.

## Both engine changes live only as patches here

Neither `winemac-toplevel-fallback.patch` nor `engine-smc-reverify.patch` is committed;
the lane is still iterating on the tree. **If the working tree is lost, these two files are
the fix.**

## Next

The language actuator (already built for this project, profile-145129) to deliver
`ConfirmLanguage`, then re-check the oracle marker sequence:
`Loaded saved language code 'EN'` → `Making UI menu lean.` → `Opening_Sequence` → gameplay.

## Integrity

`SHA256SUMS` covers every file except itself; credential sweep clean; no `final-child*`.
