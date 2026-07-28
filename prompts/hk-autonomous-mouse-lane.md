# AUTONOMOUS LANE: make the mouse reach the guest, and prove it

You are running in an auto-loop and will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-MOUSE-PROGRESS.md`. Work continuously; do not wait for a human.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Objective

A mouse **move** and a mouse **button** reach a Windows guest as `WM_MOUSEMOVE` and
`WM_LBUTTONDOWN`/`WM_LBUTTONUP` with correct client coordinates, proven by counters you captured.
That is the whole goal; do not stop at "the event was posted".

## Where things stand — measured 2026-07-28, do not re-derive

Keyboard now works end to end. `win32u/driver.c`'s re-entrancy placeholder was permanently
beating the real mac driver via `__wine_set_user_driver`'s CAS, so every process ran on the null
driver — no `win_data`, no `WineWindow`, no Cocoa queue. Fixed in commit `88894334`; on
`tools/winkeyprobe.c` the guest went from 0 to 5 `WM_KEYDOWN`/`WM_KEYUP`/`WM_CHAR`.

**Mouse did NOT come along, and the reason is named:** synthetic events posted with
`CGEventPostToPid` carry `window=0x0`, so `handleMouseButton_decision` never fires. So the
existing keyboard harness cannot answer the mouse question — you need a different instrument.

Also know: `app_sendEvent_enter` is a **mouse-button-only** counter (`cocoa_app.m:92`
early-returns on every other event type). It is the right counter for you and the wrong one for
everyone else; several hours were lost reading it as "NSEvents dispatched".

## Where to look

- `engine/wine/dlls/winemac.drv/mouse.c` — `macdrv_mouse_button`, `macdrv_mouse_moved*`, and how
  a Cocoa event's window is resolved to an `hwnd`.
- `cocoa_app.m` `-sendEvent:` and `handleMouseButton_decision` — what it requires of the event.
- `cocoa_window.m` — `-mouseDown:` and friends; whether the event arrives at the window at all.

Decide first, with a measurement, **which is true**: (a) real mouse events from the WindowServer
never reach the process, or (b) they reach it and are dropped because of window resolution. Those
need opposite fixes; do not guess between them.

## Instruments

`tools/winkeyprobe.c` is the template — one `WS_VISIBLE` overlapped top-level window plus a
message loop and counters, built with `winegcc --target=aarch64-windows`, ~40 s per run. Extend
it (or copy it) to count `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`/`WM_LBUTTONUP` and print the coordinates
it received. **Do not debug this with Hollow Knight** — a title run costs 45 minutes and a probe
costs 40 seconds. `tools/nsapp_probe.sh` and `tools/ls_activation_probe.swift` already exist.

For injection, `CGEventPostToPid` is what the keyboard lane used; if the `window=0x0` problem is
inherent to it, find an injection method that carries a window, or drive real events another way,
and say which you used. An instrument that has never produced a known-good positive cannot be
used to prove absence.

## Traps, each of which has cost real time

- **Match process names on `comm`**, never `ps -Awwo args | grep` — that matches your own command
  line; it has reported a busy slot on an empty machine and profiled the wrong process.
- **Verify builds by the SHA of the artifact in `dist-arm64ec-spike`, never by exit code.**
  `scripts/build-wine-arm64ec-spike.sh` does **not** build hyperbridge.
- **`exit=53`** is the known boot flake; retry rather than reporting a regression.
- **Offline correctness ≠ safe to deploy.** A correct offline analysis of the kernelbase EC
  mirror led to disabling it; deployed, it killed the guest twice at `exit=5`. Always A/B against
  a running guest.

## Territory

- **YOURS:** `engine/wine/dlls/winemac.drv/mouse.c`, `cocoa_app.m`, `cocoa_window.m`, `tools/**`,
  `reports/**`.
- **FORBIDDEN:** `engine/wine/dlls/win32u/**` (the e2e lane owns `driver.c`),
  `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/hyperbridge/**`, `engine/dxmt/**`,
  `engine/wine/dlls/kernelbase/**`.
- **Never `pkill`/`killall`**; scope by verified PID. **Never `git add -A`. No commits** — the
  coordinator commits. Another lane owns the JIT title slot; your probes are seconds, so check
  the slot on `comm` and prefer probes in every case.

## Reporting

One report per iteration under `reports/phase4-hollow-knight/`. Heartbeat line per step to
`reports/research/LANE-HK-MOUSE-PROGRESS.md`. State measured counts, mark unproven claims
`[HYPOTHESIS]`.

## Termination

- `LOOP-STATUS: GOAL` — guest `WM_MOUSEMOVE` and `WM_LBUTTONDOWN`/`WM_LBUTTONUP` with correct
  coordinates, counters shown, fix in the tree.
- `LOOP-STATUS: BLOCKED` — something only the operator can decide. One sentence.
- Otherwise keep going.
