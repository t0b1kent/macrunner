# MASTER LANE: make Hollow Knight actually playable on native ARM64 macOS

You are the **only** lane. You are running in an auto-loop and will be relaunched in fresh
threads until you write `LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-MASTER-PROGRESS.md`. Work continuously; do not wait for a human.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## The goal — far, and deliberately so

Hollow Knight is **playable**: it boots to its main menu, an injected key changes the menu
selection, "Start Game" enters the game, the window renders actual pixels (not black), and audio
initialises. Report `GOAL` only when you have evidence for all five, captured by you.

Everything short of that is `LOOP-STATUS: CONTINUE`. Do not report GOAL for "the fix looks
right", "the probe passes", or "the marker appeared" — this project has been burned by each.

## Sequence — do them in this order, they unblock each other

1. **Menu, reliably.** Currently reachable only with the driver install deferred
   (`MACRUNNER_MACDRV_SELFINIT_DELAY_MS`). Understand *why* installing the driver during the
   load window costs ~558 s in the Mono phase, and fix it properly rather than living on the
   delay. Measured: `Begin MonoManager`→`UnloadTime` is 784 s / 781 s with the driver, **212.6 s**
   without it; healthy-era max is 237.7 s.
2. **Input.** Keyboard is proven on `tools/winkeyprobe.c` (guest `WM_KEYDOWN/UP/CHAR` 0→5/5/5).
   Mouse is proven to reach the guest with correct coordinates. Neither has been proven **in HK**.
3. **Black frame.** The menu scene loads (`Loaded Objects now`, `Restored language`) and the
   window is still black — `shot-00-before.bmp` 1512×982, `nonblack=0`. Orthogonal to input;
   tracked on this branch as `hk/black-frame-root-cause`.
4. **Audio.** `winecoreaudio.drv` now reaches the prefix (commit `2aa93b2b`) — it never did
   before, which is why HK logged `FMOD failed to initialize any audio devices, running on
   emulated software output with no sound` at +55.7 s. Driver arriving is necessary; whether
   sound works is **unmeasured**. Verify it in a run that actually reaches audio init — a zero
   FMOD-error count in a run that never reached `Begin MonoManager` measures nothing.

## Established — do not re-derive, do not re-litigate

- **`win32u/driver.c`** (`88894334`): the re-entrancy placeholder permanently beat the real mac
  driver through `__wine_set_user_driver`'s CAS, so every process ran on the null driver while
  win32u reported the window visible/foreground/focused/active. Fixed.
- **`winemac.drv/event.c`**: `macdrv_return_route_observe()` touched the TEB from the Cocoa main
  thread, killing the thread that owns `[NSApp run]`; every later `dispatch_sync` onto the main
  queue then blocked forever. Fixed and verified by disassembly. The class was swept: of 10 C
  functions called from Cocoa, only that one and `is_skyrim_se_launcher()` touch TEB/win32u.
- **`signal_arm64.c`** (`202ade1b`): the fault router had no re-entrancy guard outside trace
  builds, so a fault inside its own PC classifier recursed forever — a silent 100 %-CPU hang that
  reads exactly like a guest livelock. Now breaks with `macrunner-hb-fault-reentry-break`.
- **`kernelbase`**: the ARM64X twins are NOT uninitialised — 0 EC-zero of 141 pairs. The EC
  mirror itself was zeroing them. Disabling the mirror is correct analysis and **kills the
  guest** (`exit=5`, twice); reverted in `08b006dc`. Leave it alone unless you A/B it live.
- **Refuted, with evidence:** service window station / `services.exe` / desktop seeding;
  "`[NSApp run]` returns"; nested run loop; bundle-lessness; the `MemoryWineUnixFuncs` bridge for
  `winemac.drv`; the ntdll build-epoch regression (run 19 arm B on clean-HEAD ntdll wedged
  identically); SMC guards; `GC_stop_world`.
- **Open, named:** `winemac.drv` is the only wine builtin in HK's process with no native aarch64
  twin (37 others get one), and no `dllmain_attach` for HK's wine pid. The ARM64X file maps
  cleanly (base moves, still `builtin`), so it is not the file. `User32LoadDriver`
  (`user32/user_main.c:130`) pins the search path to `c:\windows\system32`, which is a fact about
  the code and not yet a proven cause.

## Instruments and traps — every one of these cost a run or a day

- **A zero from a gate you did not enable, or a pattern you misspelled, is not a measurement.**
  `macrunner-get-win-data` has HYPHENS. `app_sendEvent_enter` counts **mouse buttons only**
  (`cocoa_app.m:92`). Several traces are `TRACE()` and channel-gated off. Before believing any
  zero: `strings -a <shipped artifact> | grep <marker>` and confirm the gate is on.
- **`winecfg` does not reproduce input bugs** — it never reaches `macdrv_init`. Use
  `tools/winkeyprobe.c` (one `WS_VISIBLE` top-level window + message loop + counters, ~40 s).
  Prefer a 40-second probe over a 45-minute title run in every case where it answers the question.
- **Build:** `scripts/build-wine-arm64ec-spike.sh` does **not** build hyperbridge — run
  `make -C engine/hyperbridge` with `SDKROOT="$(xcrun --show-sdk-path)"` FIRST, then relink. The
  shipping tree is `engine/wine/build-arm64ec-spike/`; the in-tree `dlls/ntdll/ntdll.so` is a
  stale decoy. **Verify every build by the SHA *and* the content (`strings`) of what lands in
  `dist-arm64ec-spike`** — a wine-only build has reported success while shipping a byte-identical
  `ntdll.so`, and an SHA-only guard has produced a false abort when a sister build was identical.
- **Slot:** `mr-run.sh` now takes an atomic `mkdir` lock (`e98ca140`) and exits 75 if it cannot.
  Do not add your own `ps`-based check-then-launch; that gap put three HK processes on one
  machine today. Count on `comm`, never `ps -Awwo args | grep` — that matches your own command
  line and has reported a busy slot on an idle machine.
- **`exit=53`** is the known xtajit64-c0000135 boot flake — retry it, never call it a regression.
- **`MACRUNNER_WINE_BIN`** exported in the environment silently forces the stale
  `engine/wine/dist`. Before believing any regression read `final-child.json` →
  `argv.entries[0].value` and confirm which binary ran.
- **`MACRUNNER_TRACE_UI_INPUT=1` is not win32u-only** — it also lights HyperBridge's per-guest-call
  tracing and buries the run.
- **An offline proof that a mechanism is wrong is not a proof that removing it is safe.** Ship
  behind a default-OFF flag and A/B against a running guest.
- **Register your prediction before the run.** This project's best results today came from
  pre-registered A/Bs; its worst hours came from reading a result after the fact.

## Territory and rules

- **Everything is yours** — you are the only lane. `engine/**`, `scripts/**`, `tools/**`,
  `reports/**`. With that comes the obligation to keep edits surgical and reversible.
- **Never `pkill`/`killall`** — scope by verified PID, supervisor-first.
- **Never `git add -A`/`git add .`. No commits** — the coordinator commits. If work is
  substantial, preserve it in a checkpoint under `reports/phase4-hollow-knight/checkpoints/`.
- **Never delete `artifacts/hk-*prefix-template*`.**
- Sample a stalled process rather than paying for another run: a live `sample` has refuted more
  hypotheses here than any rerun.

## Reporting

One report per iteration under `reports/phase4-hollow-knight/`. Append a heartbeat line per step
to `reports/research/LANE-HK-MASTER-PROGRESS.md`. State the counts you measured, not the ones you
expected. Mark every unproven statement `[HYPOTHESIS]`. When you refute your own earlier entry,
say so plainly — that has been the most valuable thing written in these journals.

Prior journals, for context you may consult but must not re-derive:
`LANE-HK-E2E-PROGRESS.md`, `LANE-HK-DLLMAIN-PROGRESS.md`, `LANE-HK-NSAPP-PROGRESS.md`,
`LANE-HK-MOUSE-PROGRESS.md`, `LANE-HK-TWINSWEEP-PROGRESS.md`, `LANE-HK-INPUT-PROGRESS.md`.

## Termination

- `LOOP-STATUS: GOAL` — menu, input, game start, real pixels, audio. All five, with evidence.
- `LOOP-STATUS: BLOCKED` — something only the operator can decide. One sentence.
- Otherwise keep going.
