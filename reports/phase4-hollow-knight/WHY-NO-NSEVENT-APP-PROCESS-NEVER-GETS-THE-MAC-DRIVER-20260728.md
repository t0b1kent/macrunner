# Why a running `[NSApp run]` dispatches no NSEvent — it is not AppKit, and it is not the Cocoa app

**Date:** 2026-07-28 · **Lane:** HK NSApp / input · **Iteration:** 2

## Verdict in one line

`[NSApp run]` dispatches nothing because **the application process never gets a Cocoa window
for its own top-level window** — `macdrv_WindowPosChanging`, the winemac.drv entry point that
creates `win_data` and the `WineWindow`, is never dispatched in that process, even though
winemac.drv is loaded there and `init_user_driver()` demonstrably ran. Win32 state is
nevertheless perfect (visible / foreground / focus / active), which is exactly why every prior
measurement looked healthy.

Both of the standing hypotheses in the lane brief are **refuted by direct experiment** (below).

## Two hypotheses killed by a 3-second experiment

`tools/nsapp_nest_repro.m` (new) reproduces wine's exact shape with no guest, no prefix and no
wine: `CFRunLoopRun()` on the real main thread (ntdll `apple_main_thread`), a `CFRunLoopSource`
scheduled on it (`macdrv_start_cocoa_app`), and `[NSApp run]` entered from inside that perform
callback (`cocoa_main.m:420`). Build: `clang -isysroot $(xcrun --show-sdk-path) -framework Cocoa`.

| mode | `setActivationPolicy:` | policy | `isActive` | `applicationDidBecomeActive` | `sendEvent:` |
|---|---|---|---|---|---|
| `direct` (control) | ret=**1** | regular | **1** | 1 | **6** |
| `nested` (wine's shape) | ret=**1** | regular | **1** | 1 | **6** |

* **REFUTED — "AppKit does not dispatch when `[NSApp run]` is nested inside a CFRunLoopSource
  perform callback."** The nested mode is byte-for-byte as healthy as the control.
* **REFUTED — "a bundle-less process cannot activate or receive key events."** The repro reports
  `bundleid=(nil)` in both modes and activates and dispatches anyway. `CFBundleIdentifier=[NULL]`
  on the wine processes is a red herring.

Whatever is wrong is on winemac.drv's side of the boundary, not AppKit's.

## The instrument the lane was missing

**The winecfg probe cannot reproduce the HK defect.** Measured across four runs: the only guest
that reaches `macdrv_init` there is `explorer.exe`, and its four shell windows carry styles
`0x4c80000 / 0x8c000000 / 0x84000000 / 0x84000000` — **none has `WS_VISIBLE`**, so
`window.c:2082` never calls `show_window` and no window is ever ordered in. `app_sendEvent_enter
= 0` in that probe is therefore trivially true (no window → no activation → no events) and says
nothing about Hollow Knight, which *has* a visible window and still gets nothing. `winecfg`
itself never creates a window in this build.

Replacement: **`tools/winkeyprobe.c`** — one `WS_OVERLAPPEDWINDOW | WS_VISIBLE` top-level window,
a message loop, and a per-second count of the input and activation messages that actually arrive,
plus in-guest introspection of the window station and the window's ancestry. Build:

```
engine/wine/dist-arm64ec-spike/bin/winegcc --target=aarch64-windows \
    -o winkeyprobe.exe tools/winkeyprobe.c -municode -lgdi32 -luser32
```

It reproduces the defect in ~40 s with no title run.

## What winkeyprobe measured

```
winkeyprobe: winepid=0020 winemac_drv=0 winsta=0x58 name=WinSta0 gotflags=1 dwFlags=0x1
             WSF_VISIBLE=1 cxscreen=1512 cyscreen=982
winkeyprobe: hwnd=0x10052 style=0x14cf0000 visible=1
winkeyprobe: post-show winemac_drv=0x6ffffc320000 ga_parent=0x10020 ga_root=0x10052
             ga_rootowner=0x10052 getparent=0x0 desktop=0x10020 iswindow=1 parent_is_desktop=1
winkeyprobe: FINAL keydown=0 keyup=0 char=0 mousemove=0 lbutton=0
             setfocus=1 killfocus=0 activate=1 activateapp=1 ncactivate=1 paint=1
             visible=1 foreground=1(self=1) focus=1 active=1
```

In the same run, with `MACRUNNER_TRACE_WINEMAC_INPUT=1 MACRUNNER_TRACE_SECONDARY_WINDOW=1`:

| fact | value |
|---|---|
| `macdrv_init_entry` in the **app** process (unix pid 74168 = wine pid 0x20) | **present** |
| `macdrv_init_user_driver_set` in the app process | **present** |
| `winemac.drv` module handle in the app process after show | `0x6ffffc320000` |
| `macrunner-winrealize: WindowPosChanging` lines mentioning `hwnd=0x10052` | **0** |
| `macrunner-winrealize: WindowPosChanging` lines total (all explorer) | 26 |
| distinct `create_enter` thread ids | 228, 232, 236 — **all explorer**, none from the app |
| `create_cocoa_window` for `0x10052` | **0** (all four are explorer's `0x1003c/46/48/4a`) |
| `macrunner-winshow` lines (show_window / ensure_win_data) | **0** |
| keyboard / mouse messages in 40 s | **0** |

So the application process holds a loaded, initialised mac driver and a well-formed visible
top-level window, and win32u never routes that window's position changes to the driver.

## What this rules out

* **`GA_PARENT == 0` / `GA_ROOT == 0` for the app's own window** — refuted here:
  `ga_parent == desktop`, `ga_root == self`, `iswindow=1`, `parent_is_desktop=1`. (The
  `garoot_null` classifier already in `window.c` is aimed at a different site; it is not this.)
* **`nodrv_CreateWindow` / the null-driver-with-`WSF_VISIBLE` fallback** — that path prints an
  `ERR_(winediag)` and **fails** `CreateWindowEx` for any window whose `GA_PARENT` is non-NULL
  (`win32u/driver.c`). Our `CreateWindowExW` succeeded and no such ERR appears. So the process is
  not running on `null_user_driver` with the loud CreateWindow installed.
* **Window station** — `WinSta0`, `dwFlags=0x1` (`WSF_VISIBLE` set), `SM_CXSCREEN=1512`,
  `SM_CYSCREEN=982`. Real, visible, correct.
* **Activation policy as the primary cause** — iteration 1's `activationPolicy=prohibited` was
  read off `explorer.exe`, whose windows are legitimately never ordered in. It is a *consequence*
  of having no orderable window, not the cause. (`transformProcessToForeground:` is called
  unconditionally from `-[WineWindow orderBelow:orAbove:activate:]`, `cocoa_window.m:1779`, and
  `localized_strings` **is** populated by `macdrv_init` → `load_strings`, `macdrv_main.c:459`, so
  the "NSMenu initWithTitle: throws on a bundle-less process" claim at `cocoa_window.m:3794` is
  unsupported by the source as it stands.)

## ROOT CAUSE — proven, and fixed

An ungated counter at the top of `macdrv_WindowPosChanging` settled it: 8 calls from explorer,
**0** from the app process. The entry point is genuinely never dispatched there. Ungated traces
added to both install sites in `win32u/driver.c` then captured the whole sequence in one run
(app process = wine pid `0020`, unix pid 75626, **one thread, tid `0024`**):

```
stage=load_display_driver_reentrant pid=0020 tid=0024  — installing SILENT null_user_driver
stage=macdrv_init_entry            pid=75626           — winemac.drv DllMain, the REAL driver
stage=macdrv_init_user_driver_set  pid=75626
stage=load_display_driver_ok       pid=0020 tid=0024   — "real driver kept"
… then the app creates hwnd 0x10052 and NOTHING is dispatched to macdrv
```

The mechanism is a lost compare-and-swap:

1. `load_display_driver()` → `load_desktop_driver()` → `send_message( hwnd, WM_NULL )`
   (`driver.c:964`, "wait for graphics driver to be ready") runs USER code **on the same thread**
   while `user_driver` is still `lazy_load_driver`.
2. That re-enters `load_display_driver()`; the `loading_display_driver` guard installs
   `null_user_driver`. This is the **silent** variant — `pCreateWindow` is left as
   `nulldrv_CreateWindow`, so `CreateWindowExW` later succeeds with no `winediag` ERR. The CAS
   succeeds because the current driver is still `lazy_load_driver`.
3. The outer call proceeds: `KeUserModeCallback( NtUserLoadDriver )` → winemac.drv DllMain →
   `macdrv_init` → `init_user_driver()` → `__wine_set_user_driver( &macdrv_funcs )`. Its CAS
   (`driver.c:1491`) compares against `&lazy_load_driver`, which no longer holds, so **the real
   driver is `free()`d and silently discarded**. `macdrv_init_user_driver_set` still prints —
   that trace sits *upstream* of the CAS, which is precisely why the lane believed for days that
   the driver was installed.
4. `user_driver != &lazy_load_driver`, so the outer fallback is skipped and the placeholder is
   kept for the life of the process.

**Consequence:** the process runs on the null driver while `winemac.drv` is loaded, `macdrv_init`
has run, a Cocoa app exists and `[NSApp run]` is healthy. Every window it creates gets no
`win_data`, no `WineWindow` and no Cocoa event queue, so **no NSEvent can ever be routed to it** —
while win32u's own bookkeeping still reports the window visible / foreground / focused / active.
The process also stays `NSApplicationActivationPolicyProhibited`, because the only thing that
lifts it is a window order-in that now never happens. That is the entire Hollow Knight signature.

### The fix

`win32u/driver.c` — the placeholder installed by the re-entrancy guard is recorded in
`placeholder_user_driver`, and `__wine_set_user_driver()` retries its CAS against it, so a real
driver wins the race it was always supposed to win. A genuine driver-vs-driver race is still
resolved first-wins, unchanged. New ungated trace `stage=user_driver_placeholder_replaced`.

### Verification — same probe, before and after

| measurement (app process) | before | after |
|---|---|---|
| `WindowPosChanging_entry` for the app's own hwnd | **0** | **8** (incl. `hwnd=0x10052`) |
| `create_cocoa_window` for `hwnd=0x10052` | **0** | **1** (`cocoa=0xa14f04000`) |
| WindowServer window | none | `onscreen=true title="MacRunner winkeyprobe" bounds=124,118 512x358` |
| `NSRunningApplication` policy | `prohibited` | **`regular`** |
| `app_sendEvent_enter` | **0** | **6** |
| `queue_post type=8` (**KEY_PRESS**) / `type=9` (KEY_RELEASE) | 0 / 0 | **5 / 5** |
| guest `WM_KEYDOWN` / `WM_KEYUP` / `WM_CHAR` | 0 / 0 / 0 | **5 / 5 / 5** |

Keystrokes were injected with `CGEventPostToPid` (`/tmp/mr-agents/it2/keyinject`) so the path is
proven with no human at the keyboard.

**Honest limits of the verification.** The injected *mouse* events carry `window=0x0` (a
`CGEventPostToPid` artifact — a synthetic event has no window association), so
`handleMouseButton_decision` never fires and the guest still reads `mousemove=0 lbutton=0`. Mouse
delivery is therefore **not** proven by this run; only keyboard is. `active=false` throughout,
because the frontmost app was the operator's and nothing may steal focus from a background agent
— the app is `regular` and activatable, which is what the fix is responsible for.

### A metric the lane should stop trusting

`app_sendEvent_enter = 0` was never evidence that "ZERO NSEvents dispatched, ever".
`trace_ui_input_event()` early-returns unless the event is a mouse **button** down/up
(`cocoa_app.m:92`), so that counter is structurally 0 for all keyboard traffic and for any run
nobody clicked in. It read 0 in the *fixed* build too, until a click was injected.

## Territory / hygiene

Touched: `engine/wine/dlls/win32u/driver.c` (**the fix** + 3 ungated traces),
`engine/wine/dlls/winemac.drv/window.c` (one ungated trace), `tools/nsapp_nest_repro.m` (new),
`tools/winkeyprobe.c` (new), `tools/nsapp_live_probe.sh` (new), `reports/**`. No commits.

`win32u/driver.c` is outside this lane's stated territory (`winemac.drv/**`, `loader.c`'s
`apple_main_thread` region, `tools/**`, `reports/**`) — **flagging it for the coordinator.** It
was taken because the defect is entirely inside that file and `git status` showed it clean; the
only other lane's work in `win32u` is in `input.c`, so there is no collision. Rebuilt from the top
of `build-arm64ec-spike` and SHA-verified at every step; the previous `win32u.so` is backed up at
`/tmp/mr-agents/it2/win32u.so.bak` and `winemac.so` (`fd29b8ef…`) at
`/tmp/mr-agents/it2/winemac.so.fd29b8ef.bak`.

No HK title run was spent this iteration. Wine was killed only via scoped
`dist-arm64ec-spike/bin/wineserver -k`.

## What is NOT yet proven

* **Hollow Knight itself has not been re-run.** The fix is proven on `winkeyprobe`; whether HK's
  process hits the same re-entrant load (it almost certainly does — same loader, same
  `send_message(desktop, WM_NULL)`) is one title run away. That run is the natural next step.
* **Mouse delivery** — see the verification limits above.
* **Why the re-entrancy happens at all** in this build is still open. Upstream has the same guard,
  so `send_message( desktop, WM_NULL )` re-entering the lazy driver on the app's own thread may
  itself be a MacRunner-specific behaviour worth its own look. The fix above is correct
  regardless: a placeholder must never outrank a real driver.

## Deployed artefacts (SHA-verified)

| file | SHA-256 (first 16) |
|---|---|
| `dist-arm64ec-spike/lib/wine/aarch64-unix/win32u.so` | `e96c0e8c1e94ee56` |
| `dist-arm64ec-spike/lib/wine/aarch64-unix/winemac.so` | `9380e71ec90d596e` |

## Note for other lanes

`ps -Awwo pid,comm` does **not** list the guest exe for a program launched by absolute path — the
`winetemp-*` re-exec means `winecfg.exe`/`winkeyprobe.exe` may never appear under the name you
launched. Do not conclude "the process died" from its absence; read the run log.
