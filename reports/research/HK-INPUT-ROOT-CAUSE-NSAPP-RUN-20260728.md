# HK input: root cause is `[NSApp run]` never entered — measured, not inferred

**Date:** 2026-07-28 · **Lane:** HK-INPUT · **Status:** root cause PROVEN; fix written + built; verification run armed.

---

## Verdict

Hollow Knight cannot receive keyboard or mouse input because **the process never enters
`-[NSApplication run]`**. AppKit receives the events from the WindowServer and queues them, but
nothing ever dequeues them, so `-sendEvent:` is never called and no event reaches
`macdrv_key_event` / `macdrv_mouse_button`. Input is *structurally impossible* in this
configuration — it is not a routing bug, a focus bug, or a window-hierarchy bug.

**This refutes the hypothesis the lane was opened on.** The `GA_ROOT(hwnd) == 0` early-return at
`engine/wine/dlls/winemac.drv/window.c:812` is **not** the blocker: events never travel far enough
to reach that line. (It may still matter once events flow — it is downstream, not the wall.)

---

## Evidence

Measured on the live run `laneA-INPUT-TEST-manual-language-FAST4of5-LONG-123136`, game pid 75428,
which had loaded the 12:12 `winemac.so` (confirmed via `lsof`; contains `run_cocoa_app_stock_recovery`,
i.e. it *is* the build carrying the 12:13 "stock-NSApp recovery" fix).

Each instrument was validated against a control with a known-in-advance result before being trusted.

### 1. Main-thread stack — 758/758 samples

```
sample 75428 1
```

| | main thread |
|---|---|
| **HK (target)** | `start → main (wine) → __wine_main (ntdll.so) → CFRunLoopRun → __CFRunLoopServiceMachPort → mach_msg` |
| **Safari (control)** | `start → SafariMain → NSApplicationMain → -[NSApplication run] → nextEventMatchingMask → _DPSNextEvent → …` |

`grep -c 'NSApplication run'` → **Safari 1, HK 0.** Zero AppKit frames on HK's main thread. It is
parked in the bare `CFRunLoopRun()` at `engine/wine/dlls/ntdll/unix/loader.c:3276` — it never became
the Cocoa app.

### 2. Accessibility window count

| process | AX windows | CG windows |
|---|---|---|
| **HK / wine (target)** | **0** | 1 (`wid=25150 "Hollow Knight" 1024x768 layer=0 alpha=1.0 onscreen=true`) |
| Safari (control) | 1 | — |
| Finder (control) | 1 | — |
| Telegram (control) | 1 | — |
| Claude (control) | 1 | — |

AppKit publishes AX windows only for windows a *running* NSApp manages. Owning a real on-screen
CoreGraphics window while publishing zero AX windows is the signature of an NSApp that is not
pumping. (`background only = false`, so the activation policy is Regular — not the problem.)

### 3. AppKit is alive but unserviced

`_NSEventThread` **is** running in AppKit and AppKit + CoreGraphics are loaded. The WindowServer
connection exists and events are being queued; `-[NSApplication run]`/`nextEventMatchingMask` never
dequeues them.

---

## Mechanism — narrowed by elimination

1. The window exists ⇒ `macdrv_init` (`macdrv_main.c:466`) returned success.
2. `macdrv_init` returns `STATUS_UNSUCCESSFUL` if `macdrv_start_cocoa_app` fails (`:487`) ⇒ it
   returned 0 ⇒ `ret = !startup_info.success` ⇒ **`success == TRUE`** ⇒ `run_cocoa_app` **did run**.
3. `[NSApp run]` was gated at `cocoa_main.m:239` by:

   ```c
   if (created_app && startup_info->success)
   ```

   `success` is TRUE and `[NSApp run]` is absent from every thread ⇒ **`created_app == FALSE`**.
4. `created_app` is FALSE only when NSApp was already non-nil **and** already
   `isKindOfClass:[WineApplication class]` — so both the `!NSApp` branch **and** the 12:13
   stock-recovery `else if` were skipped.

**The 12:13 fix addressed the wrong case.** It handles NSApp being a *stock* `NSApplication`; the
live process is not in that case. `[HYPOTHESIS]` NSApp was already a `WineApplication` when
`run_cocoa_app` ran, implying an earlier invocation or another creator — the
`stage=run_cocoa_app_entry nsapp=%p class=%s` trace settles this in one run.

---

## Fix (written, built, not yet deployed)

`engine/wine/dlls/winemac.drv/cocoa_main.m` — two defects:

**1. The guard.** What matters is not *who created* NSApp but whether this process still owes AppKit
an event loop:

```c
should_run_nsapp = startup_info->success && !macrunner_nsapp_run_entered;
...
if (should_run_nsapp) { macrunner_nsapp_run_entered = TRUE; [NSApp run]; }
```

The new static `macrunner_nsapp_run_entered` prevents a second invocation from starting a **nested**
run loop on top of a live one. All three cases now enter the loop: created-here, pre-existing stock,
pre-existing WineApplication.

**2. A use-after-return.** `startup_info` points into `macdrv_start_cocoa_app`'s **stack frame**, and
the unlock releases the thread that owns it — it can pop the frame before `run_cocoa_app` reads
`startup_info->success` at the old line 239. That read decides whether input works at all. The
decision is now captured **before** the unlock. This is a latent upstream race, independent of
defect 1, and would produce exactly this symptom intermittently.

A gated `stage=run_cocoa_app_decide` trace records `created_app / success / already_entered / will_run`.

Built clean: `engine/wine/build-arm64ec-spike/dlls/winemac.drv/winemac.so`
(`sha256 75938c99…`), **verified by content** — contains `run_cocoa_app_decide`; dist does not
(correctly not deployed under the live run, whose process has it mmap'd).

---

## Verification, armed

`reports/phase4-hollow-knight/run-INPUT-TEST-try8-NSAPPRUN.sh` = the proven-fast `FAST4of5-LONG`
config (reaches the language stage in ~7 min) **plus** `MACRUNNER_TRACE_WINEMAC_INPUT=1` — the
dedicated winemac gate, which ntdll does not read, so it cannot reproduce try4's 5.9 GB ntdll flood.
try7 already carried this gate but died after ~1 min; try6 had it and fired 8 stages, so the gate works.

A detached watcher waits for the slot, then deploys (atomic `mv`, adhoc re-sign, **content**-verified),
launches, and writes `NSAPPRUN-VERDICT.txt` into the run dir with:

- **A.** main-thread stack — expect `-[NSApplication run]` (was: bare `CFRunLoopRun`)
- **B.** AX window count — expect ≥ 1 (was: 0; controls report 1)
- **C.** the `run_cocoa_app_decide` line — *why* the guard chose what it chose

Its liveness detector was validated against the live run (reported 4 ≠ 0) and counts into a variable
with a bracketed pattern — never `ps | grep -q` under `pipefail`.

---

## Open items (not this lane's verdict)

- **★ The fix is in an untracked file.** `.gitignore:60` ignores all of `engine/`, so
  `cocoa_main.m` is not tracked — only `macdrv.h` and `window.c` were force-added. This is exactly
  the "load-bearing source lost as untracked code" failure mode. The lane forbids `git add`, so
  **the operator should force-add `engine/wine/dlls/winemac.drv/cocoa_main.m`** (and the other 7
  instrumented winemac sources) before anything prunes them.
- **Black window.** At 24:49 into the live run the on-screen window was still fully black
  (`nonblack=0`) while DXMT frame dumps existed and a prior dump shows the menu rendering
  (`evidence-20260728-menu-renders/verify30-frame600-MAIN-MENU.png`). Whether this is timing or a
  regression of yesterday's toplevel-fallback fix is **unresolved** and separate from input.
- Once events flow, re-check the `GA_ROOT == 0` early-returns at `window.c:812` / `:1344` — still
  plausible as a *second* wall, now testable for the first time.
