# AUTONOMOUS LANE: `[NSApp run]` is running and never dispatches a single NSEvent

You are running in an auto-loop. You will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-NSAPP-PROGRESS.md`. Work continuously; do not wait for a human.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## The bug, measured — do not re-derive any of this

Hollow Knight renders but receives no keyboard and no mouse input. As of 2026-07-28 the input
path exists end to end and the remaining defect is **one edge**:

```
macdrv_init_entry        8     ← the unix driver initialises
run_cocoa_app_entry      8     ← run_cocoa_app is performed ON THE MAIN THREAD
run_cocoa_app_created          ← nsapp=0x… class=WineApplication
run_cocoa_app_decide           ← created_app=1 success=1 already_entered=0 will_run=1
create_cocoa_window      8     ← real Cocoa windows exist and draw
nsapp_run_returned       0     ← [NSApp run] was entered and has NOT returned
app_sendEvent_enter      0     ← ZERO NSEvents dispatched, ever
applicationDidBecomeActive 0   ← the app never becomes active
```

On Hollow Knight, with the operator pressing keys the whole time, the events that DID reach the
Wine queue were 2346 × `CLIENT_SURFACE_PRESENTED`, 4 × `WINDOW_FRAME_CHANGED`, 2 ×
`WINDOW_GOT_FOCUS` and **zero** of `KEY_PRESS`(8) / `KEY_RELEASE`(9) / `MOUSE_BUTTON`(12) /
`MOUSE_MOVED_*`(13,14). Those numbers are the `macdrv_cocoa.h` event enum, NOT NSEvent types.

So: the Cocoa app exists, its windows draw, `[NSApp run]` is inside its loop — and AppKit
delivers nothing. **Why does a running `[NSApp run]` dispatch no events?** That is the whole task.

## Your instrument is 40 seconds, not 45 minutes — this is the most important line here

**Do NOT debug this with Hollow Knight runs.** `winecfg` reproduces it exactly:

```bash
cd <repo> && . config/env.sh
export WINEPREFIX=/tmp/mr-agents/nsapp-probe MACRUNNER_TRACE_WINEMAC_INPUT=1
rm -rf "$WINEPREFIX" && mkdir -p "$WINEPREFIX"
engine/wine/dist-arm64ec-spike/bin/wine winecfg > /tmp/mr-agents/probe.log 2>&1 &
```

Then count the stages above in `probe.log`. A title run costs 45 minutes and the operator's
attention; this costs 40 seconds and reproduces the same signature. Half a day was lost to not
having this.

## Refuted already — do not spend a loop on these

- **"`[NSApp run]` returns and the main thread falls back into the loader's CFRunLoopRun."**
  A prior lane inferred this and built a bounded re-entry loop (`cocoa_main.m:306-381`).
  `nsapp_run_returned` is UNGATED and reads **0**: it does not return. The re-entry loop is
  therefore dead code for this failure — do not tune it.
- **Wrapping `dist/bin/wine` in an `.app` bundle.** `loader.c:678 create_tempdir` re-execs the
  loader from `$TMPDIR/winetemp-<inode>-<size>-<mtime>/<exename>`, so the running image is
  outside any bundle no matter what was launched. `artifacts/MacRunnerWine.app` exists and
  `wine --version` works from it; `mr-run.sh:57` now honours `MACRUNNER_WINE_BIN`. If you want to
  test the bundle theory properly you must build the bundle **inside that tempdir**.
- **`activateIgnoringOtherApps:` being the old API.** `tryToActivateIgnoringOtherApps:`
  (`cocoa_app.m:2276`) already calls the modern `[NSApp activate]` at the end.
- **`is_service_process()` / the window station / `services.exe` / desktop seeding.** All closed
  by measurement; the driver loads and `macdrv_init` runs.

## Leads worth measuring, none confirmed — mark every one `[HYPOTHESIS]`

- **Nested run loop.** `run_cocoa_app` is a `CFRunLoopSource` perform callback
  (`cocoa_main.m:420`) scheduled on `CFRunLoopGetMain()` by `macdrv_start_cocoa_app`, and ntdll's
  `apple_main_thread()` (`loader.c:3245-3276`) is what runs `CFRunLoopRun()` on the real main
  thread. So `[NSApp run]` is entered from **inside** a run-loop callback and never returns —
  the outer `CFRunLoopRun` never regains control. Does AppKit dispatch correctly in that nesting?
  Get a live `sample` of the main thread and read the actual frames.
- **Process/WindowServer state.** Windows draw, so there is a WindowServer connection, but the
  app never activates and gets no events. Check the activation policy actually in effect,
  `TransformProcessType`, and whether the process is registered with LaunchServices
  (`lsappinfo info -only name,bundleid <pid>` reported `CFBundleIdentifier=[NULL]`).
- **`nextEventMatchingMask` starvation.** If something else drains the event queue, or the app
  runs in a run-loop mode AppKit does not treat as an event mode, `-sendEvent:` is never reached.

## Getting a PID — this trap has bitten three times today

`ps -Awwo args | grep <pattern>` **matches your own command line**, because your command
contains the pattern. It reported "2 processes busy" against an empty machine and made a
`sample` profile my own zsh. Match on `comm` instead:

```bash
ps -Awwo pid,comm | awk '$2 ~ /winecfg\.exe$/ {print $1}'
```

## Building — `build-wine-arm64ec-spike.sh` does NOT build hyperbridge

For `winemac.drv` changes a wine build is enough. If you ever touch `engine/hyperbridge/**`, you
must `make -C engine/hyperbridge` (with `SDKROOT="$(xcrun --show-sdk-path)"`) FIRST and only then
relink wine — otherwise the build reports success and ships the old code. Verified today by SHA:
`ntdll.so` was byte-identical after a "successful" wine-only build. **Always verify the artifact
by SHA, never by exit code.**

## Territory

- **YOURS:** `engine/wine/dlls/winemac.drv/**`, `engine/wine/dlls/ntdll/unix/loader.c` (the
  `apple_main_thread` region only), `tools/**`, `reports/**`.
- **FORBIDDEN:** `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/hyperbridge/**`,
  `engine/dxmt/**` — other lanes hold uncommitted work there.
- **Never `pkill`/`killall`**; scope by verified PID. Never `git add -A`. **No commits** — the
  coordinator commits.
- A title run needs the single JIT slot; check it with the `comm` form above before launching one,
  and prefer the winecfg probe in every case where it answers the question.

## Reporting

- One report per iteration under `reports/phase4-hollow-knight/`, named for the question.
- Append one heartbeat line per step to `reports/research/LANE-HK-NSAPP-PROGRESS.md`.
- State the counts you measured, not the ones you expected.

## Termination

- `LOOP-STATUS: GOAL` — `app_sendEvent_enter` is non-zero and `KEY_PRESS` events reach the Wine
  queue in the winecfg probe, with the defect named and the fix in the tree.
- `LOOP-STATUS: BLOCKED` — you need something only the operator can decide. Name it in one
  sentence.
- Otherwise keep going.
