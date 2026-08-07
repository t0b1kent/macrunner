23:56 · BACKEND FAILOVER · `codex` retired after 2 consecutive returns under 180s with no journal line; continuing on `claude`.

## iter-1 (2026-07-28, claude)
- step: read mouse path. DECIDED STRUCTURE (from source, not measured):
  `handleMouseButton:` (cocoa_app.m:1665,1704) REQUIRES `[theEvent window]` isKindOfClass WineWindow
  -> CGEventPostToPid (window=0x0) can never work for buttons. `handleMouseMove:` (cocoa_app.m:1516)
  for NSEventTypeMouseMoved does NOT use the event window: it resolves via global
  `[NSWindow windowNumberAtPoint:belowWindowWithWindowNumber:0]` (line 1537). Two different gates.
- step: found pre-existing HID-tap injector tools/hk_cgevent_click.swift (posts .cghidEventTap,
  routes via WindowServer, carries a real window) but hardcoded to the "Hollow Knight" window and
  never validated against a known-good positive. Building an oracle first.
- step: BUILT + VALIDATED the injector. `tools/cocoa_mouse_oracle.swift` (native Cocoa app that
  mirrors WineApplication's sendEvent/handleMouseButton gates) + `tools/mouse_inject.swift`
  (selects window by pid/title, two modes) + `tools/mouse_oracle_run.sh`.
  KNOWN-GOOD POSITIVE, mode=hid, MEASURED: sendEventButton=6 sendEventMoved=9 viewDown=3 viewUp=3
  viewMoved=9 **btnWithWindow=6 btnNilWindow=0** lastBtnWindow=NSWindow num=26047, ax_trusted=true.
  CONTRAST, mode=topid (the keyboard lane's method), MEASURED: sendEventButton=6 sendEventMoved=9
  **btnWithWindow=0 btnNilWindow=6** lastBtnWindow=nil num=0, viewDown=0 viewUp=0.
  => CONFIRMS the brief: CGEventPostToPid delivers window=nil, so cocoa_app.m:1704 can never pass.
  HID-tap injection is the instrument that CAN drive the button path. Injector is now proven.
- step: SAFETY — this is the operator's LIVE desktop (AutoCAD w/ open drawing, Finder, Terminal).
  hid-tap posts land in whatever window is topmost at the point. Added a refuse-unless-target-is-
  topmost guard to mouse_inject (`blocked_not_topmost`, exit 3) before any further hid runs.
- step: guest run #1 (`tools/winmouseprobe.c` + `tools/winmouse_run.sh`, deployed win32u.so
  e96c0e8c1e94ee56 / winemac.so 9380e71ec90d596e = the post-fix SHAs). MEASURED:
  guest window exists (hwnd=0x30056 style=0x14cf0000 winrect=120,120,640,480 visible=1 fg=1
  focus=1 active=1), macdrv live (winemac_drv=0x6ffffc320000, create_cocoa_window=15,
  run_cocoa_app_entry=7, WindowPosChanging_entry=44), and GetCursorPos=380,678 — the exact point
  my oracle injection left the real cursor, so the driver DOES read CoreGraphics.
  BUT no wine window appeared in the WindowServer list in 40s (candidates=0 of 6 onscreen, all the
  operator's apps) => injection never fired; counters mousemove=0 lbdown=0 lbup=0 are NOT evidence
  of a mouse bug. Widened the dump to all layers/off-screen and re-running on the now-booted prefix.
- step: guest run #2/#3 both INVALID as measurements, and each named its own hazard:
  #2 the topmost-guard REFUSED the click (`blocked_not_topmost`, topmost_owner=Claude) — winemac.drv
  orders a non-WS_POPUP window in with `activate=0` (window.c:1017: activate = WS_POPUP ||
  recent activate_on_focus_time), so the probe window sits BELOW the operator's windows. That is
  normal wine behaviour, not a defect — HK is fullscreen WS_POPUP and would activate.
  Added `--raise` (NSRunningApplication.activate, reorders windows, delivers no input).
  #3 aimed at a STALE window: run#2's process tree outlived `wineserver -k` (still alive at 2:55),
  so CGWindowList still listed its window and the injector matched pid 35050 instead of 42627.
  Harness now does a pre-flight scoped teardown (verified PIDs matched on `comm` against the
  winetemp-/dist-arm64ec-spike paths, never pkill) and pins the target with `--pid $LAUNCH_PID`.
  Both runs' mouse stages are all-zero, but that is EXPECTED given no click landed — NOT evidence.
- step: probe window confirmed realized end to end (run.log): create_cocoa_window hwnd=0x10052/0x30056
  -> macrunner-winshow show-before-order -> show-after-order -> **show-after-on-screen on_screen=1**.
  So the Cocoa window exists and is on screen; the missing piece was never window creation.
- step: guest run #4 (clean: teardown verified, pid pinned) REFUSED again — and gave the reason.
  MEASURED via NSRunningApplication on the wine pid: `policy_before=NSApplicationActivationPolicy
  (rawValue: 0)` = **.regular** (so the 88894334 keyboard fix holds for this process), but
  `activate(options:.activateAllWindows)` left `active_after=false` and topmost still Claude —
  macOS will not let a non-active CLI activate another app. Raising is a dead end from here.
  REPLACED with: scan a grid over the target's content rect and inject only at a point where
  `topmostWindow(at:) == target`. Keeps the safety guarantee AND makes the click coordinate a
  known value to check the guest's client coords against.
- step: injector RE-VALIDATED against the oracle after that change (fast, 30s):
  aim_point_verified=(146.32,549.76); FINAL sendEventButton=6 sendEventMoved=9 viewDown=3 viewUp=3
  viewMoved=9 btnWithWindow=6 btnNilWindow=0, lastBtnLoc=(146.32,549.76) == the injected point.
  The instrument is known-good at the exact configuration now aimed at wine.
- step: runs #5/#6 REFUSED again and exposed a contradiction worth naming.
  MEASURED: at injection time the probe window is `.optionAll` index 0 (frontmost) with NO window
  in front of it overlapping — yet `.optionOnScreenOnly` iteration resolves **Claude** at the same
  point. The two CGWindowList orderings disagree about the wine window.
  Also MEASURED and FIXED: `--raise` was itself the saboteur — `NSRunningApplication.activate` fails
  on wine (active_after=false) but re-raises the genuinely-active app, after which topmost=Claude.
  Dropped --raise; the window is already ordered front without it.
  Replaced the list-order guard with an Accessibility test (`AXUIElementCopyElementAtPosition` ->
  pid), which answers the only question that matters — which element would receive this click.
  (`CGWindowListCreateImage`, the obvious pixel test, is obsoleted in macOS 15.)
  Added `in_onscreen_list` + `ax_pid_at_center` to settle the contradiction with data.
  AX guard RE-VALIDATED on the oracle: in_onscreen_list=true, ax_pid_at_center=88572 (oracle pid),
  aim_point_verified on the first probe, injection POSTED.
- step: run #7 SETTLED the contradiction with data. `in_onscreen_list=true`,
  `onscreen_order=["28023:Claude","90764:wine",...]`, `ax_pid_at_center=28023`.
  The on-screen list AND Accessibility AGREE: Claude really is in front. `.optionAll` order was the
  liar. Claude(141,98,1201x809) covers the probe(124,118,512x358) except a ~17px strip at x<141 —
  which every grid I had tried started to the right of. Replaced blind AX probing with a geometric
  exposed-region computation + ONE AX confirmation (also added AXUIElementSetMessagingTimeout=1s,
  since a wine process need not answer AX at all).
- step: **run #8 = GOAL.** Injected at CG (133.325,161.95), clearance 6.3px, ax_says_ours=true.
  GUEST MEASURED: `VERDICT mousemove=5 lbuttondown=3 lbuttonup=3`
  `lastmove=9,11(screen 133,161) lastdown=9,11(screen 133,161) lastup=9,11 cursor=133,161`
  — 5 moves posted (6th correctly skipped) -> 5 WM_MOUSEMOVE; 3 clicks -> 3 down + 3 up;
  and the guest's ClientToScreen(9,11) = (133,161) = THE INJECTED POINT.
  winemac path, exact-match stage counts: app_sendEvent_enter=6 controller_handleEvent=6
  handleMouseButton_enter=6 handleMouseButton_decision=6 **handleMouseButton_post=6
  handleMouseButton_no_post=0** macdrv_mouse_button=6 send_mouse_input=13 (+hardware_enter/exit=13).
- step: run #9 REPRODUCED at a DIFFERENT point CG (134.3,161.95): mousemove=5 lbuttondown=3
  lbuttonup=3, lastdown=10,11(screen 134,161). Two different pixels, both matched exactly => the
  coordinate agreement is not a one-point fluke.
- ANSWER to the brief's (a)-vs-(b): NEITHER. Real mouse events reach the process AND resolve
  correctly. The prior negative was the INJECTOR: cocoa_app.m:1704 requires [event window] to be a
  WineWindow, and CGEventPostToPid yields window=nil (oracle-measured btnNilWindow=6/6), so that
  gate is structurally unreachable by that method in a healthy build as much as a broken one.
- NO ENGINE SOURCE CHANGED. mouse.c / cocoa_app.m / cocoa_window.m untouched; the deployed build
  (win32u.so e96c0e8c1e94ee56, winemac.so 9380e71ec90d596e) already does this correctly.
- Report: reports/phase4-hollow-knight/HK-MOUSE-REACHES-THE-GUEST-THE-PRIOR-NEGATIVE-WAS-THE-INJECTOR-20260729.md
- NOT proven: Hollow Knight itself (deliberately not run — 45min vs 40s, another lane owns the slot);
  right button / scroll / drag / capture.
- Hygiene: no leftover wine procs (verified), void run dirs pruned, 35 GB free, no commits.

LOOP-STATUS: GOAL
