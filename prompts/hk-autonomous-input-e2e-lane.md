# AUTONOMOUS LANE: Hollow Knight must reach its menu AND respond to a key

You are running in an auto-loop and will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-E2E-PROGRESS.md`. Work continuously; do not wait for a human.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## The objective — this is a FAR goal, do not declare it early

Hollow Knight boots to its **main menu** and a **synthetic key press changes the menu
selection**, proven by evidence you captured yourself (guest `WM_KEYDOWN` for the key AND a
visible change: a new marker in the log, or a screenshot diff). Anything short of that is
`LOOP-STATUS: CONTINUE`. Do not report GOAL for "the fix looks right" or "the probe passes".

## Where things stand — measured today, do not re-derive

The input path is **fixed and proven on a probe**, commit `88894334`. `win32u/driver.c`'s
re-entrancy placeholder was permanently beating the real mac driver through
`__wine_set_user_driver`'s CAS, so every process ran on the null driver: no `win_data`, no
`WineWindow`, no Cocoa queue — while win32u reported the window visible/foreground/focused/active.
After the fix, on `tools/winkeyprobe.c`: `macdrv_WindowPosChanging` 0→8, `create_cocoa_window`
0→1, policy `prohibited`→`regular`, guest `WM_KEYDOWN/WM_KEYUP/WM_CHAR` 0→5/5/5. Keys are
injected with `CGEventPostToPid`, so **you never need a human at the keyboard**.

**Hollow Knight itself has never been run through that fix.** That is your job.

## The immediate blocker, already narrowed

HK now dies at `exit=1` with `d3d11: failed to create device and context (887a0002)` =
`DXGI_ERROR_NOT_FOUND`, twice, ~2026 log lines, right after `Initialize engine version`.

**It is launcher-configuration, not the engine.** Runs through `scripts/laneA-run-hk.sh`
(try18 17:28, try20 18:35) had `d3d11_fail=0` and reached `Restored language`. Runs through
`scripts/hk-run-try12-config.sh` — a hand-rebuilt env I wrote from try12's captured
`run-contract.json` — fail d3d11 every time. The likely cause is that the contract records the
**final child** environment, i.e. values `mr-run.sh` DERIVES, and feeding those back in as inputs
breaks them; `MACRUNNER_DXMT_ROOT` was one such and removing it was not enough. Diff the two
launchers' effective env and find what DXMT actually needs. Prefer fixing
`hk-run-try12-config.sh` or just using `laneA-run-hk.sh`, whichever gets a window on screen.

Note `laneA-run-hk.sh` reaches level start in roughly 1.2% of runs historically, so treat single
negative runs on it as noise and use its retry argument.

## Traps that have each cost this project real time

- **`exit=53` is the known xtajit64-c0000135 boot flake** (documented in `laneA-run-hk.sh`'s
  header). Retry it; do not report it as a regression.
- **Match process names on `comm`, never `ps -Awwo args | grep`** — the latter matches your own
  command line and has reported "busy" against an empty machine and profiled the wrong process.
- **`scripts/build-wine-arm64ec-spike.sh` does NOT build hyperbridge.** If you touch
  `engine/hyperbridge/**` you must `make -C engine/hyperbridge` with
  `SDKROOT="$(xcrun --show-sdk-path)"` FIRST, then relink wine. **Verify every build by the SHA of
  the file that lands in `dist-arm64ec-spike`, never by exit code** — a wine-only build reported
  success and shipped byte-identical `ntdll.so`.
- **`MACRUNNER_WINE_BIN`:** `mr-run.sh` sources `config/env.sh`, which assigns it with `:=`.
  Fixed in `a4f67e5d` so the dist argument wins, but before believing any `LADDER_REGRESSION`,
  read `final-child.json` → `argv.entries[0].value` and confirm which binary actually ran.
- **An offline proof that a mechanism is wrong is NOT a proof that removing it is safe.**
  `666f6614` disabled the kernelbase EC mirror on correct offline analysis; deployed, it killed
  the guest at `exit=5`/1359 lines, twice. Reverted in `08b006dc`. Deploy behind an A/B and read
  the guest.
- **Mouse is unproven:** synthetic `CGEventPostToPid` events carry `window=0x0`, so
  `handleMouseButton_decision` never fires. Keyboard is the proven channel — a sister lane owns
  mouse.

## Territory

- **YOURS:** `engine/wine/dlls/win32u/driver.c`, `engine/wine/dlls/winemac.drv/**`, `scripts/**`
  (launcher configs), `tools/**`, `reports/**`.
- **EXCEPTION GRANTED 2026-07-29 by the coordinator:** you MAY edit the guest unixlib bridge in
  `engine/wine/dlls/ntdll/unix/macrunner_hb.c` — the `MemoryWineUnixFuncs` fallback next to the
  hardcoded `winemetal.dll` branch, and nothing else in that file. Another lane holds uncommitted
  work there, so keep the edit inside that one `if/else if` chain and never revert or reformat
  anything around it.
- **STATE OF THAT FIX, measured — start here, do not redo it.** The coordinator already added a
  `winemac.drv` branch to that bridge (dlopen of the sibling `aarch64-unix/winemac.so`, located
  via `dladdr` rather than `MACRUNNER_WINE_DIST`, whose stale default has already misdirected whole
  runs). It **did not fire**: `macrunner-hb-winemac-unixlib-bridge` = 0 and `PLACEHOLDER KEPT` = 1
  in `laneA-WINEMACBRIDGE-try24-a1-try1-033507`. An ungated diagnostic is now in the same place —
  `macrunner-hb-unixfuncs-miss: module=%s status=%08x len=%zu`, printed for EVERY module that
  fails `MemoryWineUnixFuncs` — precisely because "the name did not match" and "this handler is
  never reached for winemac.drv" need opposite fixes. Read that line's output first; it settles
  which. Gate for A/B: `MACRUNNER_HB_WINEMAC_UNIXLIB_BRIDGE=0` restores the old behaviour.
- **FORBIDDEN:** `engine/hyperbridge/**`,
  `engine/dxmt/**`, `engine/wine/dlls/kernelbase/locale.c`, `engine/wine/dlls/win32u/input.c` —
  other lanes hold uncommitted work there.
- **Never `pkill`/`killall`** — scope by verified PID. **Never `git add -A`. No commits** — the
  coordinator commits. You own the single JIT title slot; check it before每 launch.

## Reporting

One report per iteration under `reports/phase4-hollow-knight/`. Append a heartbeat line per step
to `reports/research/LANE-HK-E2E-PROGRESS.md`. State the counts you measured, not the ones you
expected. Mark every unproven statement `[HYPOTHESIS]`.

## Termination

- `LOOP-STATUS: GOAL` — HK at its main menu, a synthetic key press produces guest `WM_KEYDOWN`
  **and** an observable change in the game, evidence captured.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide. One sentence.
- Otherwise keep going.
