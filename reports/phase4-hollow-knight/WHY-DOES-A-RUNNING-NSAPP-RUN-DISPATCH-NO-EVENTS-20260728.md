# Why does a running `[NSApp run]` dispatch no events?

**Date:** 2026-07-28 · **Lane:** HK-NSAPP · **Build:** `winemac.so` `fd29b8ef…` (known-good, deployed)

## Verdict in one line

`[NSApp run]` is **healthy**. The main thread is correctly parked inside it waiting for
events, AppKit's event thread exists, and the nesting the brief flagged as suspicious is
fine. The measured defect is one level down: the Cocoa process runs at
**`NSApplicationActivationPolicyProhibited`**, and macOS delivers no key events to a
Prohibited-policy app and never lets it become active.

## What was measured (counts as observed, not as expected)

### 1. The defect reproduces on the 40-second probe

`wine winecfg` / `wine explorer /desktop=… winecfg`, `MACRUNNER_TRACE_WINEMAC_INPUT=1`:

| stage | count |
|---|---|
| `run_cocoa_app_entry` | 8 (across 8 Cocoa apps in the first prefix-creating run) |
| `run_cocoa_app_decide` | 8 — every one `created_app=1 success=1 already_entered=0 **will_run=1**` |
| `create_cocoa_window` | 8 |
| `nsapp_run_returned` | **0** |
| `app_sendEvent_enter` | **0** |
| `applicationDidBecomeActive` | **0** |

So `[NSApp run]` is entered and does not return — exactly as the brief states.

### 2. `sample` of the live process REFUTES the "nothing pumps AppKit" framing

Sampled the process that printed `will_run=1` (`explorer.exe`, 4 s, 920 samples). The main
thread, **920 / 920 samples**:

```
main (wine) → __wine_main (ntdll.so) → CFRunLoopRun → _CFRunLoopRunSpecificWithOptions
  → __CFRunLoopRun → __CFRunLoopDoSources0 → __CFRunLoopDoSource0
    → __CFRUNLOOP_IS_CALLING_OUT_TO_A_SOURCE0_PERFORM_FUNCTION__
      → run_cocoa_app (winemac.so)
        → -[NSApplication run]
          → -[NSApplication(NSEventRouting) nextEventMatchingMask:untilDate:inMode:dequeue:]
            → _DPSNextEvent → _DPSBlockUntilNextEventMatchingListInMode
              → _BlockUntilNextEventMatchingListInMode (HIToolbox)
                → ReceiveNextEventCommon → RunCurrentEventLoopInMode
                  → _CFRunLoopRunSpecificWithOptions → __CFRunLoopRun
                    → __CFRunLoopServiceMachPort → mach_msg (blocked)
```

Thread list contains **`com.apple.NSEventThread`** (thread 28871429), 11 threads total.

Two of the brief's leads die here:

- **Nested run loop.** `[NSApp run]` *is* entered from inside the `CFRunLoopSource` perform
  callback, and AppKit nests correctly: `_DPSNextEvent` is driving its own
  `RunCurrentEventLoopInMode` on top. The nesting is not the defect.
- **`nextEventMatchingMask` starvation by a competing drainer.** The app is not being
  starved by a rival consumer; it is blocked in `mach_msg` because **nothing is being
  delivered to it**.

The same sample also shows the run loop is live and serving Wine's own work
(`PerformRequest` → `__OnMainThread_block_invoke` → `macdrv_destroy_cocoa_window`), which
matches the 230 `onmainthread_enter`/`onmainthread_exit` pairs in the log. The main thread
is not stuck.

### 3. Root-cause class: the process is `Prohibited`

New instrument, `tools/ls_activation_probe.swift` (reads `NSRunningApplication` from
outside the process). Against the live Cocoa process:

```
pid=35879 policy=prohibited active=false finishedLaunching=true hidden=false
          terminated=false bundleID=(nil) name=explorer.exe exe=explorer.exe
```

Reproduced on three independent runs (pids 35879, 42244, 42931).

`NSApplicationActivationPolicyProhibited` means macOS does not consider the process an
activatable application: it cannot become active, and key events go to whichever app *is*
active. That single fact accounts for the entire signature — `app_sendEvent_enter = 0`,
`applicationDidBecomeActive = 0`, and the operator's zero `KEY_PRESS`(8) /
`KEY_RELEASE`(9) / `MOUSE_BUTTON`(12) on Hollow Knight — with `[NSApp run]` still perfectly
healthy.

The only code that lifts the policy is
`-[WineApplicationController transformProcessToForeground:]` (`cocoa_app.m:367`), which
calls `[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular]`. It is reached
from `-[WineWindow orderBelow:orAbove:activate:]` (`cocoa_window.m:1779`),
`-[WineWindow makeFocused:]` (`cocoa_window.m:2152`), and the display-mode path
(`cocoa_app.m:1054`). **Note that upstream discards `setActivationPolicy:`'s BOOL return
value**, so a failure there is currently invisible.

### 4. Second instrument: the probe's window never reaches the screen

`tools/cg_window_list_probe.swift` against the same live process:

```
onscreen-only: no windows for the requested pids
all-windows:   pid=42931 win=26001 layer=0 onscreen=false alpha=1.0
               owner="explorer.exe" bounds=0,482 500x500
```

Only one of the four `create_cocoa_window` calls reaches the WindowServer at all, and it is
`onscreen=false` — never ordered in.

## Honest limit of the winecfg probe — read this before trusting it further

The brief states winecfg "reproduces it exactly". It reproduces the **stage counts**, but
not necessarily the **cause**:

- In every probe run, the only process that loads `winemac.so` and creates Cocoa windows is
  **`explorer.exe`**. The guest app itself (`winecfg`, and `notepad` — a native ARM64 PE,
  so not a HyperBridge translation issue) never reaches `macdrv_init_entry` at all, and
  exits after ~10 s.
- No wine window in this prefix ever reaches the screen.

A process with **no on-screen window** is *expected* to sit at `prohibited` — nothing ever
ordered a window in, so `transformProcessToForeground:` had no reason to run. Hollow Knight
demonstrably **does** put an on-screen window up (it renders; 2346 × `CLIENT_SURFACE_PRESENTED`).

So the open question, and the only thing standing between this and a named root cause, is:

> On Hollow Knight — which *does* order in a visible window — is the policy also
> `prohibited`? If yes, `setActivationPolicy:` is being called and **failing**, and the fix
> is at that call. If it is `regular`, then HK's input failure has a different cause and
> the probe's `prohibited` is a probe artefact.

**That measurement was attempted three times this iteration and is still open** — see
"Title run: blocked below the window" below. `tools/hk_activation_watch.sh` is ready and
will answer it the moment an HK run reaches its own window.

## ★ `MACRUNNER_WINE_BIN` makes every `mr-run` launch use the **May 13** dist

Found while trying to run HK, and it affects **every lane**, not just this one.

- `config/env.sh:93` — `: "${MACRUNNER_WINE_BIN:=$MACRUNNER_ROOT/engine/wine/dist/bin/wine}"`
- `scripts/mr-run.sh:56` — `. "$ROOT/config/env.sh"`  ← mr-run sources it **itself**
- `scripts/mr-run.sh:65` — `WINE="${MACRUNNER_WINE_BIN:-$DIST/bin/wine}"`

Because env.sh uses `:=`, the variable is *always* set by the time line 65 reads it, so the
`:-$DIST/bin/wine` fallback **can never fire**. The `dist-arm64ec-spike` argument every HK
wrapper passes is silently ignored, and the run uses `engine/wine/dist` — a **13 May**
tree (`wine` `da9d3bb4…`, `winemac.so` `cedf92a6…`, vs the spike's `00145dab…` /
`fd29b8ef…`). Unsetting the variable in the caller does **not** help, because mr-run
re-sources env.sh.

Measured impact on back-to-back HK runs, identical otherwise:

| run | wine binary used | ladder rung | exit |
|---|---|---|---|
| `laneA-nsapp-policy2-try1-205731` | `engine/wine/dist/bin/wine` (May 13) | **1** (xtajit64-init) | 66 |
| `laneA-nsapp-policy3-try1-205852` | `engine/wine/dist-arm64ec-spike/bin/wine` | **8** (gfxdevice) | 1 |

Seven ladder rungs. Any lane currently reading `LADDER_REGRESSION: YES` should check
`final-child.json` → `argv.entries[0].value` before believing it — that field records the
binary actually executed. Workaround for a caller that needs a specific dist: **export**
`MACRUNNER_WINE_BIN` explicitly (this is what `tools/hk_activation_watch.sh` now does).
The real fix belongs in `config/env.sh` / `scripts/mr-run.sh`, which are outside this
lane's territory — flagging for the coordinator.

## Title run: blocked below the window

Three HK attempts this iteration, none reached HK's own window:

1. `laneA-run-hk.sh` bare → `run-contract-ledger status=BLOCKED exit before wine`. Its own
   header warns it is not a neutral wrapper (~1.2 % of runs reach level start); invoking it
   bare also leaves `MACRUNNER_MR_RUN_{ACTXPRXY,CRT_CASE_FUSION,WWISE_OBSERVER}` unset, and
   the run-contract ledger returns rc=2 (`branch_input_absent` ×3). **Use
   `scripts/hk-run-try12-config.sh`** — it replicates try12, the run that reached the main
   menu on 2026-07-28, and already sets `MACRUNNER_TRACE_WINEMAC_INPUT=1`.
2. try12 config, but the `MACRUNNER_WINE_BIN` trap above → rung 1, exit 66.
3. try12 config + correct binary → **rung 8 (gfxdevice)**, exit 1 at ~60 s.

In run 3 the only `create_cocoa_window` calls are hwnds `0x1003c / 0x10046 / 0x10048 /
0x1004a` — byte-for-byte the same shell/systray windows the winecfg probe produces, from
one pid. **HK never created its own game window**, so the activation policy of a rendering
HK process remains unmeasured.

Note what this means for the probe's `prohibited` reading: the four windows it sees are
wine's internal shell windows, which are legitimately never ordered in. So `prohibited` on
that process is consistent with normal macOS behaviour and is **not by itself proof of the
HK defect** — it is a strong lead that needs the title run to confirm.

## Build note — the trap in the brief is real, and it bit

`make -C engine/wine/build-arm64ec-spike/dlls/winemac.drv winemac.so` prints
`make: 'winemac.so' is up to date` and exits 0 while shipping the **old** binary
(SHA unchanged). The sub-Makefile carries no dependency information.

**Correct incremental build:** from the top of `build-arm64ec-spike`,
`make dlls/winemac.drv/winemac.so`. Verified: rebuilding the *unmodified* sources that way
reproduces the shipped binary **byte-identically** (`fd29b8ef…`), which proves the build
path is the right one.

## Open defect created and reverted this iteration — do not repeat blindly

Adding three `fprintf` traces to `transformProcessToForeground:` (logging
`setActivationPolicy:`'s return value) and to `orderBelow:orAbove:activate:` produced a
`winemac.so` (`ed124f0c…`) that **fails to load**: `winemac_so_load = 0`,
`macdrv_init_entry = 0`, the session dies just after `dllmain_attach`, 4 attempts out of 4.
Reverting to `fd29b8ef…` succeeded on attempt 1. The instrumentation is in the shipped
binary (`strings` confirms all three format strings), the library is ad-hoc signed and
links clean, and the mmap noise (`err:virtual:try_map_free_area`, 14–17 occurrences) is
present in the *working* runs too — so that is not the cause.

This is unexplained and is the first thing to bisect next iteration (add one trace at a
time). Until then, **do not ship instrumentation into `cocoa_app.m` /`cocoa_window.m`
without re-running the probe and confirming `winemac_so_load = 1`.**

## Instruments added

| file | purpose |
|---|---|
| `tools/ls_activation_probe.swift` | activation policy / active / LaunchServices record of a live pid, from outside |
| `tools/cg_window_list_probe.swift` | WindowServer window list for a pid — distinguishes "never ordered in" from "ordered in" |
| `tools/nsapp_probe.sh` | 40 s winecfg/notepad reproduction: launch + wait + probe + scoped kill in one process, with retries |
| `tools/hk_activation_watch.sh` | polls the above against a live Hollow Knight run |

Two gotchas worth keeping:

- **zsh does not word-split unquoted parameter expansions.** `ls_probe $PIDS` passes one
  argument containing all the pids and silently measures nothing. Use `${=PIDS}`. This cost
  two probe cycles that looked like empty results.
- The wine session only lives ~60 s and dies when the launching shell call returns, so
  launching in one tool call and probing in the next races teardown. Everything must happen
  inside one process.

## Refuted / closed this iteration

- ❌ "`[NSApp run]` returns and the main thread falls back into the loader's `CFRunLoopRun`."
  Already refuted by `nsapp_run_returned = 0`; now also refuted positively — `[NSApp run]`
  is *on the stack*, 920/920 samples.
- ❌ "Nothing pumps AppKit." AppKit is pumping; `_DPSNextEvent` is running its own nested
  event loop and `com.apple.NSEventThread` exists.
- ❌ "The `CFRunLoopSource`-callback nesting breaks AppKit dispatch." It does not.
- ❌ "A rival drainer starves `nextEventMatchingMask`." The app is blocked in `mach_msg`
  with nothing delivered, not losing a race.

## Next actions, in order

1. **Bisect the instrumentation load failure.** Re-add ONE trace to `cocoa_app.m`, rebuild
   from the top of `build-arm64ec-spike`, deploy, run `tools/nsapp_probe.sh` and confirm
   `winemac_so_load = 1` before adding the next. Until this is understood no winemac
   instrumentation can be trusted to ship.
2. **Get any wine window on screen.** No app window is created in the probe prefix at all —
   `winecfg` and `notepad` (native ARM64 PE) both exit ~10 s without reaching
   `macdrv_init_entry`. Until one does, `cg_list` cannot show an ordered-in window and the
   `prohibited` reading cannot be promoted from lead to root cause.
3. **Then the title run**: `tools/hk_activation_watch.sh <tag> 2400 2` (already carries the
   `MACRUNNER_WINE_BIN` workaround). The single number that settles the whole question is
   HK's `policy=` while it has a visible window.
4. If HK reads `policy=regular`, the `prohibited` lead is a probe artefact and the next
   suspect is why `-sendEvent:` is not reached despite an active app.
   If HK reads `policy=prohibited`, the fix is at `cocoa_app.m:400` — check
   `-[NSApplication setActivationPolicy:]`'s discarded BOOL return and find why it fails or
   why `transformProcessToForeground:` is never reached.
