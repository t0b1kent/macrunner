# Run8 C1 window control: drawable is presented but the visible window has NO metal view

Date: 2026-07-27. Lane: HK Mono/JIT. Run: `laneA-c1-window-control-8-try1-084834`
(same build as run5/6, ntdll.so sha256 `b9be3226…`, winemetal.so `f44ad641…` == dist).
Classification: `VERIFIED_CONTROL_RESULT_NEW_COMPOSITOR_GAP_LOCALIZED`

(run7 `c1-window-control-7` try1–3 was `INVALID_ORCHESTRATION` — contract BLOCKED,
`branch_input_absent` ×3; no product conclusion; fixed in run8 by replicating run6's
three zero-branches.)

## The coordinator's three gates — all passed

1. **Instrument fired.** `+1084.181s macrunner-hb-present-surface: phase=c1-inject
   result=ok texture=0x8d4a7cf00 value=1,0,1,1`, then `phase=schedule result=ok
   ordinal=200 … texture=0x8d4a7cf00` (same texture pointer).
2. **Readback.** `+1084.203s phase=readback-complete checkpoint=presented-surface
   hash=0x730b0a687b7d0383 pixels=786432 black=0 nonblack=786432 colorful=786432
   min=255,0,255,255 max=255,0,255,255 mean=255.000,0.000,255.000,255.000`
   — the presented drawable's texture is FULL magenta. The control demonstrably
   writes the exact surface the readback samples.
3. **Capture tool liveness, from the lane's own lineage.** Full-screen
   `screencapture` rc=0; Finder window verdict PASS non_bg=350750; and the decisive
   tool control: **SCK capture of the OCCLUDED Safari window returns full content
   (nonblack=1,434,237)** — occlusion is not an excuse for a black window capture.

## The verdict — decision table row "magenta in readback, black in window"

- `screencapture -l <wid>` fails on the HK window ("could not create image from
  window") — that path is dead for this window; the run6 `-l` captures were void.
- SCK capture (`tools/cg_window_capture.swift`, works on occluded windows):
  HK window wid=20433 = **786432/786432 black**, while the presented drawable
  simultaneously carries the language-menu frame.
- Root localization via lldb on the live game (pid 84612, main thread in normal
  Cocoa run loop): `NSApp windows` = one `WineWindow 0x8485b4000`
  (`isVisible=YES`, 1024x768 at (0,33)); its `WineContentView 0x813478c00` has
  **ZERO subviews**. The `WineMetalView` (which `newMetalViewWithDevice` adds as a
  subview, `cocoa_window.m:707-720`) is NOT in the visible window's hierarchy.

**The presented drawable is composited into a view that is not attached to the
visible window.** Every D3D-side layer was already exonerated (RTV 210/210,
readback now shows real content); the gap is between the winemetal-bound
CAMetalLayer and the on-screen WineWindow. This is a narrow, local blocker —
and it retroactively voids every window-level "black screen" observation to date:
they were measuring a window that never had the game surface attached.

[HYPOTHESIS, ranked]
1. The swapchain's hwnd (`0x2002e`, create at +163.5s) is NOT the visible window's
   hwnd — Unity/win32u created a new window afterwards, or the swapchain targets
   a child/hidden hwnd whose Cocoa view is detached.
2. The Cocoa view bound at swapchain creation was later replaced/reparented
   (`willRemoveSubview` nils `_metalView`, `cocoa_window.m:838-839`).
3. The binding never hit this window's content view because `get_win_data(0x2002e)`
   resolved a different view from the start.
Next run must enable `WINEMETAL_TRACE_HWND` to capture the bound view pointer, then
diff it against the live content view.

## What the drawable actually contains — first real game UI through the pipeline

`MACRUNNER_DXMT_FRAME_DUMP=1` persisted raw 1024x768 RGBA per present schedule
(frames 1–5, then every 200th). Frame 200 rendered and inspected:

**It is the Hollow Knight LANGUAGE SELECT menu** (ENGLISH / FRANÇAIS / DEUTSCH /
ESPAÑOL / ITALIANO / PORTUGUÊS(BRASIL) / РУССКИЙ / 日本語 / 简体中文 / 繁體中文 /
한국어), grey text on black, `nonblack=11968, max=255, mean(nonblack)=99.2`.
The earlier `[HYPOTHESIS: Team Cherry logo]` is REFUTED. This is real, post-boot,
localized game UI — the managed side advanced far past the pre-fix black
backbuffer. Dumps: `reports/phase4-hollow-knight/frame-dumps-c1-control-20260727/`.

Frames 200/400/600/800/1000/1200/1800 are **byte-identical**
(sha256 `2c058750…`) — a static menu re-presented for 20+ minutes. Frames 1–5 are
all-black with distinct hashes (boot transition).

## Boot-flow position (corrected map)

- run8 Unity lines stop at +611s ("Couldn't find a UIManager" ×12), the same
  pre-`Performing automatic level start.` window as the 07-22 runs' line 1823→2000
  gap. **`Performing automatic level start.` and `Game controller set to None.`
  appear ZERO times in run6 and run8** — the current stall is BEFORE level start,
  not after it. (The 07-22 order was: level start → controller → language init.)
- The language picker is on screen because no saved language exists:
  `Loaded system language code 'EN'` / `Restored language code 'EN'`, never the
  oracle's `Loaded saved language code 'EN'`. The oracle template's save snapshot
  does not carry the language choice (no `language` key in its .reg files).
  [HYPOTHESIS] the picker itself does not gate level start (the 07-22 runs
  reached level start with the same picker flow), but it proves the template is
  not oracle-equivalent on first-run state.
- A `CGEvent.postToPid` Return keypress (36) to the game pid produced no log line
  and no frame change — that delivery path does not reach winemac's event queue.
  No global keystrokes were sent (user's desktop active; Telegram/Kimi frontmost).

## Adjacent product evidence

- SMC reverify live and consistent with run5/6: `progress tracked=58051
  reverified=33554432 evicted=2972 unreadable=0` at +728s; same deterministic
  eviction offsets. No evictions after the boot window — the residual stall is
  NOT stale translation (re-proven on this build).
- interp-lock fix (candidate 2) implemented this iteration: `exec_instr` wrapper
  in `hb_interpreter.c` fences SEQ_CST around every `is_locked` instr + bounded
  trace (`MACRUNNER_HB_TRACE_INTERP_LOCK=1`, armed line = positive liveness).
  Control `tools/hb_interp_lock_control.c` PASS in trace and notrace modes
  (armed=1, exactly 1 count line for `lock or [rax],7`, semantics 0x30|7=0x37,
  unlocked twin adds zero count lines). Suite: 27 fails byte-identical to a
  reverted-baseline run except ONE flaky slot that rotates
  (5325/5400/19286) without the fix too — no regression. Not yet in a live run.

## What run9 must carry

1. `WINEMETAL_TRACE_HWND=1` (capture the bound view pointer + `attached_to_hwnd`).
2. The interp-lock fix in ntdll.so (relink) + `MACRUNNER_HB_TRACE_INTERP_LOCK=1`
   — first live measurement of whether the spinning region executes locked RMWs.
3. Frame dump + ordinal-200 readback as run8.
4. Live view-hierarchy probe script (lldb: contentView subviews of the visible
   WineWindow vs the winemetal-bound view address).
5. Consider pre-seeding the language choice into the prefix (oracle-equivalent
   first-run state) — separate variable, do not mix into run9.
