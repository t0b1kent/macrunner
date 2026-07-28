# MAIN MENU REACHED — Hollow Knight's title screen on native ARM64 (2026-07-28)

**Class:** `VERIFIED_ONSCREEN_NOT_PLAYABLE`. The game reaches and renders its main menu.
It does **not** respond to input, so it is not playable.

## The frame

`evidence/HK-MAIN-MENU.png` — the full title screen: HOLLOW KNIGHT logo with the ornamental
flourish, START GAME / OPTIONS / ACHIEVEMENTS / EXTRAS / QUIT GAME, version 1.5.12620 and the
achievement icons bottom-left, the team cherry emblem bottom-right, atmospheric particle
background. Native ARM64, no Rosetta.

Run: `laneA-INPUT-TEST-manual-language-FAST4of5-LONG-123136`, menu on screen at **+47 min**.

## What made it reachable — five findings, all from today

1. **Compositor.** `GA_ROOT(hwnd)=0` made `client_surface_update` early-return, so the Metal
   view was never parented. 19-line toplevel fallback in `winemac.drv` → first visible pixel.
2. **Stale translations.** The guest icache-flush callback was a no-op stub
   (`xtajit64/unixlib.c:710`), so Mono ran against stale translated code. Proven live with
   3295 evictions.
3. **Registry.** Our own `macrunner_hb_try_registry_semantic` faked all seven advapi32
   W-registry APIs since 2026-06-01, breaking reads **and writes** for every title. Removing
   it (50 lines) let the game read its saved language and skip the menu gate entirely.
4. **LDP-fusion stale base.** The JIT fused two loads into an ARM64 LDP using a stale base,
   corrupting a pointer to `0xffffffff01000166`. Bisected to `MACRUNNER_HB_JIT_DIRECT_MEM`
   alone (the other four direct paths are innocent); the engine lane reproduced the exact
   value synthetically, with zero HK involved, and fixed it.
5. **Budgets.** Measured time-to-menu is **2782 s** while runs were budgeted at 2400 s. The
   runs that "never reached the menu" were being cut short, not blocked. This run used 5400 s.

Config that got here: `DIRECT_MEM=0` (the faulting one) with `XMM_MEM`, `SCALAR_MEM`,
`SCALAR_SCAN`, `STACK` all ON. That combination also cut RSS from 5.0 GB to 1.4 GB.

## What is NOT claimed

- **The game is not playable.** The operator pressed keys and clicked many times over several
  minutes; the menu never responded, no scene change, no input line in the log. **No actuator
  was armed in this run**, so nothing else explains it. Input is now the single blocker.
- The game was demonstrably alive throughout: CPU 217-261%, RSS 1.4 GB, menu scene loaded
  (`Loaded Objects now: 47240`, 11.5 s GC), `present-surface ordinal=200 result=ok` still
  cycling. Not a hang, not a render failure — it renders and presents while receiving nothing.
- `Making UI menu lean.` never appeared in the log even though the menu is plainly on screen,
  so oracle-marker tracking is incomplete and the screen is the better witness.
- 18 × `VirtualAlloc STATUS_CONFLICTING_ADDRESSES` still occur; unexplained.

## Next

Input. Leading [HYPOTHESIS]: the same null `GA_ROOT` that blocked view parenting also drops
events at `winemac.drv/window.c:812`; yesterday's fix repaired only the rendering path.
First step is a counted trace on `macdrv_key_event` / `macdrv_mouse_button`, **proven by a
control before its silence is read as evidence**.

## Integrity

`SHA256SUMS` covers every file except itself.
