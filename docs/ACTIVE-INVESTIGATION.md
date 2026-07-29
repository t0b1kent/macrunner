# ACTIVE INVESTIGATION — живое состояние

## Heartbeat 2026-07-28 12:22 — HK INPUT ROOT-CAUSED: NSApp = stock NSApplication → sendEvent/handleEvent dead → NO macOS input enters Wine; recovery fix + thief hook deployed, try7 queued (HK-INPUT lane)

- **ROOT DEFECT (proven live, lldb on try5 pid 15041):** в игровом процессе `NSApp` — обычный
  `NSApplication` (superclass NSResponder), delegate=nil, не отвечает на `setWineController:`.
  По коду `run_cocoa_app` (cocoa_main.m) это значит: controller не установлен, `[NSApp run]`
  не запущен → `WineApplication.sendEvent`/`handleEvent` не существуют → НИ ОДНО macOS событие
  (клавиатура И мышь) не входит в Wine. Это и есть корень мёртвого ввода у оператора.
  Окно при этом нормальное (WineWindow, level=0, ignoresMouseEvents=0, canBecomeKeyWindow=1),
  main thread в честном CFRunLoop.
- **Thief:** NSApp создаётся МЕЖДУ dlopen winemac.so и driver init (live на run35A pid 34444:
  .so загружена, run_cocoa_app ещё не бежал, NSApp уже stock). Кто именно — покажет hook
  `stage=first_sharedApplication` (caller class + stack) в try7.
- **FIX (deployed в dist-arm64ec-spike winemac.so, strings-verified):** recovery-ветка в
  `run_cocoa_app` — если NSApp не WineApplication: delegate=sharedController + swizzle
  `-sendEvent:` (recovery-impl через sharedController) + вход в `[NSApp run]`.
- **Wine-сторона очереди ЖИВА:** lldb `[win keyDown:]` → `postKey_posted KEY_PRESS` в run.log.
  НО событие НЕ извлекается consumer'ом — очередь копится (type=8/10 висят): поток-владелец
  очереди не пумпит. Это согласуется с JIT-spin блокером MONO lane (producer thread в
  exec_two_block_loop). **ВАЖНО для MONO lane: даже после починки входа ввода, игра его не
  прочитает, пока поток не выйдет из JIT-спина и не начнёт пумпить сообщения.**
- **Слот:** try7 (thief hook + fix + WINEMAC_INPUT trace) вооружён на watcher за run35A (~12:47).
  NOTE: мой thiefwatch по ошибке приаттачился к процессу run35A (34444) на ~11:57–12:05
  (несколько stop/continue циклов) — их A/B данные за этот период perturbed, извиняюсь.

## Heartbeat 2026-07-28 11:00 — HK-INPUT lane: TRACE_UI_INPUT = ntdll FIREHOSE (try4 killed at 5.9GB); winemac-only gate deployed, try5 armed (HK-INPUT lane)

- **Trap (now in AGENTS.md Known traps):** `MACRUNNER_TRACE_UI_INPUT` и `MACRUNNER_TRACE_UI_EVENT_PATH`
  также включают per-call flood в ntdll (`hb_run_guest_return` на каждый guest return,
  `NtWaitForMultipleObjects_enter/exit` на каждый wait). try4 написал 5.9 GB за 4 минуты и был
  убит мной (мой ран, TERM→KILL wrapper 76840, дерево чисто, диск 35 GB после disk-guard).
- **Fix:** dedicated gate `MACRUNNER_TRACE_WINEMAC_INPUT=1` добавлен во все 8 winemac
  `trace_ui_input_enabled()` helpers (event.c, cocoa_event.m, mouse.c, macdrv_main.c, cocoa_app.m,
  cocoa_window.m, keyboard.c, window.c); ntdll его не читает. Rebuilt + installed into
  `dist-arm64ec-spike/lib/wine/aarch64-unix/winemac.so`, content-verified (strings).
- **try5 armed:** `run-INPUT-TEST-try5.sh` = try3 env + `MACRUNNER_TRACE_WINEMAC_INPUT=1`
  (единственная переменная, NOLANG prefix). Watcher (10s poll) ждёт слот за wx-census-34
  (~11:35). Во избежание гонок: предыдущий try5-запуск в 10:55 попал на занятый слот и был
  убит мной за ~5 сек, до spawn wineserver — run34 (wx-census) не пострадал.
- **План try5:** boot → дождаться `stage=windowDidBecomeKey`/`applicationDidBecomeActive` или их
  отсутствия → меню → `tools/hk_cgevent_click` (HID click в центр окна) → при мёртвой клавиатуре
  `tools/hk_nsevent_inject.sh` (lldb, isActive/keyWindow + Return injection).

## Heartbeat 2026-07-28 08:58 — HK-INPUT lane: input-chain trace deployed, try4 armed for next free slot (HK-INPUT lane)

- **Deployed (byte-verified by content):** 12 new `macrunner-ui-input: stage=*` trace points
  across the keyboard/focus chain in winemac (`cocoa_window.m` keyDown/postKey/windowDidBecomeKey/
  windowDidResignKey/canBecomeKeyWindow_NO, `keyboard.c` macdrv_key_event/send_keyboard_input_sent,
  `window.c` set_focus GA_ROOT-null+give_cocoa_focus/window_got_focus_event,
  `cocoa_app.m` applicationDidBecome/ResignActive). Installed into
  `dist-arm64ec-spike/lib/wine/aarch64-unix/winemac.so` (adhoc re-signed; strings-verified).
  Mouse chain was already fully traced. Gate: `MACRUNNER_TRACE_UI_INPUT=1`.
- **Static finding (rules out a suspect):** clicks are NOT view-routed — `WineApplication
  sendEvent -> handleMouseButton` computes the WineWindow from the NSEvent and posts MOUSE_BUTTON
  with no app-active gating and no view hit-test dependency, so the parented Metal client view
  (toplevel fallback fix) cannot eat clicks. Mouse break, if any, is in delivery or below
  NtUserSendHardwareInput. Keyboard almost certainly needs the window to become key.
- **Slot claim:** `run-INPUT-TEST-try4.sh` (try3 env verbatim + `MACRUNNER_TRACE_UI_INPUT=1`,
  NOLANG prefix template) is armed on `try4-slotwatch.sh` (10s poll) behind the live
  FASTJIT-084929 run (timeout ~09:29). MONO run32 slotwatch is also queued; try4 is the direct
  continuation of the INPUT-TEST series this slot was for — after try4 (~15 min) the slot is free.
- **Input driving for try4:** `tools/hk_cgevent_click` (compiled Swift; HID-tap click at HK window
  centre, JSONL timestamps) + lldb in-process NSEvent injection as fallback for keyboard.

## Heartbeat 2026-07-28 08:15 — HK boot freeze root-caused: SIGUSR1 suspend handler self-deadlock on HB x64-ctx mutex; FIXED+DEPLOYED, run32 pending slot (HK-MONO lane)

- **CONFIRMED (live sample + instruction-level disasm of the deployed ntdll.so):** run31b froze
  after `<RI> Input initialized.`. Thread interrupted at `macrunner_hb_update_current_x64_context+0x138`
  HOLDING `macrunner_hb_x64_thread_context_mutex` (+0x138 disasm-proven inside trylock..unlock);
  its own `usr1_handler → NtGetContextThread → macrunner_hb_get_x64_thread_context` hard-locked the
  same non-recursive mutex = self-deadlock; `Loading.PreloadManager` deadlocked behind it.
  This is an env-independent second hang mechanism in the "alive but never advances" family;
  it is NOT the 2026-07-26 jit_code_hash spin. run30's quiet-park is a different stack, still open.
- **FIX (applied, not yet run-validated):** `macrunner_hb.c:321` mutex →
  `PTHREAD_RECURSIVE_MUTEX_INITIALIZER`. Recursive sig `0x32aaaba2` byte-verified in deployed
  `dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so` (SHA …9f616472 → …7ab1d576).
  Report: `reports/phase4-hollow-knight/RUN31B-RESULT-SUSPEND-HANDLER-SELF-DEADLOCK-ON-X64-CTX-MUTEX-20260728.md`.
- **NEXT:** run32 (script ready, run31b env verbatim, one variable = the fix) launches when the
  coordinator's `laneA-INPUT-TEST-manual-language` run frees the title slot. Mission unchanged:
  ring readout at the actuator fault (S2 [B] direct proof: OK-arm rcx, wrapper-post-call rax).

## Heartbeat 2026-07-27 21:00 — HK PlayerPrefs root cause PROVEN: HB registry import-semantic stub fakes W-registry calls (HK-REGISTRY lane)

- **Root cause (proven by 30s probe, not hypothesis):** `macrunner_hb_try_registry_semantic`
  (`engine/wine/dlls/ntdll/unix/macrunner_hb.c:30794`, chain entry `:32604`) short-circuits every
  advapi32 W-registry import from translated x64 guests: `RegOpenKeyExW`/`RegOpenKeyW`/`RegCreateKeyExW`
  return fabricated sequential handles (`0x6f00f0000000++`) with `ERROR_SUCCESS` and never touch the
  registry; `RegQueryValueExW` always returns `ERROR_FILE_NOT_FOUND`; `RegSetValueExW`/`RegDeleteValueW`/
  `RegCloseKey` fake success.
- **Game impact:** HK PlayerPrefs opens `HKCU\Software\Team Cherry\Hollow Knight` via `RegOpenKeyExW`
  (UnityPlayer.dll+0x7cb4d4: HKCU, KEY_READ, no WOW64 flags) and queries via `RegQueryValueExA` →
  every query on the fake handle fails (`ERROR_INVALID_HANDLE`) → `GetInt("GameLangSet")=0`,
  `HasKey("M2H_lastLanguage")=false` → language-select park. This is the confirmedLanguage gate's
  data-side root cause.
- **Wine registry stack exonerated:** probe (x64, `dist-arm64ec-spike`, seeded disposable prefix):
  `RegOpenKeyExA` → real handle, `GameLangSet_h1172976845`=**1**, `M2H_lastLanguage_h3859156181`=**"EN\0"**;
  direct `NtOpenKeyEx`/`NtQueryValueKey` → dword=1. WOW64/type/timing/path hypotheses all disproven.
- **Fix (engine lane territory, ready):** `reports/phase4-hollow-knight/REGISTRY-SEMANTIC-STUB-REMOVAL-20260727.patch`
  (49 deletions, `git apply --check` clean against the live tree). Evidence:
  `reports/phase4-hollow-knight/REGISTRY-READ-PATH-ROOT-CAUSE-20260727.md`. Post-fix verify:
  `./tools/hk_registry_probe/run-probe.sh A` → expect `RESULT: HK VALUES READ OK`.
- **Adjacent flagged risk:** `macrunner_hb_try_security_token_semantic` fakes `OpenProcessToken`/
  `OpenThreadToken` success — same class, separate audit.
- Lane status: `reports/research/LANE-HK-REGISTRY-PROGRESS.md` = `LOOP-STATUS: BLOCKED` (fix requires
  editing forbidden `macrunner_hb.c`; handed to engine lane/operator with ready patch).

## Heartbeat 2026-07-23 00:08 — foreground launch fixed; HK starts and reaches Present, but no post-marker Present

- **Launch fix:** canonical foreground `bash scripts/mr-run.sh "$DIST" "$EXE" 2400 > launch.stdout 2> launch.stderr; echo rc=$? > launch.rc` succeeded in starting Wine/HK. Evidence includes `final-child.json`, `run-contract.json`, `wine-child.pid`, `launch.stdout`, `launch.stderr`, and `launch.rc`.
- **Runtime result:** bounded run ended `rc=124`; product reached `CreateSwapChainForHwnd=2`, `GetBuffer=1`, `Present=96`, `Present1=48`, manager markers (`GameManager=1`, `UIManager=14`, `GameCameras=3`), `SetLanguage=4`, `ConfirmLanguage=4`, `HighlightDefault=2`, and `Performing automatic level start.=1`.
- **Boundary:** all Present/Present1 records occurred before automatic-level start; post-marker `Present=0`. `UNSUPPORTED=0`, `MEMORY_FAULT=0`, `reject=0`.
- **Capture note:** planned live LLDB hash-chain capture/mutation did not execute because the required marker was only known after the foreground runner completed; no second runtime was authorized.
- **Evidence:** `reports/phase4-hollow-knight/PIXEL-FIRST-FIX-LAUNCH-MONO-JIT-HASH-CYCLE-RESULT.md`.

## Heartbeat 2026-07-22 23:25 — HK Mono JIT hash-cycle causal run stopped pre-Wine UNKNOWN

- **Scope:** executed `docs/CODEX-TASK-hk-mono-jit-code-hash-cycle-causal.md` through Phase 0 and preflight only; no product source/build/install/cache/input/focus/shader changes.
- **Preflight:** synthetic chain fixtures PASS 5/5; staged dist diff was exactly one file (`mono-profiler-hk_language.dll` actuator), required native/managed SHA matched, collisions=0, env=116/116, predicted child `WINEDLLPATH` SHA `cce9f264...`.
- **Runtime boundary:** the single detached launch PID exited before `final-child.json`, `run-contract.json`, prefix creation, Wine, or Hollow Knight. `run.log` is empty and process residue is zero, so no LLDB attach, cycle traversal, mutation, post-cut Present, or pixel measurement occurred.
- **Verdict:** `UNKNOWN_PRE_WINE_LAUNCHER_LIFECYCLE / NOT_GOLDEN`. No retry and no commit; evidence is under `reports/phase4-hollow-knight/laneA-mono-jit-code-hash-cycle-causal-20260722-231158`.

## Heartbeat 2026-07-21 22:33 — profiler-OFF control exonerates Mono instrumentation

- **Exact control:** one 1300-second profiler-OFF runtime reused sealed ntdll `9a3f20b4...` / `3863660a...`, the exact prior DXMT overlay, full lookup paths, accepted save authority, and the same cache root. Final-child capture proves `MONO_ENV_OPTIONS`, language observer, and one-shot sequence absent; stock profiler PE `ca5a6e22...` was synced but not initialized.
- **Runtime:** `CreateSwapChainForHwnd rc=0` once; `GetBuffer=0`, `Present=0`, `Present1=0`, `UNSUPPORTED=0`, `MEMORY_FAULT=0`, reject=0 through nominal timeout rc 124. +1000/+1200 captures were identical BLACK 1024x768 frames with zero non-black/colorful pixels.
- **Verdict:** `PROFILER_EXONERATED / NOT_GOLDEN`. The current native state remains regressed relative to the Present-positive 2026-07-17 state; next work is a narrow native binary/source bisect. No retry, source change, production install, snapshot, commit, or GOLDEN.
- Evidence: `reports/phase4-hollow-knight/PIXEL-FIRST-FIRST-GETBUFFER-PROFILER-OFF-RESULT.md`.

## Heartbeat 2026-07-21 18:44 — Unity JIT MOVNTDQ corridor fixed; product runtime remains UNKNOWN

- **Exact root cause:** compile-only reproduction of the HK Unity corridor proved the first failing instruction was `UnityPlayer.dll+0xe1028d`, bytes `66 0f e7 14 07` (`MOVNTDQ m128,xmm`), not the first RIP-relative `MOVDQA` at `+0xe10256`.
- **Fix:** x64/x86 decoders now cover the legacy MOVNT packed-store family (`MOVNTPS`, `MOVNTPD`, `MOVNTDQ`) as guest-visible 128-bit stores; focused decode/JIT tests and exact Unity corridor compile test pass.
- **Floors:** focused `unity_movnt_store` PASS 4/4; real Unity `.pdata` function compile PASS with `block_fail=0 instr_fail=0`; phase1_core PASS 50/50; W03 static/source floors PASS. Broad default HB runner remains known non-green at 453/29 and is not claimed as full-green.
- **Sole runtime:** `laneA-unity-jit-corridor-try1-183420` was started once with fresh ntdlls, but deployment monitor stopped fail-closed before managed product replacement (`STOP final_child_present_before_deploy`). Scoped stop rc 0; no retry.
- **Verdict:** JIT corridor component PASS; product pixels and historical Return artifact causality remain UNKNOWN/NOT_GOLDEN. No commit, no GOLDEN, no production install.
- Evidence: `reports/phase4-hollow-knight/PIXEL-FIRST-UNITY-JIT-CORRIDOR-FAMILY-RESULT.md`.

## Heartbeat 2026-07-21 12:10 — W03 getenv/suspend boundary passed; profiler identity gate invalid

- **W03 repair/build:** bounded import/memory/cache env snapshot gates PASS; fresh current-source ntdll pair Unix `c185ef86...` / PE `9c91c7d6...`, build/product/dist identical, Unix codesign PASS.
- **Sole HK child:** passed the old `+301.5s` freeze boundary and continued through language restore at `+411.5s`; `+330s` sample has zero `wait_suspend`, `usr1_handler`, `server_select`, and `_os_unfair_lock` fanout.
- **Fail-closed result:** sealed profiler `b53cef81...` was atomically verified in prefix system32 before exec and child env was correct, but required `observer-init allocations=excluded` never appeared. Stopped exact child at `+456.3s`; **`INVALID_DEPLOYMENT`, NOT_GOLDEN, no retry**.
- Zero HighlightDefault, one-shot SetLanguage/ConfirmLanguage, manager/menu, Present, or capture-trigger records. Managed-language and historical Return-artifact causality remain `UNKNOWN`; no commit.
- Report: `reports/phase4-hollow-knight/PIXEL-FIRST-GETENV-SUSPEND-FIX-RESULT.md`.

## Heartbeat 2026-07-21 10:40 — deploy-fix proven, but run froze at +301s in wineserver suspend handshake (UNKNOWN)

- **Correction worked:** literal ordering `sync → atomic replace → verify → exec` held; `deploy-fix.json` proves sealed PE `b53cef81…` in per-run system32 (regular file, size 30720, SHA verified 23:23:51Z) before HK exec and ~40s before Mono init; dist stock `ca5a6e22…` unchanged after.
- **New stall (unrelated):** run passed the old +173.6s window, then froze at +301.5s (`ldr-init index=52`): guest thread stuck in `getenv → SIGUSR1 → usr1_handler → wait_suspend → server_select → read` (wineserver alive, handshake never completes); two identical samples 6s apart (`suspend-stall-sample.txt`). Predates any Mono/profiler load → not caused by the replacement; wineserver/HB suspend path, out of scope.
- **Outcome:** no profiler init, no identity gate, no invocations → **UNKNOWN, stop, no retry**.
- Report: `reports/phase4-hollow-knight/PIXEL-FIRST-MANAGED-SEQUENCE-NOALLOC-DEPLOY-FIX-RESULT.md`.

## Heartbeat 2026-07-21 10:05 — noalloc actuator never loaded: system32 stock dll shadowed the overlay variant (UNKNOWN)

- **Built:** no-allocation one-shot actuator (src `add408aa`, contract PASS, 2 deterministic builds IDENTICAL `b53cef81`); zero allocation callbacks/events; late `FindObjectsOfType` exactly-one lookup; thread-identity-first; ordered one-shot SetLanguage→ConfirmLanguage.
- **Run (`laneA-focus-input-try1-083601`):** clean boot **past the +173.6s freeze window** (no allocation profiling works — previous freeze trigger confirmed), `HighlightDefault` at +1013s, zero faults, VALID_RUN.
- **Failure mode:** loader used prefix `system32` stock `mono-profiler-hk_language.dll` (`ca5a6e22…`), not the overlay variant (`b53cef81…`) — init record lacks `allocations=excluded`; zero `oneshot-*` records; **no invocation → UNKNOWN, stop, no retry**.
- **Correction path recorded (for operator authorization):** replace/remove per-run prefix `system32/mono-profiler-hk_language.dll` with the variant before Mono init (~+52s).
- Report: `reports/phase4-hollow-knight/PIXEL-FIRST-MANAGED-SEQUENCE-NOALLOC-RESULT.md`.

## Heartbeat 2026-07-21 09:15 — managed sequence run froze at boot window before arming (UNKNOWN)

- **Built and proven:** one-shot SetLanguage→ConfirmLanguage actuator variant (source `89faad9c`, focused contract PASS, two deterministic builds IDENTICAL `325bf915`); scratch PE shadowed only the per-run overlay; dist untouched.
- **Run (`laneA-focus-input-try1-065625`):** profiler armed at +52.8s (language-flow, only `MACRUNNER_HB_LANGUAGE_ONESHOT_SEQUENCE=1`); run log stops at +173.6s during async-show window setup — the same deadlock window as 8 prior freezes today (main-queue async-show vs `_MetalLayer_setProps dispatch_sync`; observer/managed variants widen the race window; allocation profiling from init is the plausible widener here).
- **Outcome:** no arming, no invocations, no captures → **UNKNOWN, stop** (single authorized run, no retry per task).
- Report: `reports/phase4-hollow-knight/PIXEL-FIRST-MANAGED-LANGUAGE-SEQUENCE-RESULT.md`.

## Heartbeat 2026-07-21 08:55 — direct PostMessageW 6/6 accepted, still black → boundary is Unity input/state handling

- **B run:** one 1500s probe-off run (`laneA-focus-input-try1-042153`), zero faults; action at +1145s; frontmost verified throughout.
- **Helper** (`c1891c8b`, live prefix `_mr-run.ZOGq9B`, exit 0): target exactly one `0x2002e`/UnityWndClass; pre-state foreground==active==focus==HK required and held; **all six direct `PostMessageW(WM_KEYDOWN/WM_KEYUP)` accepted** (exact lParam: repeat 1, MapVirtualKeyW scan, extended for Right, bits 30/31 on up); no `WM_CHAR`/SYSKEY/fallbacks.
- **Outcome:** baseline/+10/+30/+60 valid per-window captures all BLACK; `Menu_Title=0`; faults=0.
- **Verdict:** direct Win32 window-message delivery is not sufficient. Chain fully closed (graphics → host → foreground → SendInput queue → direct window message). Next boundary is **inside the game process: Unity's input/state handling or language-flow readiness**.
- Report: `reports/phase4-hollow-knight/PIXEL-FIRST-DIRECT-WMKEY-CAUSAL-RESULT.md`.

## Heartbeat 2026-07-21 08:20 — Win32 foreground EXONERATED: pre-foreground already HK, B black → boundary is Unity message acceptance

- **B run:** one 1500s probe-off run (`laneA-focus-input-try1-034826`), zero faults; action at +1129s; macOS frontmost verified throughout.
- **Causal helper** (`62f65fec`, live prefix `_mr-run.OmLQR3`, exit 0): `GetForegroundWindow()==0x2002e` (Hollow Knight/UnityWndClass) **already before** `SetForegroundWindow`; `hwndActive==hwndFocus==0x2002e`; SFW returned true (redundant); SendInput 6/6 accepted after match.
- **Outcome:** baseline/+10/+30/+60 valid per-window captures all BLACK; `Menu_Title=0`; faults=0.
- **Verdict:** Win32 foreground exonerated. Chain closed: graphics (C0–C3), host ingress, SendInput acceptance, foreground state — all clean. Next boundary is Unity's own message acceptance in the running language UI. The window.c `WM_ACTIVATE/WM_SETFOCUS` hypothesis is refuted by measurement; no source fix applied.
- Report: `reports/phase4-hollow-knight/PIXEL-FIRST-WIN32-FOREGROUND-CAUSAL-RESULT.md`.

## Heartbeat 2026-07-20 23:20 — late-ready SendInput: 6/6 accepted, three valid HK captures black (determined direct negative)

- **Run:** one fresh 1500s probe-off run (`laneA-focus-input-try1-225145`), zero faults; action only at +1145s (≥+1050s); activation via System Events verified twice and held through all captures.
- **Helper:** exact bytes `c8e7197a`, one launch inside live prefix `_mr-run.jCR35X` with full loader env (`PREFIX_SYSTEM32_ARCH` incl.), exit 0 → all six `SendInput` records accepted; wineserver identity stable; `helper-launch.json` durable.
- **Captures:** predeclared atomic sequence; per-window valid HK PNGs at baseline/+10/+30/+60, frontmost verified before/after each; existing checker: **all BLACK** (`non_black_px=0`, `colorful_px=0`); visual inspection: only the HK window, no foreign pixels.
- **Verdict:** determined direct negative — Win32 `SendInput` acceptance is not sufficient to advance the language UI. Next boundary is downstream input acceptance (`macdrv → win32u → Unity`), not graphics, not generic Mono-init.
- Report: `reports/phase4-hollow-knight/PIXEL-FIRST-LATE-READY-SENDINPUT-RESULT.md`.

## Heartbeat 2026-07-20 15:20 — trusted input posted, Unity acceptance not proven (NOT_GOLDEN stop)

- **Run:** one 1500s probe-off production run (`laneA-focus-input-try1-144728`), zero faults; real 1024x768 HK window; frontmost verified stable; `READY_FOR_TRUSTED_INPUT pid=40466 window_id=3485` at +1211s.
- **Coordinator log (exact):** `WAITING 04:46:04Z` → `MARKER 05:08:11Z pid=40466 window_id=3485` → `TRUSTED_INPUT_SENT` → `DONE 05:08:18Z`. Trusted `Return,Right,Return` posted; run lived ~5 more minutes.
- **Outcome:** `Menu_Title=0`, frame fully black, zero fatal faults. Direct negative result, NOT_GOLDEN, stop. No retry, no instrumentation.
- **Careful classification:** post is proven; Unity acceptance is not. Failure localizes to the Wine/macdrv→win32u→Unity input-acceptance path (not host, not frontmost). Does NOT prove a generic Mono-init stall; scene/bootstrap incompleteness remains a separate prior finding.
- **Retracted:** host-wide synthetic-input-dead claim (Codex trusted TextEdit control disproved it).
- Report: `reports/phase4-hollow-knight/PIXEL-FIRST-DIRECT-INPUT-RESULT.md`.

## Heartbeat 2026-07-20 14:15 — direct input: host synthetic-keyboard delivery is dead; Wine dispatch intact

- **Run:** one 1500s probe-off production run (`laneA-focus-input-try1-133720`), READY/VALID_RUN, zero faults; real 1024x768 HK window; System Events frontmost held the game active; `Return,Right,Return` posted via `CGEventPost` — no reaction, `Menu_Title=0`, pixel BLACK.
- **Blocker (exact, native proof):** synthetic keys reach NO app — TextEdit shows 0 chars after `CGEventPost` (session+HID taps) and System Events keystroke; AppleEvent reads intermittently time out; `IOHIDCheckAccess=granted`, Secure Event Input OFF; activation works (System Events frontmost; NSRunningApplication accepted-but-ignored). 07-19 `CGEventPostToPid` no-hit = same host behavior.
- **Production fixes (winemac, deployed `02b4d94e`):** (1) OnMainThread self-deadlock → inline query on main; (2) async-show now makes eligible window key/frontmost (setActivationPolicy guard; transformProcessToForeground NSMenu-nil crash avoided); (3) win32u GUI queries main-gated. Focused tests PASS.
- **Out of scope (recorded):** observer-enabled runs freeze in HB SIGILL routing (`ldr_entry_from_pc` spin vs `_MetalLayer_setProps dispatch_sync`; 3 stacks saved). Physical keyboard is the only remaining ordinary input channel on this host.
- Report: `reports/phase4-hollow-knight/PIXEL-FIRST-DIRECT-INPUT-RESULT.md`.

## Heartbeat 2026-07-20 08:55 — exact-draw C0–C3 determined: original VS output production causal; graphics exonerated

- **Series:** four fresh serialized processes (`MACRUNNER_HB_CAUSAL_CONTROL=C0|C1|C2|C3`, 1200s each) on the exact `ps_bef43b20` draw via `laneA-run-hk.sh`; one-variable discipline verified by arm-pair env diff. All arms: exact 54-field signature checksum `0x07194c46f282d32d`, one claim, side-channel `655360/655360`, zero faults, Present>0, swapchain `rc=0x0`.
- **Outcomes:** C0=BLACK, C1=MAGENTA, C2=MAGENTA, C3=MAGENTA → predeclared verdict: original VS output production is causal for the black quad.
- **Boundary inspection:** the VS production is a bit-identical passthrough of vertex input `v1` (slot 0, stride 88, offset 24); 07-18 vertex-data proof shows Unity itself writes `(0,0,0,5/255)`. No DXMT/AIR defect → **no graphics fix authorized, no probe-off run** (that step is fix-conditional).
- **C2 try1 disclosure:** boot-stage freeze at +157s pre-swapchain (pre-causal, transient; wrapper timeout did not fire) → one documented manual re-attempt, clean.
- **Next boundary (out of scope here):** Unity scene/bootstrap incomplete (Game Manager/GameCameras/UIManager absent, zero scene loads) — managed-content lane.
- Report: `reports/phase4-hollow-knight/PIXEL-RUN-FIRST-C0-C3-RESULT.md` (DIAGNOSTIC_ONLY/NOT_GOLDEN). V5/admission and language/focus remain frozen.

## Heartbeat 2026-07-19 09:43 — real language input stops before Unity acceptance

- **A/A PASS:** two zero-input fresh processes have identical observer semantics and byte-identical BLACK captures; config/data/save are byte-identical and env differs only by permitted run paths. Fault/reject counters are zero.
- **Sealed B:** exact language UI and target HK window were proven. The first targeted Return down/up was posted to the HK PID, but `StartManager.SetLanguage` never executed. The harness correctly withheld Right/final Return, so three-event delivery and Unity acceptance are not proven. Strict verdict is `UNKNOWN` at host post→Unity EventSystem; do not blame StartManager.
- **Direct fallback (`NOT_GOLDEN`):** a separate default-off observer captured a pinned StartManager object, invoked `ConfirmLanguage` exactly once after `HighlightDefault`, and returned `ok`. No post-confirm `allowSceneActivation`, `Menu_Title`, managers, or visible pixel followed. Managed false→true remains unknown because Mono call-context introspection returns no values.
- **Next boundary:** instrument Wine/HB Win32 keyboard/message delivery through Unity legacy input/EventSystem acceptance. Direct confirmation is diagnostic only and is not a production fix.
- Report: `reports/phase4-hollow-knight/LANGUAGE-INPUT-AB-VERIFY.md`.

## Heartbeat 2026-07-18 20:28 — first-run callback is statically exact; one-event A/B is impossible

- **Exact callbacks:** `LanguageSelect` preselects `EnglishButton`; Submit/click calls `SetLanguage("EN")`. `LanguageConfirm` then fades in and explicitly preselects `CancelButton`; one horizontal navigation selects `ConfirmButton`; its Submit/click calls `ConfirmLanguage()`.
- **Minimum real input:** keyboard/controller requires three actions: Submit language, Horizontal to Confirm, Submit confirmation. A direct pointer path still needs two clicks because Confirm is initially inactive.
- **Observer ready:** default-off `mono-profiler-hk_language.dll` filters only the language callback, `confirmedLanguage`, `allowSceneActivation`, and three manager Awake boundaries. It has a capped record budget and no managed invocation/input API.
- **Build:** focused source test PASS; two deterministic builds and build/dist are byte-identical SHA `1913857dd4ce6f1c67d13d53f3458cadcfa0322da5d081c3136d8b5690f91376`.
- **Runtime status:** not run. The authorized one-event fresh A/B cannot reach ConfirmLanguage from the initial state, so runtime/pixel remain UNKNOWN. Next run requires explicit authorization for the exact three-action sequence as B's single sealed input variable.
- Report: `reports/phase4-hollow-knight/FIRST-RUN-LANGUAGE-INPUT-STATIC-BUILD.md`.

## Heartbeat 2026-07-18 17:34 — GameLangSet preseed is NO-HIT

- **Exact preseed reached the process:** Unity's DJB2-XOR oracle maps `GameLangSet` to `GameLangSet_h1172976845`; the fresh B template and live registry both contained `REG_DWORD=1`.
- **No scene effect:** the probe-free 1800s B run produced zero `Menu_Title`, `level1`, or `Switching Scenes` markers and retained the same Game Manager/GameCameras/UIManager missing state.
- **Pixel:** 254/254 valid `1024x768` captures are exactly BLACK; first/final PNG are byte-identical. Pixel gate rc=5; no snapshot was created.
- **Health:** valid timeout, wrapper accepted the run, and `c0000005/c000007b=0`. Build/dist remain byte-identical and signed.
- **Correction:** preseeding `GameLangSet=true` is not sufficient and is not causal under the authorized decision table. Do not implement a language-flow production fix from the prior hypothesis.
- **Next evidence:** identify the actual runtime condition holding `Menu_Title` activation after `GameLangSet=1`; do not revisit disproved graphics/shader/blend/vertex-upload paths.
- Report: `reports/phase4-hollow-knight/GAMELANGSET-PRESEED-VERIFY.md`.

## Heartbeat 2026-07-17 10:10 — HK GPU-truth probe built and deployed

- **Established boundary:** 3046 Draw calls execute and 1390 Present1 calls return `S_OK`; all CPU/signal fault gates are zero, but 147 window captures are exactly black.
- **Current diagnostic:** default-off `MACRUNNER_HB_GPU_READBACK_PROBE` reads the actual Metal render target after bounded clears/draw passes and the exact D3D11 backbuffer before presenter encoding. It uses completion handlers, not a GPU wait or continuous observer.
- **Draw evidence:** the first 32 actual calls record viewport/scissor, active RTV, PSO-associated VS/PS, vertex/fragment buffers, textures and raster state; samplers are explicitly identified as argument-buffer state.
- **Build/deploy:** ARM64 WineMetal plus x86_64 D3D11/WineMetal callers built; ABI index 132 exists in native and WOW64 tables, strict codesign passed. Signed Unix SHA `36924559…`, D3D11 SHA `df8a4863…`, PE thunk SHA `612696f2…`.
- **Next gate:** sealed 1800 s run. Nonblack pre-present isolates the native presenter; black pre-present plus draw-state establishes zero coverage/shader-state evidence and keeps missing GameCameras/scene lifecycle as the leading managed candidate.

## Heartbeat 2026-07-16 09:05 — HK 0x27fd is coordinator callback scheduling, not Present

- **`0x27fd` reached:** 244/275 readable ranges. Handler `Unity+0x11cc3b5` calls Gfx vtable `+0xa38`; runtime target is `Unity+0x6cda80`, tail wrapper to `Unity+0x478980`.
- **Not Present:** bounded trace observed 25 method returns and zero transitions into D3D11/DXGI. The method stores token `0x100000004`, performs QPC/timing and internal queue work, then schedules callback `Unity+0x4784d0` through `Unity+0x51b4b0` while setting coordinator pending `+0x18e8=1`.
- **Corrected graphics state:** real ClearRTV/ClearDSV execute, but draw/GetBuffer/RTV-create/OMSet/Present/Present1 probes remain zero. Clear on an off-screen RTV does not prove swapchain GetBuffer. Window is still strictly BLACK.
- **Next evidence:** event-trace callback enqueue and `Unity+0x4784d0` entry/exit on any thread, including pending `+0x18e8` clear. If enqueued but never executed, fix scheduler/wake; if executed but pending remains, fix callback completion. Do not force pending or Present.
- **Health:** main alive through 600 s timeout; fastfail/SIGILL/SIGBUS/c000007b/pc60 all zero.
- Report: `reports/phase4-hollow-knight/FRAME-FINALIZE-PROBE-RESULT.md`.

## Heartbeat 2026-07-16 04:53 — HK decoder healthy; wall moved to frame finalization/present

- **Readable range is decoded:** target bytes `a9 27 00 00` return as packed range `0xa844..0xa848`; `UnityPlayer+0x11bf031` selects jump-table case `0x27a9 -> +0x11cb4f7`.
- **Handler execution is real:** the next command `0x2713` reaches Gfx vtable `+0x58` and real D3D11 immediate-context `ClearRenderTargetView`/`ClearDepthStencilView`. Scheduler, wake, ring pop, refill, decoder, and handler selection are excluded as the black-frame wall.
- **`+0x180` is not dispatch:** NULL is valid for this optional refill wait-progress callback; `+0x118` is timing/QPC. Do not initialize or patch either field.
- **D3D11 exists:** IAT-only `D3D11CreateDevice` telemetry misses the creation path, but a real device/context and clear calls prove initialization succeeded. `GetBuffer/RTV-create/OMSet/Present/Present1` probes remain zero and the strict window capture is fully black.
- **Next evidence:** opcode `0x27fd` dominates 206/237 returned ranges and maps to Unity case `+0x11cc3b5`, Gfx vtable `+0xa38`. Trace that handler through frame finalization/submission/present; do not patch the proven-good decoder.
- Report: `reports/phase4-hollow-knight/READABLE-RANGE-DECODER-RESULT.md`.

## Heartbeat 2026-07-16 00:05 — HK wake/refill healthy; wall is downstream command handler selection

- **Ring boundary closed:** sealed 600s event-only run observed 209 enqueue/wakes and 209 `dequeue-ready` returns. Corrected provenance proves worker object `RDI=0x7100000c0` and `[object+0x60]=signal=0x3a0414c70` on 208/208 target cycles. The exact `Unity+0x9d3021` publication of `0x27a9` paired with sequence 18 and moved worker readable limit `+0x10c` to the new published limit.
- **Original pop premise disproved:** `+0x114` is the 32 MiB allocation-block base, not the per-command cursor; retaining `27aa/27a9/275d` in backing storage is expected. The `ThreadedBlockAllocatingBuffer` refill repeatedly returns to its caller; main is alive/changing through +590.918s.
- **Callbacks identified:** `+0x180=NULL`; `+0x118=Unity+0x9e0da0`, installed at `+0x11bee33`, calls `KERNEL32!QueryPerformanceCounter` and is thread timing/profiling, not render dispatch.
- **Remaining wall:** swapchain creation and factory association return 0, but real `GetBuffer/RTV/OMSet/Present/Present1` remain 0 and pixel gate is BLACK. Next evidence must trace the packed readable-range output into the command decoder/selected handler; do not patch scheduler/wait/callbacks. Report: `reports/phase4-hollow-knight/RING-POP-PROBE-RESULT.md`.

## Heartbeat 2026-07-15 23:00 — HK Gfx scheduler healthy; wall is payload pop/dispatch

- **Event-driven proof:** valid 600s run observed 240 main→`UnityGfxDeviceWorker` wake events and 240 matching `dequeue-ready` returns; 220/220 occur after factory `MakeWindowAssociation rc=0`. The extra 241st wait is the final outstanding sleep. No continuous ready-watch was used.
- **Concrete command:** Unity RVA `0x9d2ff0` publishes dword token `0x27a9` into the Gfx ring, updates `queue+0xc0`, and wakes `queue+0x60`; the worker returns through `UnityPlayer+0x62b199`. Therefore absent enqueue, missing wake, and failed semaphore acquisition are excluded.
- **Remaining boundary:** real swapchain `GetBuffer/RTV/OMSet/Present/Present1` remain 0. Semaphore-ready does not yet prove the exact ring payload was popped and dispatched. Next probe must capture publish/pop cursors and dynamic worker callbacks `+0x180/+0x118`; do not patch wake/wait.
- **Thunk mapped:** `0x6f0000003d80` is import slot 984, `KERNEL32!HeapAlloc`, called by Mono RVA `0x545bcb`; it is not an ARM64EC/JIT graphics mapping thunk.
- **Health/pixel:** main alive/running at +585.631s, all HB fault families 0; visible 1024x768 game window remains entirely black. Report: `reports/phase4-hollow-knight/GFX-THREAD-EVENT-PROBE-RESULT.md`.

## Heartbeat 2026-07-15 21:56 — HK post-u128 graphics wall is before first render command

- **Main is not stuck:** valid 600s baseline remains alive/running through `+585.731s`; guest PC moves from UnityPlayer to Mono and synthetic thunks while `guest_pc_changes` rises from 16.1M at `+100s` to 69.8M. Old Mono RVA `0x2791bc` is gone.
- **DXGI boundary:** `CreateSwapChainForHwnd rc=0` and factory `MakeWindowAssociation rc=0`; no later method is called on the typed swapchain. Real `GetBuffer/RTV/OMSet/Present/Present1` remain zero, so the wall is upstream of DXMT backbuffer use.
- **WINSHOW:** strict pixel gate sees an on-screen 1024x768 Hollow Knight window, fully black. Missing show alone is not causal; activation/focus remains unknown in the valid baseline.
- **Present-ready:** early diagnostic gate sees `byte138=0/state13c=0`, but it runs `~165s` before swapchain and is not proven connected. Continuous ready-watch is nontransparent: exact-cache diagnostics self-terminate `c0000005` before swapchain, so they are DIAGNOSTIC_ONLY.
- **Next evidence:** map synthetic thunk `0x6f0000003d80` (22/117 samples; native `hb_memory_protect` 21/117), then event-trace Unity Gfx-thread create/enqueue/wake/dequeue/first swapchain call. Do not force ready/focus or patch uncalled GetBuffer.
- Report: `reports/phase4-hollow-knight/POST-U128-FIX-GRAPHICS-GAP.md`.

## Heartbeat 2026-07-15 18:40 — HK store_u128 live W^X bypass fixed; graphics wall remains

- **Root fix applied:** `hb_jit_live_host_ptr()` no longer reuses cached live host pointers for write requests. The failed path was `hb_jit_helper_store_u128` using a stale RW cache entry after Mono flipped the live target page back to RX, allowing raw memcpy into executable code.
- **Regression covered:** `jit_x64_checked_xmm_store_exec_page` now primes the live-pointer cache while RW, flips the two-page mapping to RX, then performs an unaligned cross-page 128-bit store. Targeted runner passed 2/2.
- **Build/deploy verified:** rebuilt `libhyperbridge.a`, relinked `engine/wine/build-arm64ec-spike/dlls/ntdll/ntdll.so`, copied to dist, codesigned, and strict verify passed. Dist ntdll SHA after signing: `fcc7f76360a68a5b84063be66b5fdf5dc3e1101bb4c78c602a116d2d13f9b0ff`.
- **HK 600s verification:** main stays alive through timeout and keeps executing (`hb_jit_helper_exec_ir_block_once` final sample). Mono 0x2791bc, UDF0/native zero word, SIGILL, SIGBUS, fastfail/int29, import951/RaiseException, c000007b and pc=0 counters are all 0.
- **Remaining wall:** `CreateSwapChainForHwnd rc=0` and factory `MakeWindowAssociation rc=0` are reached, but real swapchain `GetBuffer/RTV/OMSet/Present/Present1` remain 0. Pixel gate captures the Hollow Knight window as black (`nonblack=0`). Current status is PARTIAL: CPU W^X corruption fixed; graphics path still stalls before backbuffer use.
- Report: `reports/phase4-hollow-knight/STORE-U128-W^X-FIX-VERIFY.md`; black capture: `reports/phase4-hollow-knight/store-u128-wx-fix-verify-20260715-1819/V/pixel/20260715T083000Z-89864/window-32526.png`.

## Heartbeat 2026-07-13 — ★ABZU ABBA ROOT CAUSE НАЙДЕН: две реализации CS на одном process-heap локе

- **ROOT CAUSE (доказан исходником + рантайм-ловушкой):** process-heap `RTL_CRITICAL_SECTION`
  мутируют ДВЕ независимые реализации. Wine-внутренний heap (`RtlAllocateHeap`) берёт лок через
  **ntdll** `RtlEnterCriticalSection` (`sync.c`); **гость** зовёт Win32 `EnterCriticalSection`, который
  HB перехватывает по имени (`macrunner_hb.c:5277`/`:19421`) и диспатчит в **собственную** копию
  `macrunner_hb_critical_section_enter/leave/try_enter` (`macrunner_hb.c:22815/22912/22892`, dispatch
  `:24164-24193`). Та пишет `OwningThread/RecursionCount/LockCount` **напрямую** (22882-83, 22850) и
  ждёт на **своём** семафоре (`NtWaitForSingleObject`, 22873), тогда как ntdll ждёт через
  `RtlWaitOnAddress`. Захват одной реализацией НЕВИДИМ другой → `RecursionCount` расходится → depth-1
  hold от гостевого `EnterCriticalSection` никогда не закрывается → heap CS залипает → ABBA против
  `loader_section`.
- **Как пойман:** self-triggering ловушка, tid 00a4, между двумя ПОСЛЕДОВАТЕЛЬНЫМИ вызовами ОДНОГО
  потока (no race): `seq35074 LEAVE_OK lock=-1 rec=0 own=0000` (свободна) → `seq35075 ENTER_REQ
  lock=0 rec=1 own=00a4` (уже захвачена, ДО нашего enter). `ENTER_REQ` снимает поля до мутации, оба
  ntdll-писателя (`RtlEnterCriticalSection` + `RtlTryEnterCriticalSection`) хукнуты — значит запись
  пришла ВНЕ ntdll = HB-путь. Значение `(0,1,00a4)` — байт-точный «успешный захват», не мусор.
- **195/195 строк `view=NATIVE`, ноль `view=EC`** ⇒ гипотеза «дуальная ARM64X .data / EC-vs-native»
  ОПРОВЕРГНУТА. Гость исполняет нативное aarch64-тело ntdll; фантомный захватчик — unix-side HB.
- **Все 5 гипотез убиты данными** (unmatched-ENTER / mixed-view / вторая x86_64-ntdll / ранний
  pre-latch ENTER / порча памяти). ⇒ **фикс-кандидат native-dispatch CreateThread НЕ применять** —
  лечит не тот класс.
- **Направление фикса (НЕ применено, нужно решение оператора):** правка в **Main** `macrunner_hb.c`.
  (A, предпочт.) HB не должен переопределять Enter/Leave/TryEnterCriticalSection — форвардить в
  реальный ntdll `RtlEnterCriticalSection` (одно тело + один wait-протокол на CS). (B, узко) исключить
  process-heap CS (и всё, что wine трогает внутренне) из нативного диспатча HB.
- **Гигиена:** `MacRunner-abzu` (чужой лайн) возвращён байт-в-байт после всех 8 сборок
  (`sync.c=6bc55767…`, build-dll+frozen dist `8e2a5691…`, `cmp` IDENTICAL). Свой abzu orphan-префикс
  снят (878 MB); живой HK-прогон и его Main-префикс не тронуты. Диск 57 GiB. `e7cca2e3`/golden целы.
- Отчёт: `reports/dualdata/ABZU-ABBA-HEAP-CS-OBSERVER-RESULT.md`;
  улики: `reports/research/checkpoints/abzu-abba-heap-cs-{observer,probe}-20260713{b,c,e,f,g,h,i,j,k,m,n}/`.

## Heartbeat 2026-07-13 — ABZU ABBA: crux ПЕРЕКВАЛИФИЦИРОВАН — поля CS расходятся с вызовами Enter/Leave

- **ABBA воспроизведён 5×**, цикл замкнут: main держит `heap.c: main process heap section` + ждёт
  `loader.c: loader_section`; worker держит `loader_section` + ждёт heap. Сторона worker'а полностью
  объяснена (HOLD-запись, acquire PC `0x7FFD07B5B98`, ledger 29/28).
- **★CRUX: «незакрытого ENTER» НЕ СУЩЕСТВУЕТ.** Запись безусловная с первой CS-операции, хукнуты ОБА
  писателя `OwningThread` (`RtlEnterCriticalSection` + `RtlTryEnterCriticalSection`): по heap CS
  `enters == leaves` (38006/38006, unmatched=0) и **HOLD-записи нет вообще**, при этом
  `OwningThread=<main>`, `LockCount=1`, `RecursionCount=1`. **Поля лока расходятся с фактической
  последовательностью вызовов.** Класс = порча/некогерентность состояния CS, НЕ lock-ordering.
- **Убиты данными 4 гипотезы:** (a) unmatched ENTER в CreateThread/`_initterm` — ledger сходится;
  (b) mixed EC/native view — `state=%p` одинаков у обоих потоков (дуальной ARM64X .data нет);
  вторая копия ntdll (x86_64) — не загружена (все модули `machine=0xaa64`, гостевой `HeapAlloc`
  диспатчится нативно в наш же CS); ранний pre-latch ENTER — запись безусловна, всё равно пусто.
  ⇒ **фикс-кандидат native-dispatch CreateThread (`macrunner_hb.c:21351`/`:26612`) НЕ применять** —
  он лечит класс, которого в уликах нет.
- **★2 ловушки, молча обнуляющие любой будущий зонд:** (1) **TID'ы гостя не стабильны между прогонами**
  (`0xa4/0xa8` → `0x9c/0xa0` → `0xa4/0xa8`) — зашитый TID-гейт даёт тихий ноль (именно это и убило
  инструментовку, лежавшую в дереве); (2) наблюдатель внутри реализации CS **не имеет права брать лок** —
  env-гейт через `RtlQueryEnvironmentVariable_U` берёт PEB-lock, а в ранней ldr-init `FastPebLock` ещё
  NULL ⇒ `RtlEnterCriticalSection(NULL)` ⇒ фолт на `crit+0x20` ⇒ рекурсивный шторм, убивавший
  regedit/regsvr32/services (99% CPU, игра не стартовала). Оба факта подтверждены дизассемблером
  (`ldr x8,[x20,#0x20]` сразу за нашим хуком).
- **Идёт зонд:** ring-buffer последних 64 heap-CS операций с полями `lock/recursion/owner` ДО и ПОСЛЕ
  каждой Enter/Leave + **per-TID** счётчики (глобальный ledger маскирует кросс-поточный перекос).
  Дискриминатор: `OwningThread` меняется без Enter ⇒ прямая порча памяти CS; неконсистентные
  `LockCount`/`RecursionCount` ⇒ гонка между потоками на одной CS.
- **Гигиена:** дерево `MacRunner-abzu` (чужой лайн) возвращено байт-в-байт после каждой из 6 сборок
  (`sync.c=6bc55767…`, build-dll и frozen dist `8e2a5691…`). PE-сборка НЕ байт-воспроизводима
  (таймстамп в заголовке) → build-артефакт восстанавливается копией эталона, не пересборкой.
  `e7cca2e3` и golden не тронуты.
- Отчёт: `reports/dualdata/ABZU-ABBA-HEAP-CS-OBSERVER-RESULT.md`;
  улики: `reports/research/checkpoints/abzu-abba-heap-cs-observer-20260713{b,c,e,f,g,h,i}/`.

## Heartbeat 2026-07-13 — ABZU ABBA observer attempt 2 (устарел: §3 кросс-древесный ntdll ОПРОВЕРГНУТ прогоном c)

- **ABBA доказан на рантайме (контрольный прогон, чистый frozen dist, `+sync`)** — реципрокная пара:
  `00a8` ждёт `heap.c: main process heap section` (7FFD02400D0), blocked by `00a4`; `00a4` ждёт
  `loader.c: loader_section` (7FFD08EB270), blocked by `00a8`. Цикл замкнут в обе стороны.
  **TID'ы `0xa4`/`0xa8` подтверждены живыми** → зашитый гейт наблюдателя (`sync.c:81`) валиден.
- **Crux НЕ взят:** observer выдал `0` событий → unmatched-ENTER и caller PC = UNKNOWN;
  классификация **(a) CreateThread-путь vs (b) mixed EC/native view — UNKNOWN, обе живы**.
  **Фикс-кандидат (native-dispatch CreateThread, `macrunner_hb.c:21351`/`:26612`) НЕ валидирован — не применять.**
- **★ЛОВУШКА ДЕПЛОЯ (доказана, класс «stale dist»):** PE `ntdll.dll` собирался в **Main**, а деплоился во
  **frozen ABZU overlay dist**, чья unix `ntdll.so` — из дерева **MacRunner-abzu**. Половины ABI-связаны
  (syscall table / unixlib / macrunner_hb structs) → игра умирала **до main** рекурсивным SEGV-штормом
  (`fault=0x20`, sp вниз), `exit=1`. Контроль без подмены: шторма `0`, main достигнут, `exit=143`.
  Один изменённый фактор ⇒ причина = кросс-древесный ntdll, не ABZU и не инструментовка.
  Попытка 1 (10:39) деплоила так же — просто не дожила до этого места.
- **Путь дальше (проверен, ждёт авторизации):** у `MacRunner-abzu` есть своё build-дерево, и его PE ntdll
  = `8e2a5691` — **байт-в-байт как во frozen dist**. Значит наблюдателя надо собирать ТАМ (один `sync.o` +
  релинк), а не в Main. Блокер: это worktree чужого лайна (терминал #2).
- **Харнесс починен** (оба бага попытки 1 мертвы): `set -euo pipefail` + `pgrep`-preflight вместо
  BSD-враждебных awk-регэкспов — **сработал**: живой HK-автолуп поймал, отказал `SERIALIZATION_BLOCKED=1`,
  **попытку не сжёг**; окно детекта старта (420s) разведено с окном наблюдения (90s).
  Рантайм после каждого прогона восстановлен байт-в-байт (`8e2a5691`).
- **Чистка:** снёс осиротевший `artifacts/_mr-run.FIw9W6` (880 MB) + 3× `/tmp/macrunner-*`.
  Диск **58 GiB** свободно (87%). Translation cache и build-деревья не тронуты, `make clean` не делал.
- **Флажок:** `engine/wine/dlls/ntdll/sync.c` **не отслеживается git** (`.gitignore:49` игнорит `engine/`,
  файл не был force-add'нут) — load-bearing правка наблюдателя живёт вне git.
- Отчёт: `reports/dualdata/ABZU-ABBA-HEAP-CS-OBSERVER-RESULT.md`;
  улики: `reports/research/checkpoints/abzu-abba-heap-cs-observer-20260713b/`.

## Heartbeat 2026-07-13 — HK EH real-RFLAGS full-env statistical verification

- 10:18 · preflight complete · HEAD `0b09f805`, deployed `ntdll.so` SHA `b8183a007bc3ce2e17cfe2257d36e271117618918c0ed0678bd2e16c17c5f87f`, no live Lane-A Wine/HK collision, Data volume has 58 GiB free · launching 600s full-env WITH `MACRUNNER_HB_EH_REAL_RFLAGS=1`, no build and no Patch C-H/EH-noop edits.
- 10:19 · full-env WITH launched · `eh-rflags-full`, timeout 600s, max tries 1, full performance env plus `WINEDEBUG=+seh`; auto-triage disabled to bound log processing · wait for SEH window and wrapper exit.
- 10:31 · full-env WITH mid-run · run valid through Mono/Unity, log 426 KiB/1988 lines, no runtime-fail/UNSUPPORTED, but no `guest-exception-delivery` or 714fd yet; event stream quiet after +61s while wrapper remains live · continue to bounded 600s exit.
- 10:37 · WITH-1 complete · rc=143 valid Mono/Unity run, last useful boundary `GfxDevice: creating device` +50.746s; delivery/714fd/e06d7363/present-gate/Present1 all 0, runtime-fail/UNSUPPORTED 0 · EH-ineligible, keep as evidence; log 426 KiB, no `/tmp/macrunner-*`, 57 GiB free; launch identical WITH-2 for A/A.
- 10:49 · WITH-2 complete / A/A passed · same 1990 lines and identical marker counts as WITH-1; D3D11 40.535s, Mono 48.640s, GfxDevice 54.369s, then same EH-ineligible wall; log 426 KiB, tmp/process cleanup clean, 58 GiB free · launch OFF-1 with only `EH_REAL_RFLAGS=0`.
- 11:00 · OFF-1 complete · same 1990-line rung-9 shape, GfxDevice +43.892s; EH delivery/714fd/e06d/present/failure counts all 0; log 426 KiB, tmp/process clean, 58 GiB free · OFF has no exposure either; launch WITH-3.
- 11:11 · WITH-3 complete / WITH group 3-of-3 · again 1990 lines, GfxDevice +46.745s, all EH/present/failure markers zero; every WITH run is EH-ineligible, logs 426 KiB, cleanup clean, 58 GiB free · launch OFF-2, then OFF-3 to complete requested matrix.
- 11:22 · OFF-2 complete · 1990 lines, GfxDevice +46.632s, EH/present/failure markers zero; 426 KiB, tmp/process clean, 58 GiB free · launch final OFF-3.
- 11:34 · EH RFLAGS 3x3 matrix complete · WITH 0/3 and WITHOUT 0/3 `guest-exception-delivery`; 714fd raw 0/3 vs 0/3 but conditional rate N/A, Context.Rsp/e06d/object+138/present gate unobserved; all runs wall after GfxDevice and have zero runtime-fail/UNSUPPORTED · verdict INCONCLUSIVE_EH_PATH_NOT_REACHED; no fix #3, logs 426-427 KiB, tmp/process clean, cache preserved, 58 GiB free; result `reports/phase4-hollow-knight/EH-RFLAGS-FULL-VERIFY-RESULT.md`.
- 15:33 · GfxDevice regression provenance · historical +142.277s swapchain run is `laneA-post-omset-scheduling-try1-054550` with cache/build key `ntdll-5bfc2b41e4b46e93`, not 3dbc107f; sequence Gfx +34.358s, D3D11 +34.665s, FMOD +35.824s, MonoManager +36.128s, swapchain +142.277s · compare true 5bfc→b818 delta; current disk 57GiB, no process/tmp cleanup needed.
- 15:37 · GfxDevice discriminator selected · current lacks Direct3D/FM0D/MonoManager completion and is silent after +53.4s; six warm-cache repeats exclude first-run cold cost, but b818 critical-section family is only a candidate until stack evidence · launch one 120s OFF no-build host-sample diagnostic, no Patch C-H mutation.
- 16:15 · first GfxDevice host sample invalid · `pgrep` selected wrapper bash PID 73263, not game Wine; no stall claim made · deleted entire invalid run dir and wrapper log, translation cache untouched; Data free space now 56GiB; repeat once with shell-excluding PID validation.

Last update: 2026-07-07, Opus (HK dist rebuilt+deployed with Patch H).

Читайте этот файл первым после compaction. Не перепроверять DISPROVED без нового
контр-факта. Не возвращать Wine-render патчи.

## Heartbeat 2026-07-08 — fix #3-B reference: wine x64-domain SEH dispatch chain (read-only)

Extracted the exact wine-arm64ec chain that dispatches SEH over the x64 AMD64_Context (dlls/ntdll/
signal_arm64ec.c): dispatch_exception (search loop) → virtual_unwind (RtlLookupFunctionEntry +
RtlVirtualUnwind on &AMD64_Context, fills DISPATCHER_CONTEXT_ARM64EC) → call_seh_handler (x64 SEH ABI
rcx=rec/rdx=frame/r8=context/r9=dispatch) → disposition switch; RtlIsEcCode(ControlPc) per-frame for
x64↔ARM64EC boundary; terminate at context.Sp==Tib.StackBase. AMD64_Context fields to map: Rip
(→ControlPc), Rsp (→Sp, boundary+termination — the field that's corrupt today), Rbp/Rbx/Rsi/Rdi/R12-15
+ Xmm6-15 (nonvol, unwound via x64 .pdata), EFlags/MxCsr passthrough. Fix #3-B = run that loop over our
already-built AMD64_Context (guest x64 RtlLookupFunctionEntry/RtlVirtualUnwind), bypassing the ARM64EC
.seh_context frame-shape mismatch entirely. Report: reports/WINE-DISPATCH-EXCEPTION-X64-REF.md
(phase4-hollow-knight symlink volume is read-only again → wrote to main-repo reports/ fallback).

## Heartbeat 2026-07-08 — fix #3 research: exception FRAME SHAPE is wrong (read-only)

Compared our macrunner_hb.c exception frame vs wine-arm64ec KiUserExceptionDispatcher (naked asm).
**machine_frame@0x590 is a RED HERRING** — the whole frame is the wrong SHAPE. We build the wine
PURE-X64 dispatcher layout (AMD64_CONTEXT@0, rec@0x4f0, machine_frame@0x590; rsp=frame_addr; rip=
dispatcher) and jump it into the ARM64EC dispatcher, whose entry contract is: `.seh_context` +
`sub sp,#0x4d0`, then rec@entry_sp+0x3b0, context(working)@entry_sp-0x4d0, arm_ctx@entry_sp (an
ARM64_NT_CONTEXT), prepare_exception_arm64ec→context_arm_to_x64+ResetToConsistentState. **NO machine
frame; unwind is .seh_context, not UWOP_PUSH_MACHFRAME.** Mismatches: (1) context TYPE — we put
AMD64_CONTEXT where it wants ARM64_NT_CONTEXT → x64 EFlags 0x202 read as SP; (2) rec offset 0x4f0 vs
0x3b0 (off 0x140, inside our context); (3) machine_frame@0x590 vestigial; (4) no arm_ctx/ResetTo-
ConsistentState. So fix #1 (real rflags) is necessary-but-NOT-sufficient (just changes 0x202→real-
eflags, still mis-read). Real fix: #3-A build the ARM64EC frame (needs ResetToConsistentState = fix #2)
OR #3-B dispatch SEH in the x64 domain (RtlDispatchException over AMD64_Context + RtlIsEcCode, like
wine dispatch_exception). Full: reports/phase4-hollow-knight/MACHINE-FRAME-FIX3-RESEARCH.md.
(NB archive volume flip-flops read-only; wrote report via cp to bypass the Write temp+rename EPERM.)

## Heartbeat 2026-07-08 — EH real-RFLAGS fix (#1) implemented; verify INCONCLUSIVE

Waited for the ABZU/native-direct lane to free the worktree (poller), then applied fix #1
(WINE-ARM64EC-MIRROR-COMPARISON.md): env-gated MACRUNNER_HB_EH_REAL_RFLAGS=1 block in
macrunner_hb_deliver_guest_exception_record — materializes REAL rflags (lazy+eager, mirrors
read_flags_image) into ctx->regs.x64.rflags before the context fill, so EFlags stops defaulting to
0x202. Built clean (2c6353f1), deployed. **The HK lane then re-edited macrunner_hb.c (09:50) + rebuilt
(09:51) → current dist ntdll=3dbc107f, which STILL contains my EH_REAL_RFLAGS edit (grep=1) — so my fix
is folded into the lane's live build.** Did NOT re-deploy 2c6353f1 (would drop their 09:50 work).
VERIFY inconclusive: run laneA-eh-rflags-fix-095003 walled at rung 9 (dxgi-factory) in a loader-init
loop BEFORE the EH delivery path — 0 guest-exception-deliveries, so the fix's code never ran; 714fd/
0x202 absent but that's NOT proof (it's INTERMITTENT, ~2/14 recent runs; EH path not reached). Lane's
eh-rflags-present runs (3dbc107f) also stalled early (0 deliveries). No pixel, runtime-fail=0, no
regression. Need: fuller env that reaches rung 11+/SEH path + WINEDEBUG=+seh + MULTI-run statistical
714fd rate WITH vs WITHOUT the gate. Caveat: if root is the machine_frame@0x590 UWOP offset (report
§A), real EFlags still mis-reads as Rsp → fix #3. Full: reports/phase4-hollow-knight/EH-RFLAGS-FIX-RESULT.md.

## Heartbeat 2026-07-08 — ARM64EC boundary vs wine-arm64ec + MS ABI (read-only compare)

Fetched wine mainline dlls/ntdll/signal_arm64ec.c (ARM64EC EH is upstreamed; supersedes AndreRH branch)
+ unix/signal_arm64.c + MS arm64ec-abi doc; compared to our signal_arm64.c + macrunner_hb.c.
- EH: wine's KiUserExceptionDispatcher is NAKED with `.seh_context`/`.seh_stackalloc 0x4d0` (frame +
  unwind info CO-GENERATED), uses emulator `pResetToConsistentState` for a coherent x64 CONTEXT, and
  runs its OWN RtlVirtualUnwind/RtlUnwindEx over AMD64_Context with per-frame RtlIsEcCode. We HAND-BUILD
  an exception frame (rec@0x4f0, machine_frame@0x590, sizeof 0x5c0), fill from ctx->regs.x64 with NO
  ResetToConsistentState, and jump to guest KiUserExceptionDispatcher. **Root of Context.Rsp=0x202:**
  our fill defaults EFlags→0x202 when rflags untracked (macrunner_hb.c:1649) AND the hand-laid
  machine_frame offset must match the real dispatcher's UWOP_PUSH_MACHFRAME .pdata bit-for-bit — skew →
  unwinder reads the eflags slot (0x202) as Rsp at ntdll+0x714fd. Wine structurally can't hit either.
- Callbacks: wine routes x64↔ARM64EC via __os_arm64x_check_call + entry/exit thunks + FFS (x64 CAN call
  ARM64EC builtins natively). We lack that layer and Phase-F REJECT native-PE targets — but our own
  comment (26600) shows the rejected targets look like CORRUPTED SEH handler addrs → downstream of the
  EH/unwind bug. Same root; fix EH (A) before callbacks (B).
- Fix candidates: (1) track real RFlags; (2) add ResetToConsistentState-equiv; (3) co-generate the
  dispatcher frame via .seh_context OR verify machine_frame@0x590 vs real .pdata; (4) own RtlVirtualUnwind
  +RtlIsEcCode; (5) build check_call/entry-exit-thunk/FFS transition layer. Full:
  reports/phase4-hollow-knight/WINE-ARM64EC-MIRROR-COMPARISON.md.

## Heartbeat 2026-07-07 — HK dist rebuilt + deployed with Patch H — DONE

HK dist ntdll.so was on Patch G (5d3110c0); source at HEAD 67e0e287 (Patch H). Rebuilt from source and
deployed so HK runs now use Patch H (XSAVE/XRSTOR/XSAVEOPT, RDRAND/RDSEED, x64 FXSAVE XMM8-15 fix).
- WAITED for the active HK run (laneA-run-hk.sh unity-probe 300) to finish first — polled to idle,
  re-checked immediately before the cp. HK idle throughout rebuild+deploy. No source touched, no commit.
- `make -C engine/hyperbridge` (ccache, clean) + `make ... dlls/ntdll/ntdll.so` (forced relink).
- Deployed: build ntdll.so → engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so.
- SHA: libhyperbridge.a=bf563e32; **dist ntdll.so now 37b849cae4461ebd… (== build), was 5d3110c0 (Patch G).**
  New ntdll SHA differs from the Patch-H-session build (643521e0) because the ntdll unix source
  (uncommitted HK-path edits in macrunner_hb.c/sync.c) advanced since — fresh build carries BOTH
  Patch H lib + latest ntdll source (correct for HK).
- SHA record: reports/PATCHH-DIST-REBUILD-SHA.txt (the intended reports/phase4-hollow-knight/ path is
  on the archive volume "/Volumes/MacOS 1" which is currently mounted READ-ONLY — existing PATCHH-*
  reports there stay intact/readable; only new writes are blocked).

## Heartbeat 2026-07-07 — HB UNSUPPORTED-opcode audit — DONE (read-only, no Patch I needed)

Audited all 77 `return HB_ERR_UNSUPPORTED_OPCODE` in hb_decode_x64.c (467 supported). Verdict:
**no unsupported opcode is on the HK/ABZU critical path.** Classification: A) x87 reserved-ModRM 16,
B) SSE/SSE2 legacy 6, C) 3-byte 0F38/0F3A 6, D) 1-byte group reserved-ext 16 — all DEFENSIVE guards
for architecturally-invalid encodings (correct to reject); E) AVX/AVX-512 VEX/EVEX 32 = the only
reachable gaps but AVX-512-only and **CPUID-gated OFF**; F) final catch-all 1. Probe-verified as
SUPPORTED (people assume missing): MOVBE, SHA1/256-NI, CRC32, MOVDIR64B, ROUNDxx, STMXCSR, MOV-Sreg.
Empirical: 10 newest HK runs + all ABZU runtime logs opcode-clean; 14/244 historical HK hits = 12×
INT3-padding (control-flow/thunk bug, not decoder; INT3 decodes, lift=-6→trap) + 2× a context-save
block that failed in patchE/F verify but now decodes fully. Current HK wall = PRESENT_MISSING
(graphics), triage classifier has no opcode class. **Patch I: DEFER** (AVX-512 = high-effort/zero-
payoff while gated). Report: reports/phase4-hollow-knight/HB-UNSUPPORTED-OPCODE-AUDIT.md. No source
touched; Patch C/D/E/F/G/H untouched; no game runs.

## Heartbeat 2026-07-07 — Patch H (HB extended-state / flags-stack family) — DONE + COMMITTED 67e0e287, VERDICT PASS

Update 2026-07-07 (later): first pass applied the edits but left them UNCOMMITTED in the
main worktree → looked like "drift" from any other worktree/checkout. Re-verified in source
AND committed: `67e0e287` on main (parent 4780f1dd), 5 source files, `+115/-4`, builds clean,
smoke re-run PASS. (Operator's git-add list was corrected: it named nonexistent hb_instr.h
and omitted hb_lift_x64.c + hb_decoder.h; committed the actual 5 Patch-H files.)

Isolated opcode-coverage worktree (NO game runs). Closed the rest of the extended-state
family in the HB x64 decoder so it stops unblocking one opcode at a time (F/G already did
pushf/popf + FXSAVE/FXRSTOR at HEAD 4780f1d):
- `0F AE /4,/5,/6` XSAVE/XRSTOR/XSAVEOPT → reuse FXSAVE/FXRSTOR IR (FXSAVE-equiv w/o AVX
  modeling); `66 0F AE /6` CLWB → NOP.
- `0F C7 /6,/7` RDRAND/RDSEED (reg form) → new `HB_INS_/HB_IR_RDRAND/RDSEED`; interp fills
  dst via arc4random, CF=1 + others cleared; codegen uses the existing interp `default:`
  fallback (no ARM64 emitter needed). CMPXCHG8B/16B `/1` mem form regression-safe.
- FXSAVE64 (`REX.W`/`66 0F AE /0`) already decoded — confirmed, no change.
- **Step-4 bug found + fixed:** `x87_fxsave_mem`/`x87_fxrstor_mem` used `regs.x86.xmm[i]`
  (wrong union member in x64 mode) and only 8/16 XMM regs → x64 FXSAVE/XSAVE saved wrong
  bytes + dropped XMM8-15, FXRSTOR restored nothing. Now mode-aware (8 regs 32-bit via x86
  view, 16 regs 64-bit via x64 view). Behavioral delta on the shipping x64 FXSAVE path —
  flagged for operator review.

Verify: `libhyperbridge.a` + `ntdll.so` clean under `-Werror`; 15-vector decode/lift matrix
all HB_OK; `rdrand_exec_test` + `fxsave_exec_test` (x64 XMM0+XMM8 round-trip) PASS. Details:
`reports/phase4-hollow-knight/PATCHH-{VERIFY-RUN,SMOKE-TEST,BUILD-SHA}`. Final ntdll.so
SHA `643521e0c93a8d12720c07f602bcdc8c66dd95cac0e13745bf31ccc28637ee56`.
Did NOT touch C/D/E/F/G; did NOT wipe translation cache / build/ / lib (incremental only).

## Current Task

Update 2026-07-02 22:47 VLAT: Swapchain-creator thread named. Diagnostic
`ntdll.so` is `122ec18648af98e6e5826dc6ee8f38b96991d205542178647cc573e4cf6c7279`
(trace-only rebuild; reused populated cache root
`artifacts/hb-translation-cache/ntdll-b8b7d4978dd1bf70-jitstorefence1-waithandle`).
Run `reports/phase4-hollow-knight/laneA-swapchain-creator-wait-v2-20260702-222503-try1-222503`
filtered as `PRESENT_MISSING` with `SELF_CHECK=PASS`; raw D3D evidence includes
`CreateSwapChainForHwnd rc=0`, `swapchain=2`, `present=0` even though the ladder
line still prints rung 9, so treat that rung line as a classifier ladder bug for
this run, not a raw regression. Live sample run
`reports/phase4-hollow-knight/laneA-swapchain-creator-wait-v3-20260702-223734-try1-223734`
captures swapchain creator guest `tid=0x64`, native `0x2bfe358`
(`Thread_46130008`). Its one post-swapchain wait on semaphore `0x1a4`
(`CreateSemaphoreExW initial=0 max=2147483647`) returns `wait_status=0`;
the creator is not parked there. Pinned sample
`sample-postswap-creator-pinned-224340.txt` shows creator CPU-active in
`macrunner_hb_call_import_thunk -> macrunner_hb_try_kernel32_handle_semantic ->
macrunner_hb_sync_virtual_region -> hb_memory_protect` with no
`NtWaitForSingleObject`, no `GetMessage`, and no `WaitMessage` on that thread.
Post-swapchain user32 message/visibility trace count is 0; on-demand realization
still logs visible Win32 rects for `hwnd=0x20058`. Current wall is HB
protect/sync churn on the producer thread, not DXMT frame latency, not worker
semaphores, and not a message-pump activation wait.

Update 2026-07-02 22:09 VLAT: HK rung-11 post-swapchain wait is no longer
explained by the parked worker semaphores. Next diagnostic target is the exact
thread that called `CreateSwapChainForHwnd`: record its thread id at swapchain
creation, then trace whether that same thread parks in `WaitForSingleObject*`,
enters the message pump (`GetMessage`/`PeekMessage`/`WaitMessage`), or receives
window activation/visibility messages after `MakeWindowAssociation`. Diagnostic
cache rule: trace-only `ntdll` rebuilds that do not touch HyperBridge codegen or
guest translation semantics may reuse the previous populated
`MACRUNNER_HB_TRANSLATION_CACHE_ROOT`; any codegen/translation-semantics change
must use a fresh root keyed by engine/codegen flags.

Update 2026-07-02 18:55 VLAT: Hollow Knight remains filtered rung 11 after
ABZU FP+COM import fixes on `main` (`18499f3`) and deployed `ntdll.so`
`2cc696d858cbb25c4441aff7c7d599a83cd911a600fc278e70ca9f5ea6cc5b73`.
Snapshot:
`artifacts/milestone-dist/hk-rung11-abzu-fp-com-merge-20260702-182214`
(forced `--dist engine/wine/dist-arm64ec-spike`). Post-merge populate run
`reports/phase4-hollow-knight/laneA-postmerge-fp-com-populate-20260702-182357-try1-182459`
reaches real `CreateSwapChainForHwnd rc=0`, then `MakeWindowAssociation`
returns `rc=0`; no `GetBuffer`, RTV, or real Present before timeout. Follow-up
warm runs showed variance below the frontier: one timed out at rung 9 and a
long monitored run was CPU-active in
`load_display_driver -> KeUserModeCallback -> macrunner_hb_route_x64_callback_fault
-> macrunner_hb_pc_in_executable_section` while helper threads waited. Mono
metadata/string fusions are now gated to actual Mono modules via
`HB_CONTEXT_CODEGEN_MONO_MODULE`; deployed `ntdll.so` after the gate is
`7638362cb8fc593269ca4f46eb7566cf47e1035703f94eae8d276bf9e12bedc7`.

Update 2026-07-02 09:35 VLAT: Hollow Knight/DXMT frontier is now honest
filtered rung 9 (`dxgi-factory`). Critical build regression after `3cc5f4a`
was fixed by tracking `ntdll/unix/unix_private.h` in `0f79be8`; clean rebuild of
the ntdll pair passes. Fast DX11 smoke exposed a kernelbase locale registry
static issue (`RegSetKeyValueW+0x8c`, `ldrh w8,[x23]`, bad `entry_sintlsymbol`
subkey), fixed by `3babcf8` and verified by direct DXMT smoke `rc=0` with
`CreateDXGIFactory1`, `D3D11CreateDevice`, `Present`, and `pixel_readback=PASS`.
HK run `reports/phase4-hollow-knight/laneA-dxgi-after-kernelbase-try1-092308`
uses drift-verified graphics-prep DXMT hashes and reaches real
`CreateDXGIFactory2` (`macrunner-hb-d3d-boundary ... rc=0`, factory object
`0x10c4ce730`). There is no signal/fault wall. It times out at `rc=143` after a
DXMT device-side `CreateFence` marker; filtered classify self-check now PASSes
with raw counts `dxgi_iat=7 real_factory=2 d3d11_iat=1 d3d11_device_markers=1
swapchain=0 present=0`. Next blocker is real `PRESENT_MISSING`: no
`CreateSwapChain*`, no `macrunner-hb-dxgi-swapchain`, no `Present`.

Update 2026-07-02 08:40 VLAT: Hollow Knight loader/frontier moved past Mono
with HB exec-registration restored in `ntdll/unix/virtual.c`. The suspicious
`WINDOW_SERVER_ERROR c000007b` at `mono-2.0-bdwgc.dll+0x385e73` was a derived
classifier false positive: raw was HB `STATUS_INVALID_IMAGE_FORMAT` used for a
runtime `MEMORY_FAULT`, not a window-server status. Mainline run
`reports/phase4-hollow-knight/laneA-mono-standard-env-try1-082507` reaches
filtered rung 8 (`gfxdevice`) and records two born-exec allocation registrations.
Forced diagnostic `MACRUNNER_HB_JIT_DIRECT_MEM=1` still reproduces a separate
direct-mem JIT fault at Mono `cmpw %r8w,(%rbx)` with
`rbx=0xffffffff01000166`; keep direct-mem disabled for the HK gate until that
class is audited separately. Next blocker is Lane C graphics/PRESENT_MISSING.

Update 2026-05-28 09:46 VLAT: evidence-first PC=0/null-PC classification
completed. The old status-fix follow-up was not a real `PC=0`: fresh child lldb
capture in `reports/phase-h/npp-x86-nullpc-vmmap-20260528-093235` shows thread
#2 at native `pc=0x7ffd06b7bb0`, `fault=0x7ffd06b7bb0`, `x4=x17=pc`,
`lr=0x7ffd0ab90e4`; vmmap proves the target is an r-- AMD64 guest view
(`win32u.dll`, module base `0x7ffd06a0000`, rva `0x17bb0`), while caller is in
ARM64 `ntdll.dll` generated code. This matches the x64 golden return-target
class: native ARM64 code branched to x64 guest code and must be routed through
HyperBridge.

Fix applied:
- `engine/wine/dlls/ntdll/unix/signal_arm64.c`: allow the existing x64 guest
  fault route in PE32/WOW64 only when x64 loader is enabled, current native
  machine is ARM64, main image is I386, and raw/fault/x4 passes the existing
  registered AMD64 guest-code test. This mirrors x64 behavior without trusting
  arbitrary stale `x4`.
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c`: dispatch x64 callback execution
  with `module_from_pc(target)` as the image base, so PE32 callbacks into 64-bit
  side DLLs mark the correct AMD64 module exec sections instead of the I386 main
  image.

Validation: rebuilt/installed/codesigned `aarch64-unix/ntdll.so` from
`artifacts/phase-h/build-ntdll-wow64-x64-route-20260528-093708.log` (`rc=0`;
only pre-existing warnings). `scripts/verify-build-freshness.sh` is PASS in
`reports/phase-h/freshness-wow64-x64-route-20260528-094552.log`. Direct scoped
Wine run `reports/phase-h/npp-x86-direct-after-wow64-x64-route-20260528-094245`
confirms the former fault now routes: `target=0x7ffd06b7bb0`, module
`win32u.dll`, callback returns `ret=0x1 blocks=12 steps=0x4c`; no
`UNSUPPORTED`, no `MEMORY_FAULT`, no reset-context.

Current next blocker: PE32 no longer sits in the old branch-to-r-- SIGBUS loop,
but still does not reach a Notepad++ window. Direct runs now either return
`rc=53` or spend time in repeated WOW64 `NtMapViewOfSection` activity without an
opcode/memory-fault signature. Next step is to classify that mapping loop/exit
with scoped-prefix runs only. Do not use `run-windows-app.sh` for long runs until
its global cleanup is fixed; it calls `cleanup-wine-runtime.py`, which can kill
parallel Wine work. Use direct `CompatibilityPlan.command` with
`WINEPREFIX=bottles/generic-x86` and stop via that prefix's `WINESERVER -k`.

Update 2026-05-28 13:03 VLAT: resumed in the canonical internal worktree
(`/Users/timurtoby/Documents/MacRunner/Main/MacRunner`); ignore the external
Kimi graphics worktree at `/Volumes/MacOS 1/...` except read-only if explicitly
needed. Latest post-route PE32 reports:

- `reports/phase-h/npp-x86-process-debug-long-20260528-124348`: child spins
  with no `UNSUPPORTED`, no `MEMORY_FAULT`, no process-exit trace; stderr shows
  repeated WOW64 `NtMapViewOfSection` returns `STATUS_IMAGE_NOT_AT_BASE`
  (`40000003`) while mapping PE modules.
- `reports/phase-h/npp-x86-npp-window-poll-20260528-125241`: no Notepad++ CG
  window in 180s. lldb attached to the child at `_sigtramp` with Wine exception
  code in `x24=0xc0000005`; this is not a fresh `PC=0` branch-to-r-- signature.
- `reports/phase-h/npp-x86-normal-window-poll-20260528-125200` is a false
  positive: it matched an unrelated external `notepad.exe` window, not our
  `notepad++.exe`. Future window probes must match `notepad++` and verify the
  owner PID belongs to this worktree/prefix.

Current next step: scoped direct run from the saved compatibility command,
capture a short hot-spin sample/lldb snapshot plus stderr markers, then classify
whether the loop is normal loader rebasing, repeated handled AV, or a WOW64
mapping/state regression. Cleanup remains prefix-scoped only.

Update 2026-05-28 13:16 VLAT: current no-window blocker classified as prefix
deployment, not CPU opcode/status. `reports/phase-h/npp-x86-pgid-long-sample-20260528-131053`
shows real PE32 child `notepad++.exe` running ~97% CPU while loader maps
dependencies, then exiting with `0xc0000135`; `macrunner-start-exit` propagates
that to `start.exe` (`rc=53`). Prefix audit: `bottles/generic-x86/.../syswow64`
had only 11 i386 DLLs and was missing 14 Notepad++ imports (`shlwapi`,
`dbghelp`, `version`, `crypt32`, `wintrust`, `sensapi`, `wininet`, `uxtheme`,
`dwmapi`, `advapi32`, `ole32`, `oleaut32`, `imm32`, `ucrtbase`), while x64
golden/current system32 has a full DLL set. Root fix started in
`scripts/sync-prefix-from-dist.sh`: sync all arch DLLs into system32 and all
i386 DLLs into syswow64, not just the old core subset. Next: run that sync on
`bottles/generic-x86` with `--system32-arch=x86_64-windows`, verify syswow64
imports, then rerun PE32 Notepad++.

Update 2026-05-28 13:29 VLAT: full prefix sync was applied:
`reports/phase-h/sync-generic-x86-full-dlls-20260528-1316.log`; syswow64 now
has 604 i386 DLLs and all Notepad++ imports are present. The next blocker is a
real signal/VM classifier fault, not missing DLLs: post-sync run
`reports/phase-h/npp-x86-postsync-hotspin-lldb-20260528-132501` keeps PE32
`notepad++.exe` alive at ~99% CPU with no window. No `UNSUPPORTED` or
`MEMORY_FAULT` marker appears. Fault trace
`reports/phase-h/npp-x86-postsync-faulttrace-20260528-132350` shows the handler
recursing while classifying an invalid address: `virtual_handle_fault` calls
`get_host_page_vprot()` and faults reading the vprot bucket near
`fault=0x30617009c`; enabling `MACRUNNER_HB_TRACE_FAULTS=1` amplifies this into
recursive `macrunner_hb_native_fault_dump_qwords()` faults. Root fix applied in
`engine/wine/dlls/ntdll/unix/virtual.c`: `get_host_page_vprot()` now unions
host-page protections via bounded `get_page_vprot()` per guest page, avoiding a
cross-bucket read during signal handling. Next: rebuild/install/codesign
`aarch64-unix/ntdll.so`, run freshness, rerun PE32 Notepad++.

Build/validation 2026-05-28 13:30 VLAT: rebuilt `ntdll.so` with the vprot
boundary fix in
`artifacts/phase-h/build-ntdll-vprot-boundary-20260528-1329.log` (`rc=0`, only
pre-existing warnings), installed/codesigned
`engine/wine/dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so`, and freshness is
PASS in `reports/phase-h/freshness-ntdll-vprot-boundary-20260528-1329.log`.

Update 2026-05-28 13:38 VLAT: post-vprot rerun
`reports/phase-h/npp-x86-after-vprot-fix-20260528-133052` no longer shows the
recursive vprot trace fault, but still produces no Notepad++ CG window after
240s. The PE32 child stays hot (~99% CPU), stderr has no `UNSUPPORTED` or
`MEMORY_FAULT`, and lldb catches thread #2 in `_sigtramp` with
`x13=0xc000001d` (`STATUS_ILLEGAL_INSTRUCTION`), `x19=x2=0x5467438b6880b41f`,
`x23=0`, `x24=0`, `x28=0x160616f294`; sample top is `_sigtramp` plus an unknown
native target around `0x438b6880b19f`. Current next step: enable the narrow
xtajit/BTCpuSimulate exception-status trace (not broad recursive fault qword
dumping), capture guest EIP/status/bytes for the `c000001d` path, then mirror
the working x64 route if this is return/BOP target corruption, or apply opcode
family audit if it is a real unsupported PE32 instruction.

Update 2026-05-28 13:53 VLAT: narrow xtajit trace knobs identified in
`engine/wine/dlls/xtajit/cpu.c`: `MACRUNNER_XTAJIT_TRACE_ALL_SIMULATE`,
`MACRUNNER_XTAJIT_TRACE_SYSCALLS`, and `MACRUNNER_XTAJIT_TRACE_STACK`. Short
trace run `reports/phase-h/npp-x86-xtajit-trace-all-20260528-133941` was
intentionally killed after 20s; with all-simulate enabled it had not yet reached
the `_sigtramp`/`c000001d` state and logged no `publish-exception-context`.
Evidence gathered: PE32 execution is running through normal WOW64 BOP syscalls,
with repeated returns through `NtUserCallOneParam` (`svc=0x133d`, `ret=7a9304ac`)
near the latest trace tail. Next probe must be low-overhead and attach to the
actual child executable line (`.../notepad++.exe Z:\...`), not the parent
`wine start.exe` line.

Update 2026-05-28 18:07 VLAT: low-overhead callback/signal trace
`reports/phase-h/npp-x86-callback-signal-trace-20260528-174647` classifies the
current hot spin as guest32 execute control-flow, not an opcode or missing DLL.
The old x64 callback route still works first (`win32u.dll` target
`0x7ffd06b7bb0`, `ret=0x1`), then WOW64 starts normally
(`BTCpuGetBopCode -> ... guest=00270000`). After loader mapping, the process
enters a tight signal loop at `pc=fault=0xb68000`; sample shows thread #2 stuck
under `_sigtramp` for the whole sample. This is the PE32 analogue of the x64
return-target/callback route: ARM64 native control reached a 32-bit guest code
address and Wine's native signal path keeps re-raising it instead of re-entering
the i386 CPU. Next: mirror the proven x64 signal-route shape, but target the
WOW64/i386 CPU entry path; do not add status-pointer patches.

Patch 2026-05-28 18:14 VLAT: implemented the PE32 mirror route, scoped to
WOW64/I386-on-ARM64 execute faults. `signal_arm64.c` now refuses to let
`virtual_handle_fault()` "fix" a low 32-bit execute fault by mprotect/retry;
instead it raises the normal native exception dispatcher. `wow64/syscall.c`
then recognizes that dispatcher case in `Wow64PrepareForException`, restores
the I386 `Eip` to the low guest PC, and re-enters the existing `cpu_simulate()`
loop. This mirrors the x64 callback route concept while using the established
WOW64 CPU entry instead of inventing a direct ntdll->xtajit path. Next:
rebuild/install/codesign `ntdll.so` and `wow64.dll`, sync prefix, freshness,
then rerun PE32 Notepad++.

Build attempt 2026-05-28 18:16 VLAT:
`artifacts/phase-h/build-wow64-i386-exec-route-20260528-1816.log` rebuilt
`ntdll.so` but failed at `wow64.dll` because the manual make environment did not
prepend `engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin`
(`aarch64-w64-mingw32-clang` not found). This is an environment error, not code
evidence. Next: rerun build with the same LLVM_MINGW exports used by
`scripts/build-wine-pure-arm64-experiment.sh`, then install/sync/freshness and
rerun PE32.

Build/install 2026-05-28 18:18 VLAT: reran with the LLVM_MINGW toolchain env.
`artifacts/phase-h/build-wow64-i386-exec-route-20260528-1817.log` built
`ntdll.so` and `wow64.dll` (`rc=0`, only existing warnings plus one pre-existing
`MESSAGE` format warning in `wow64/syscall.c`). Installed/codesigned
`aarch64-unix/ntdll.so`, installed `aarch64-windows/wow64.dll`, synced
`bottles/generic-x86` via
`reports/phase-h/install-wow64-i386-exec-route-20260528-1818.log`, and freshness
is PASS in
`reports/phase-h/freshness-wow64-i386-exec-route-20260528-1818.log`. Next: direct
scoped PE32 Notepad++ run with callback-route trace enabled, then classify
whether the low-guest execute fault now re-enters `cpu_simulate()`.

Run 2026-05-28 18:20 VLAT:
`reports/phase-h/npp-x86-after-i386-exec-route-20260528-182055` did not reach a
window, but it also did not reproduce the low `pc=fault=0xb68000` spin: the
child exited after ~53s with `0xc0000135`, and markers show
`hb_i386_exec_route=0`, `wow64_i386_exec_route=0`, `signal_low_b68000=0`,
`UNSUPPORTED=0`, `MEMORY_FAULT=0`. The earlier x64 callback route still fires
and returns once. This means the new route was not exercised in this run; next
step is loader evidence for the renewed `STATUS_DLL_NOT_FOUND` exit before
touching CPU/status code.

Rerun 2026-05-28 18:29 VLAT without signal-chain trace:
`reports/phase-h/npp-x86-no-signalchain-20260528-182917` proves the new signal
route does fire, but the target is not code. `Wow64PrepareForException` routed
`pc=00b68000`, then xtajit returned `MEMORY_FAULT` with
`reason=unable to fetch executable i386 code`; memory probe says
`00b60000-01b60000 perm=3` and `can_x=0` (RW stack-like region containing ESP/EBP).
This mirrors the x64 oracle rule more closely: only registered/executable guest
targets are callback/return targets. Patch refined in `wow64/syscall.c` so the
WOW64 native-execute route calls `cpu_simulate()` only when `NtQueryVirtualMemory`
shows an executable guest32 page; non-exec low PCs are left to the normal WOW64
exception reset/dispatch path. Next: rebuild/install `wow64.dll`, sync prefix,
freshness, rerun PE32.

Build/install 2026-05-28 18:33 VLAT: rebuilt the refined `wow64.dll` in
`artifacts/phase-h/build-wow64-i386-exec-nonexec-filter-20260528-1832.log`
(`rc=0`, one pre-existing `MESSAGE` format warning), installed/synced prefix in
`reports/phase-h/install-wow64-i386-exec-nonexec-filter-20260528-1833.log`, and
freshness is PASS in
`reports/phase-h/freshness-wow64-i386-exec-nonexec-filter-20260528-1833.log`.
Next: rerun PE32 Notepad++ and verify the stack/non-exec low-PC case no longer
enters `cpu_simulate()`.

Run 2026-05-28 18:34 VLAT:
`reports/phase-h/npp-x86-after-nonexec-filter-20260528-183425` confirms the
filter prevented the bogus `cpu_simulate()` MEMORY_FAULT (`memory_fault=0`), but
it exposed the next root: `Wow64PrepareForException` now spins on the same
non-exec stack PC, logging `native guest32 execute skip nonexec pc=00b68000
state=MEM_COMMIT protect=PAGE_READWRITE` 31k times. `BTCpuResetToConsistentState`
in our xtajit backend is a stub, so returning to Wine's native dispatcher cannot
consume a low guest32 execute fault. Next: mirror x64's evidence rule further and
classify the BOP/return target source for `pc=00b68000`; do not add status-pointer
patches.

Trace 2026-05-28 18:41 VLAT:
`reports/phase-h/npp-x86-module-until-nonexec-20260528-184109` shows another
root-cause clue before the low-PC spin: xtajit publishes exceptions for valid
guest `ntdll` EIPs (`7bde7e06`, `7bde1458`) with host addresses under
`0x27bde...` and all-zero bytes, while module mapping in the same run maps PE32
system DLLs under `0x3....`. This means xtajit's local `guest32_host_ptr()`
derives the guest32 base from `teb + WowTebOffset`, unlike the fixed WOW64 helper
path that uses `MACRUNNER_WOW64_TLS_GUEST32_BASE`. Patch applied in
`xtajit/cpu.c`: use the same TLS guest32 base for xtajit host-pointer
translation, falling back to the old WowTebOffset derivation only if the TLS slot
is absent. Next: rebuild/install/codesign `xtajit.dll`/`xtajit.so`, freshness,
rerun PE32.

Build/install 2026-05-28 18:46 VLAT: rebuilt `xtajit.so` and `xtajit.dll`
cleanly after moving declarations above code:
`artifacts/phase-h/build-xtajit-guest32-tls-base-clean-20260528-1846.log` has no
warnings/errors. Installed/codesigned/synced prefix in
`reports/phase-h/install-xtajit-guest32-tls-base-clean-20260528-1846.log`; build
freshness PASS in
`reports/phase-h/freshness-xtajit-guest32-tls-base-clean-20260528-1846.log`.
Next: rerun PE32 Notepad++ and classify the next boundary.

Run 2026-05-28 18:47 VLAT:
`reports/phase-h/npp-x86-after-xtajit-tlsbase-20260528-184739` no longer enters
the non-exec low-PC route (`nonexec_skip=0`, `memory_fault=0`), but exits after
~53s with `0xc0000135`. Loader trace
`reports/phase-h/npp-x86-loader-after-tlsbase-20260528-184901` timed out under
heavy `+module,+loaddll` before exit, but shows the status already held in
native x64-side state during callback-route rejects (`x19=0xc0000135` at
`raw_pc=0x7ffd0ada578`, `fault=0x48`). Next: find the exact failing
loader/LdrLoadDll return status/name with targeted instrumentation or narrower
trace; do not infer a DLL name from static imports.

Probe patch 2026-05-28 18:56 VLAT: added capped `ERR` instrumentation in
`ntdll/loader.c` (`macrunner-ldr-load-dll-fail` and
`macrunner-ldr-LdrLoadDll-fail`) to print the exact failing loader status/name
under `WINEDEBUG=-all`. This is diagnostic-only and evidence-first for the
current `0xc0000135`; next rebuild PE `ntdll.dll` for the relevant arches, sync
prefix, rerun PE32, then either fix the named dependency/root or remove/narrow
the probe once it has served.

Build/install 2026-05-28 18:54 VLAT: PE `ntdll.dll` probe build for `aarch64`
and `i386` succeeded, but `x86_64-windows/ntdll.dll` still fails to link with
pre-existing `___chkstk_ms` unresolved in the x64 PE ntdll link
(`artifacts/phase-h/build-ntdll-loader-status-probe-fullenv-20260528-1853.log`).
Installed only the successful `aarch64` and `i386` probed DLLs, then synced
prefix:
`reports/phase-h/install-ntdll-loader-status-probe-partial-20260528-1854.log`;
freshness remains PASS in
`reports/phase-h/freshness-ntdll-loader-status-probe-partial-20260528-1854.log`.
Next run may identify failures from the 32-bit loader; if the status originates
only in the x64 PE loader, solve the x64 ntdll link or instrument another layer.

Run 2026-05-28 18:55 VLAT:
`reports/phase-h/npp-x86-ldrprobe-i386-20260528-185537` did not reach the
previous `0xc0000135` exit within 80s; child stayed hot at ~98% after the first
two `NtMapViewOfSection` mappings and produced no loader-fail probe lines. This
means the partial i386/aarch64 loader probe changed timing/control enough to
reveal a hot spin before the prior exit. Next: capture a proper sample/lldb on
this hot-spin shape, then decide whether to keep the diagnostic probe or remove
it after extracting evidence.

Update 2026-05-27 22:18 VLAT: continued PE32/WOW64 only. Do not touch the x64
golden snapshot except as read-only reference. Current local x64 `signal_arm64.c`
diff belongs to earlier x64 work; do not extend it for PE32 unless new evidence
requires the signal layer.

PE32 Notepad++ current exact env still launches a child `notepad++.exe`, but no
Notepad++ CG window appears. Important reports:

- `reports/phase-h/npp-x86-child-spin-probe-20260527-214609`: child PID spins
  ~99% CPU; lldb caught `EXC_BAD_ACCESS address=0x17` at ARM64 PE `xtajit.dll`
  `handle_bop_context`, instruction `str wzr, [x20]` after
  `xtajit_arm64_call_wow64_syscall`. Objdump maps it to `*status =
  STATUS_SUCCESS`. Classification: host `NTSTATUS *status` pointer was kept in
  ARM64 callee-saved `x20` across the WOW64 syscall boundary; that boundary can
  restore guest ARM64 state into `x20`.
- `reports/phase-h/npp-x86-after-statusptr-volatile-20260527-220110`: attempted
  stack reload changed the fault to `address=0x0` at the reloaded status pointer.
  This disproves "save status pointer in host stack slot" as sufficient; the
  stack slot is not reliable after this boundary either.
- Fix now in `engine/wine/dlls/xtajit/cpu.c`: for syscall/unix BOP handling,
  set `*status = STATUS_SUCCESS` before cross-boundary dispatch, and do not store
  success status after dispatch on the hot path. Rebuilt/copied
  `xtajit.dll`/`xtajit.so`; build log
  `artifacts/phase-h/build-xtajit-status-before-dispatch-20260527-220411.log`
  (`rc=0`, one existing C89 declaration-after-statement warning in
  `BTCpuSimulate`).
- `reports/phase-h/npp-x86-after-status-before-dispatch-20260527-220526`:
  old `x20/status` store fault is gone. New current blocker is a separate
  control-flow family: child still spins ~99% CPU, lldb sees thread #2 at
  `PC=0x0` / `EXC_BAD_ACCESS address=0x0`; no `UNSUPPORTED_OPCODE`, no
  `c000001d`, no `MEMORY_FAULT`, no unhandled Wine exception.
- `reports/phase-h/npp-x86-syscall-trace-after-status-20260527-221130`:
  do not misclassify `NtQuerySystemInformation class=102` as terminal. With
  syscall tracing, execution logs later BOP syscalls and `svc=00000004` wait
  returns `0x102`; the next blocker is the null-PC/control-flow path, not a
  missing NtQSI leave log.

Next step: evidence-first classify the `PC=0` return target path. Start from
the latest null-PC reports above; do not add more status-pointer patches, and do
not broaden into x64 callback routing without a trace proving the signal layer is
the active route.

Update 2026-05-27 04:34 VLAT: PE32 Notepad++ HyperBridge boundary progressed
past the x87 environment-control blocker and two later CPU-family gaps. Fixed:

- x87 environment-control family `FWAIT/FNCLEX/FCLEX/FNINIT/FINIT` through
  decode -> IR -> lift -> interpreter -> tests.
- x86 SSE scalar/convert family copied from the already-working x64 path for
  the Notepad++ block at `0x005455d9`: `CVTDQ2PD`, `CVTPD2PS`, `ADDSD`,
  `DIVSS`, `COMISS` and sibling packed/scalar forms.
- x87 `FRNDINT` (`D9 FC`) for the later Notepad++ helper at `0x00782860`.

Validation:

- `engine/hyperbridge/tests/hb_test_runner`: `292 passed, 0 failed`.
- Rebuilt `engine/hyperbridge/libhyperbridge.a`.
- Force-relinked and copied
  `engine/wine/dist-pure-arm64/lib/wine/aarch64-unix/xtajit.so`.
- Bounded 120s PE32 Notepad++ run:
  `reports/phase-h/npp-x86-post-x87-frndint-20260527-043102`.
  Result: `UNSUPPORTED_OPCODE=0`, `c000001d=0`, old `01b5fd90` stack-execute
  loop gone, only one handled early AV remains, process reaches wait/message
  loop (`svc=00000004` returned `0x102`).

Do not touch Wine-render hacks for the remaining visual/window question. The
CPU boundary is now past the known Notepad++ PE32 unsupported opcodes; if the
window is still not visible, keep that separated from HyperBridge opcode
coverage unless a new CPU fault appears.

Update 2026-05-26 21:24 VLAT: PE32 x87 environment-control family is
implemented through decode -> IR -> lift -> interpreter -> tests for
`FWAIT/FNCLEX/FCLEX/FNINIT/FINIT`; HyperBridge validation is green
(`hb_test_runner`: 289 passed, 0 failed; Python x86/decode/IR: 26 passed).
PE32 safety probe after the x64 detour reached `process_ready=1`; the only
error scan hit was the pre-existing `non-application-target pc=0x7ffd02000b0`
safeprobe tail, not a new x86 HyperBridge memory fault.

Temporary x64 detour status: Obsidian notes confirm x64 Notepad++ really did
work (`109-milestone-real-onscreen-window-after-bootstrap-marathon.md`,
`95-achievements-and-multi-agent-state.md`), and report
`reports/phase-h/npp-x64-20260523-234453` reached `FULL_UI_READY`. Current
regression root was a stale ARM64 `x4` register being trusted as an x64 callback
target after `RtlRunOnceExecuteOnce` / `kernelbase!init_current_version`; this
misrouted an ordinary native ntdll fault to Notepad++ thunk `0x140004930`, then
crashed at `0x1400059b0` with `rcx=0x208`. Fixed in
`engine/wine/dlls/ntdll/unix/signal_arm64.c`: trust `x4` as a normalized x64
callback target only when the fault PC/address already identifies x64 guest
execution. Rebuilt/installed/codesigned `aarch64-unix/ntdll.so`; build
freshness is PASS. Post-fix x64 probe no longer reproduces the
`0x140004930 -> 0x1400059b0` crash and stays alive ~17s, but current manual
readiness still fails `CG_WINDOW_READY`; next x64 step is compare against the
2026-05-23 milestone/canonical window path rather than change Wine-render hacks.

Update 2026-05-26 08:34 VLAT: current session stopped to restart Codex after a
`context-mode` MCP transport failure. Resume from
`reports/phase-h/CODEX-RESUME-PE32-X87-CONTEXTMODE-20260526.md`.

Context-mode fix applied outside the repo: killed runaway
`node /opt/homebrew/bin/context-mode` pid `6151` (~96% CPU, ~3.1 GB RSS),
checkpointed/truncated the stale
`~/.claude/context-mode/content/4746962d34e0be46.db-wal` from ~638 MB to 0 B,
and pinned Codex MCP storage in `~/.codex/config.toml`:
`CONTEXT_MODE_DIR=/Users/timurtoby/.codex/context-mode`. New Codex session is
required because the current MCP transport is closed.

PE32 Notepad++ HyperBridge status: `NtContinue(context, TRUE)` small-sentinel
bug is fixed in `wow64_NtContinueEx`, and xtajit reset-context handling resumes
at i386 `RtlUserThreadStart` (`7bde146c`, then `7bde147c`). Fresh blocker is
`UNSUPPORTED_OPCODE` in Notepad++ PE32 at guest `0x0075709d`, bytes `db e2`,
decoded as `fnclex`. Required next implementation is x87 environment-control
family audit/fix: `FWAIT 9B` as NOP, `FNCLEX DB E2` / `FCLEX 9B DB E2`,
`FNINIT DB E3` / `FINIT 9B DB E3`, through x86 decode -> IR -> lift ->
interpreter -> tests. Do not patch around Notepad++; fix the opcode family.

Update 2026-05-25 15:36 VLAT: active repo is now the internal SSD copy
`/Users/timurtoby/Documents/MacRunner/Main/MacRunner`; `/Volumes/MacOS/MacRunner`
is stale/archive. `AGENTS.md`, `config/env.sh`, active docs, runner scripts,
Control Center defaults/tests, generated Wine build metadata, and current prefix
registry were moved to the internal path. `ccache` is active under
`artifacts/ccache`; `scripts/verify-build-freshness.sh` is PASS.

Implemented the reboot handoff fix in
`engine/wine/dlls/wow64/virtual.c:wow64_NtQueryVirtualMemory`: current-process
WOW64 queries now keep low `addr32` for the 32-bit contract/range checks and use
`guest32_host_ptr(addr32)` only for native `NtQueryVirtualMemory()`. Rebuilt and
installed `wow64.dll`; rebuilt/installed/codesigned `ntdll.so` against current
`libhyperbridge.a`. Evidence: in
`reports/phase-h/npp-x86-queryvm-fix-lane-20260525152549/run.json`,
`svc=00000023` returns `status=00000000`; previous `alloc_module`/`EDI=0`
boundary is gone.

Also fixed the launcher routing needed for this PE32 HyperBridge work:
`scripts/run-windows-app.sh --lane arm64-hyperbridge` now passes the lane to
configurator, and `config/engines.json` allows explicit `arm64-hyperbridge` for
`x86`. Default PE32 launch remains `x86-rosetta-wow64`; pure HyperBridge is
explicit.

Latest PE32 run after TEB mirror fix:
`reports/phase-h/npp-x86-teb32-actctx-20260525153236/run.json`, exits `rc=123`.
The `TEB32+0x1a8` memory fault is gone (`MEMORY_FAULT=0`). Current fresh
boundary is loader returning `STATUS_INVALID_IMAGE_FORMAT (c000007b)` later in
the PE32 load path, around mapped image addresses `6fe5/6fe6...`. Next exact
step: source-search the i386 loader path that sets `c000007b` around those image
validation branches, then add a focused trace if the static path is ambiguous.
Do not revert to Rosetta and do not patch loader symptoms.

Update 2026-05-25 06:46 VLAT: `NtAllocateVirtualMemory` current-process
guest32 translation is now installed in `wow64.dll`; rerun proved the previous
`c0000019` heap commit boundary is fixed. New evidence from
`MACRUNNER_HB_TRACE_FAULTS=1`: HyperBridge stopped at i386
`guest=0x7bd81113 bytes=66 89`, i.e. operand-size-prefixed
`MOV r/m16,r16` in i386 `ntdll`. Fixed the MOV operand16 family in
`hb_decode_x86.c` (`66 89`, `66 8b`, `66 b8+rw`, `66 a1/a3`, plus LEA size
selection) and the shared i386 partial-register mechanism in
`hb_interpreter.c` (`write_reg_sized`, sized LOAD/STORE register operands).
Validation: `engine/hyperbridge make test` PASS, then `xtajit.dll`/`xtajit.so`
rebuilt, installed, and `xtajit.so` codesigned. Next: rerun PE32 Notepad++ and
classify only the next fresh boundary.

Update 2026-05-25 06:21 VLAT: incorporated the new ultra-native strategy as
validation direction, but kept the active scope narrow: PE32 Notepad++ under
pure arm64 Wine + `xtajit`/HyperBridge. Latest evidence after the size128 and
TEB-mirror fixes: `NtAllocateVirtualMemory` reserve succeeds, but the following
commit of the same heap reservation fails with `STATUS_CONFLICTING_ADDRESSES`
(`c0000019`). DISPROVED: BOP syscall stack offset is wrong; raw BOP stack proves
`args = ESP+8` is correct. Current boundary: direct guest32 window requires
current-process WOW64 virtual-memory base-address APIs to pass host pointers to
native `Nt*VirtualMemory`, while returning low 32-bit guest VAs to i386 code.
Patch in progress: map current-process `NtAllocateVirtualMemory`,
`NtAllocateVirtualMemoryEx`, `NtFreeVirtualMemory`, and
`NtProtectVirtualMemory` base addresses through `guest32_host_ptr()`.

Update 2026-05-25 04:48 VLAT: PE32 Notepad++ advanced past the WOW64 syscall
guest-pointer boundary and the stack-as-code failure. Root for the latter was
near `RET imm16` / stdcall cleanup being dropped; fixed the full RET family path
(`decode -> IR -> interp -> JIT-helper`) and added decoder/interpreter tests for
x86/x64 `C3` and `C2 iw`. Current concrete boundary is TEB32 addressing for
guest i386 `fs:` loads. Probe at `7bdb06a0` showed `64 8b 0d 18 00 ...`
(`mov ecx, fs:[0x18]`); first mirror attempt using low32(host TEB32) was
DISPROVED because it collided with i386 `ntdll` code around `0x7bdb6917` and
overwrote executable bytes with TEB data. Active fix: allocate a dedicated safe
guest32 TEB mirror in `0x70000000..0x7f000000`, copy host TEB32 into it before
`BTCpuSimulate`, patch guest `TIB.Self` to the guest base, use that as
`fs_base`, and copy back only `Tib.ExceptionList` after successful simulation.
Next: rebuild/install `xtajit`, rerun PE32 NPP with byte probe, and classify the
next boundary by fresh evidence only.

Update 2026-05-25 00:16 VLAT: PE32 Notepad++ advanced past the x86 decoder
family blockers (`POP r/m32`, operand16 Group1 `80/81/83`, operand16 `C7`) and
the `BTCpuSetContext` low-pointer crash. Current concrete boundary is WOW64
syscall pointer aliasing: `wow64_NtRaiseException()` received guest32
`EXCEPTION_RECORD32`/`I386_CONTEXT` pointers and `exception_record_32to64()`
dereferenced the low guest address natively, crashing in `wow64.dll` at
`exception_record_32to64` (`ldr d0, [x19]`, fault around `0x00d5f874`). Applied
the boundary fix in `dlls/wow64/syscall.c`: map i386 syscall context/exception
pointers through `guest32_host_ptr()` before native dereference in
`NtRaiseException`, `NtContinueEx`, `NtGetContextThread`, and
`NtSetContextThread`. Next: rebuild/install `wow64.dll`, rerun clean PE32 NPP,
then classify the next boundary by fresh evidence only.

Update 2026-05-24 23:19 VLAT: completed the first build integration for the new
PE32 CPU module. Reconfigured `engine/wine/build-pure-arm64`, built
`dlls/xtajit/aarch64-windows/xtajit.dll` plus `dlls/xtajit/xtajit.so`, installed
them into `engine/wine/dist-pure-arm64/lib/wine/aarch64-windows/` and
`aarch64-unix/`, and signed `xtajit.so`. Artifact check: `xtajit.dll` is PE32+
AArch64, `xtajit.so` is Mach-O arm64, codesign verifies, and BTCpu exports
include `BTCpuGetBopCode`, `BTCpuProcessInit`, `BTCpuThreadInit`,
`BTCpuSetContext`, `BTCpuSimulate`, `BTCpuNotifyMemoryAlloc`, and
`__wine_get_unix_opcode`. Remaining blocker before real PE32 launch: Wine's
WOW64 virtual/image mapping must copy/map PE32 bytes into `guest32_window`
backing, not merely notify VMA metadata.

Update 2026-05-24 23:06 VLAT: started the Wine WOW64 CPU DLL layer at the
correct module boundary. Evidence from `dlls/wow64/syscall.c` and
`wine.inf.in`: on native ARM64 with i386 guest, Wine loads `xtajit.dll`
(`Software\\Microsoft\\Wow64\\x86`), not `wow64cpu.dll`; existing tree only had
`xtajit64.dll` for amd64/ARM64EC. Added new `dlls/xtajit` source: PE `cpu.c`
exports the `BTCpu*` surface and calls a Unixlib; Unix `unixlib.c` owns the
HyperBridge-backed `hb_wow64_process_t`/thread state, context import/export,
`hb_wow64cpu_simulate()`, and initial memory notify wiring. Added configure
entries for `enable_xtajit`/`dlls/xtajit`. Local syntax check for both new
sources is clean; HyperBridge validation remains PASS (`202 passed, 0 failed`,
Python `41 tests OK`). Remaining before runnable PE32: regenerate/build Wine
target and fix real winebuild/makedep issues, then wire low-VA image copy into
guest32 mappings.

Update 2026-05-24 22:49 VLAT: added the internal WOW64 execution contract
slice. `hb_wow64cpu_simulate()` now validates the i386 thread/process contract,
fetches executable bytes from the direct `guest32_window` at `EIP`, decodes via
`hb_decode_x86()`, lifts via `hb_lift_func_x86()`, and runs the bounded block via
the selected HyperBridge backend (AOT currently falls back to interpreter). Added
regression `wow64cpu_simulate_runs_i386_guest32_block`, proving a Win32-context
`mov eax, imm32` block executes from guest VA `0x00400000` and exports updated
`EAX/EIP`. Validation: `engine/hyperbridge make test` PASS; C runner
`202 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 22:35 VLAT: added the third x87 batch: ST(i)/pop-register
forms. Covered decode -> lift -> interpreter for `FADD`/`FMUL`/`FSUB`/`FSUBR`/
`FDIV`/`FDIVR` against `ST(i)`, pop variants `FADDP`/`FMULP`/`FSUBP`/
`FSUBRP`/`FDIVP`/`FDIVRP`, `FCOM`/`FCOMP` register forms, `FCOMPP`, `FLD ST(i)`,
and `FXCH`. Added regression `interp_x86_x87_stack_register_pop_core` for
ST-stack arithmetic, `FXCH`, `FADDP`, `FCOMPP`, final empty tag word, and C3
status export. Validation: `engine/hyperbridge make test` PASS; C runner
`201 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 22:27 VLAT: added the second x87 batch: memory
arithmetic/compare (`D8/DC /0..7`) through decode -> lift -> interpreter. Covered
`FADD`/`FMUL`/`FSUB`/`FSUBR`/`FDIV`/`FDIVR` against `m32real/m64real`, plus
`FCOM`/`FCOMP` status-word semantics (`C0`/`C2`/`C3`, pop on `FCOMP`). Added
`hb_x87_set_st_f64()` and `hb_x87_fcom()` helpers. Regression
`interp_x86_x87_memory_arithmetic_compare_core` verifies arithmetic result,
`FCOM equal => C3`, and final empty tag word. Remaining x87 P0 gap is the
separate ST(i)/pop-register variant batch (`FADDP`/`FSUBP`/`FMULP`/`FDIVP`,
`FCOMPP`, `FXCH`). Validation: `engine/hyperbridge make test` PASS; C runner
`200 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 22:21 VLAT: added the first x87 decode/lift/interpreter
family batch: load/store/control/conversion (`FLD m32/m64`, `FSTP m32/m64`,
`FILD m16/m32/m64`, `FISTP m16/m32/m64`, `FLDCW`, `FNSTCW`, `FNSTSW AX`).
This deliberately does not mix in the separate arithmetic/compare batch
(`FADD`/`FSUB`/`FMUL`/`FDIV`/`FCOM*`), which remains next x87 work. Also fixed a
root decoder-contract bug: `hb_decode_next()`/`hb_decode_at()` now dispatch to
`hb_decode_x86()` for `HB_ARCH_X86`; before this they always used the x64
decoder. Regression `interp_x86_x87_load_store_control_conversion_core` verifies
guest32-backed x87 CW load/store, integer conversion, real64 load/store, and
status-word export. Validation: `engine/hyperbridge make test` PASS; C runner
`199 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 22:01 VLAT: added the first PE32 ABI contract slice. The x86
ABI helpers now validate `HB_ARCH_X86`/32-bit mode/memory presence and propagate
guest-stack write faults instead of silently ignoring them. Added fastcall and
thiscall setup APIs (`ECX`/`EDX` and `ECX=this`, respectively) beside existing
cdecl/stdcall. Regression `x86_abi_calling_convention_stack_contracts` verifies
argument order, return sentinel placement, register arguments, target `EIP/pc`,
and fault behavior on an unmapped guest stack. Validation:
`engine/hyperbridge make test` PASS; C runner `198 passed, 0 failed`, Python
suite `41 tests OK`.

Update 2026-05-24 21:53 VLAT: closed the first real x86 execution-path slice on
top of `guest32_window`. `resolve_addr()` now treats every `HB_MODE_32BIT`
effective address as a wrapped 32-bit guest VA, including segment bases, instead
of requiring the x64-only `addr32` IR flag. Fixed `hb_lift_func_x86()` to set
`func->cfg->entry` like the x64 lifter; without that, x86 lifted functions could
not execute. Added regression
`interp_x86_guest32_memory_operands_wrap_to_direct_window`: `ebx=0xffffffe0`
plus disp32 wraps to guest addresses `0x4/0x8` and reads/writes through the
direct guest32 arena. Validation: `engine/hyperbridge make test` PASS; C runner
`197 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 21:32 VLAT: added the HyperBridge-side WOW64 CPU contract
spine. New `hb_wow64cpu` C ABI exposes versioned process/thread/context structs,
process init with guest32 arena reservation, thread init with x86 context, BOP
opcode retrieval, i386 context import/export, and memory notify wrappers wired to
guest32 VMA map/protect/free. This is not yet the PE arm64 `xtajit.dll`; it is
the stable internal ABI that `BTCpuProcessInit`/`BTCpuThreadInit`/`BTCpuSimulate`
can wrap next. Validation: `engine/hyperbridge make test` PASS; C runner
`196 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 21:25 VLAT: added the first x87 foundation slice for the
PE32 vertical MVP. `hb_regs_x86_t` now owns an `hb_x87_state_t` with default
control word/tag/top state. Added `hb_x87` helpers for reset, stack push/pop,
`FLDCW`/`FNSTCW`, and `FISTP i32` with x87 control-word rounding modes. This
targets the CRT-critical `_ftol`/integer-conversion path before full x87 decode.
Validation: `engine/hyperbridge make test` PASS; C runner
`193 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 21:17 VLAT: implemented the next PE32 memory-layer slice in
HyperBridge. Added a 4GB-aligned `guest32_window` direct mapping API:
`hb_memory_guest32_reserve/base/to_host/map/protect/unmap`, VMA generation
counters, and guest32 range split/protect/free tests. Validation:
`engine/hyperbridge make test` PASS; C runner `191 passed, 0 failed`, Python
suite `41 tests OK`.

Important boundary found during implementation: Apple Silicon host pages are
16KB while Win32 guest pages are 4KB. Therefore host `mprotect()` cannot be the
source of truth for exact Win32 4KB permissions. The direct-window path now uses
VMA metadata for 4KB guest permissions/invalidation generation and host backing
only at host-page granularity. This keeps the NO-SoftMMU model, but the future
x86-32 JIT must consult/encode VMA permission assumptions for faults/SMC instead
of relying on host page faults for every 4KB guest page.

Update 2026-05-24 18:31 VLAT: started the real HyperBridge memory-layer work for
the PE32 blocker. Added `hb_memory_map_private()` so a guest fixed VA such as
`0x400000` can be represented as guest address `0x400000` while backed by a host
mapping above macOS `__PAGEZERO`. Read/write/protect now use `host_base` for
allocated/private regions and preserve existing live-host mappings for Wine-owned
native ranges. Added regression `memory_private_guest_low_va_backing`; validation:
`engine/hyperbridge make test` PASS and C runner reports `189 passed, 0 failed`.
This is a foundation piece only; it does not yet make Wine WOW64 launch PE32.
The remaining boundary is still the Wine WOW64 process-parameter / image mapping
layer plus an arm64 `xtajit.dll`/HyperBridge x86 CPU module.

Update 2026-05-24 17:04 VLAT: retried forced pure-arm64 HyperBridge launch for
32-bit Notepad++ after clearing stale Wine processes. Result is unchanged at the
real blocker:
`reports/phase-h/npp-x86-forced-hyperbridge-20260524070209./`.
Failure key lines:
`map_fixed_area out of memory for 0x400000-0xb51000`,
`WINEPRELOADRESERVE ... overlaps preloader __PAGEZERO`, then
`Assertion failed: (!status), function build_wow64_parameters, file env.c,
line 1823`. This confirms again that the failure is low 32-bit VA/WOW64 CPU
integration, not missing DLL installation.
Also tested the low-pagezero arm64 probe with the valid local Apple Development
identity and Wine entitlements:
`artifacts/phase-h/lowpagezero-appledev-probe/`. It is still killed by macOS,
so ad-hoc signing was not the cause of the low-VA wall.
Added configurator fail-fast so PE32 cannot be forced onto `arm64-hyperbridge`
while the x86 WOW64 CPU/memory layer is absent. Implementation boundary is
captured in
`reports/engine-audit/X86-HYPERBRIDGE-WOW64-MEMORY-LAYER-SPEC.md`.
FEX's public Wine WOW64 backend confirms the needed `xtajit` contract but is not
a macOS drop-in: it also allocates bridge trampolines in the lower 2GB, which is
the address range blocked by macOS arm64 `__PAGEZERO`.

For immediate dev/manual testing only, a 32-bit Notepad++ window was launched
through the existing compatibility lane `x86-rosetta-wow64`:
`reports/phase-h/npp-x86-dev-rosetta-hold-20260524070345./`.
Window verified by CGWindow: `new 1 - Notepad++ [Administrator]`, owner
`notepad++.exe`. This is explicitly not pure HyperBridge. The fallback window was
closed before continuing pure-HyperBridge work to avoid confusing it with a real
x86 HyperBridge launch.

Update 2026-05-24 16:32 VLAT: 32-bit Notepad++ dev path is bounded. Dev launch
through `scripts/run-windows-app.sh` opens
`artifacts/phase-h/npp-x86/notepad++.exe`, but that is the `x86-rosetta-wow64`
lane. Forced pure-arm64 launch with absolute path and `WINEARCH=wow64` still
fails before UI:
`reports/phase-h/npp-x86-pure-arm64-abs-20260524-162402/`.
Root boundary: not prefix/DLL setup. The i386 PE wants low `0x400000`, and
`build_wow64_parameters()` needs low <2GB process parameters, while both
`dist-pure-arm64/bin/wine` and `wine-preloader` have 4GB `__PAGEZERO`. Minimal
low-pagezero arm64 probes are killed by macOS (`rc=137`), and deallocating/mapping
low `0x400000` from a normal arm64 process is also killed. Second blocker:
`dist-pure-arm64` has only `x86_64-windows/wow64cpu.dll`; there is no arm64
`wow64cpu` that routes Wine WOW64 to HyperBridge x86. Detailed report:
`reports/engine-audit/X86-PURE-HYPERBRIDGE-WOW64-BLOCKER.md`. Honest current
32-bit lane remains Rosetta until a HyperBridge-backed WOW64 CPU/memory layer is
built.

INSTALLER phase is active. PERF phase is complete enough for the requested task
order. Product Notepad++ Installed E2E now launches through `MacRunner.app` with
the live engine/dist and a freshly synced installer prefix. The Open dialog blank
list regression is closed by the shell32 drive-PIDL path fix and verified on the
real product window, but manual acceptance found a remaining shell-dialog visual
bug: the Open dialog file list is populated, while the left namespace pane is
blank and details-list item icons are missing.

Freeze-on-app-switch remains open, but current automation did not reproduce it;
do not spend cycles on focus-toggle or synthetic clicks. The next freeze step is
to use the newly installed lifecycle trace on the next real manual repro.

Performance work is evidence-driven on real Notepad++ UI-smoke scenarios. Current
artifacts:

- Baseline active sample: `reports/performance/npp-active-20260523-231858/`
- Trace-gate fix sample: `reports/performance/npp-post-tracegate-20260523-233115/`
- Live-region cache sample: `reports/performance/npp-post-live-region-cache-20260523-233849/`
- Block trace-gate sample: `reports/performance/npp-post-block-tracegate-20260523-234449/`
- IR-cache sample: `reports/performance/npp-post-ir-cache-20260523-235347/`
- Read-cache failure sample:
  `reports/performance/npp-post-live-read-cache-20260524-025717/`
- Control after read-cache revert:
  `reports/performance/npp-after-read-cache-revert-20260524-030411/`
- Boundary/env cleanup samples:
  `reports/performance/npp-post-direct-native-fastpath-20260524-031847/`,
  `reports/performance/npp-post-envgate-cache-20260524-032504/`,
  `reports/performance/npp-post-native-write-env-cache-20260524-033235/`,
    `reports/performance/npp-post-special-write-env-cache-20260524-033815/`,
    `reports/performance/npp-post-memory-region-lookup-cache-20260524-034541/`,
    `reports/performance/npp-post-import-target-map-20260524-040821/`,
    `reports/performance/npp-direct-live-mem-optin-20260524-041256/`,
    `reports/performance/npp-post-direct-live-mem-default-20260524-041702/`

Measured fixes applied:

- Cached interpreter trace env flags per `hb_interpreter_run`; clean run no longer
  calls SIMD/mem/branch tracing probes per instruction. `trace_simd_data_exec`
  samples dropped 1892 -> 0.
- Cached successful dynamic live VM regions from `macrunner_hb_special_read/write`
  into HyperBridge memory metadata. `macrunner_hb_special_read/write` samples
  dropped to 0 and `mach_vm_region` dropped from 2000 to near-zero in the next
  comparable sample.
- Cached block-level diagnostic gates in `macrunner_hb_run_x64`; clean run no
  longer calls calc/NPP-open trace probes per block. Both dropped to 0 samples.
- Added per-run IR block cache by guest PC. It reduced `macrunner_hb_lift_one_block`
  from 1011 -> 44 and `hb_lift_func_x64` from 914 -> 37 in the post-cache sample.
- Avoided duplicate guest-stack reads when a direct-native target is already a
  registered import thunk; the registered path now routes to `call_import_thunk`
  before the generic direct-native argument read.
- Cached clean-path diagnostic env gates in `macrunner_hb.c` and native-write
  trace env gates in `hb_memory.c` / `macrunner_hb_special_write`.
- Removed one duplicate `hb_memory_find_region` lookup from successful
  `hb_memory_read/write` by carrying the permission-check region into the
  read/write body.
- Added a target hash map for registered import thunks so direct-native target
  routing no longer linearly scans the import table on the hot path.
- Enabled direct same-process live memory read/write by default for nonallocated
  HyperBridge live regions, with `MACRUNNER_HB_DISABLE_DIRECT_LIVE_MEM=1` as an
  emergency rollback knob. This avoids the Mach VM syscall path for normal
  readable/writable in-process regions while keeping the old path available.

Functional verify after each perf patch: `scripts/verify-build-freshness.sh` PASS,
NPP UI-smoke `overall_effective=PASS`, `case=clean_exit status=PASS`. Visual/icon
paths were not changed.

Latest clean PERF sample is
`reports/performance/npp-post-direct-live-mem-default-20260524-041702/`.
It is functionally green (`overall_effective=PASS`, clean exit) and improved the
same full UI-smoke wall estimate from `169s` in
`npp-post-import-target-map-20260524-040821/` to `157s`. Sample deltas in the
same scenario: `hb_runtime_run -8.3%`, `hb_interpreter_run -8.2%`,
`exec_instr -7.2%`, `hb_memory_read -8.3%`, `mach_vm_read_overwrite -9.5%`,
`server_select -8.5%`. PERF phase is good enough to move back to the P0 freeze
track; remaining perf work is later import arity/prototype evidence and JIT
coverage, not a blocker for the requested task order.

## Current Bug

Open dialog shell visuals are active for installer acceptance.

Update 2026-05-24 12:37 VLAT: user screenshot shows the file list populated
(`autoCompletion`, `plugins`, `notepad++.exe`, etc.) but no left-side
`Favorites/Desktop/My Computer` tree and no file/folder glyphs in the details
list. Static boundary:

- left pane: `shell32/ebrowser.c` reserves nav-pane space and calls
  `CoCreateInstance(CLSID_NamespaceTreeControl)`, but no real
  `INameSpaceTreeControl2` implementation is present in this tree; only the
  caller/event sink exists. This explains a reserved but blank nav pane.
- list icons: `shell32/shlview.c` attaches `Shell_GetImageLists()` and inserts
  rows with `I_IMAGECALLBACK`; `LVN_GETDISPINFO` maps rows through
  `SHMapPIDLToSystemImageListIndex()`. Next boundary is whether the system image
  list/indices are valid or whether the listview draw/callback path drops them.

Do not mark installer E2E complete until the real Open dialog shows namespace
tree entries and file/folder icons onscreen.

Notepad++ x64 freeze remains active but deprioritized. User reproduced a real
click/event freeze during the live human-in-the-loop run.

Update 2026-05-24 12:25 VLAT: user later reported the freeze did not reproduce
while switching between windows, but prior Claude-Code switch freezes remain
unclosed. Keep the boundary below and only resume freeze work on a fresh real
repro.

Captured boundary from `reports/phase-h/npp-x64-20260523-212033/stderr.log`:
real clicks reached `NSApplication sendEvent`, `handleMouseButton`, and
`WineEventQueue` (`queue_post` + `queue_signal rc=1`). After line 97212,
Notepad++ pid `35490` stopped entering `macdrv_ProcessEvents`; the queue kept
growing to about 41 pending events. Therefore the break is between queue signal
and Wine-side wake/drain, not wndproc, not render, not syscall/native target.

DISPROVED fix attempt: `OnMainThread` re-signaling `WineEventQueue` after a
QUERY_EVENT-only wait is not sufficient. Rebuild/install/codesign of
`winemac.so` passed freshness, but run
`reports/phase-h/npp-x64-20260523-213910/` froze before manual acceptance.
Fresh sample paths:
`reports/phase-h/MILESTONE/real-event-freeze-after-resignal-20260523-213910/pid-37713-freeze-214656.sample.txt`
and `pid-37741-freeze-214702.sample.txt`.

New boundary from `npp-x64-20260523-213910/stderr.log`: NPP pid `37713` received
real AppKit events (`app_sendEvent_*`/`handleMouseButton_*` all returned) and
posted to NPP `WineEventQueue` `0x850f8c7c0` hundreds of times. Queue depth grew
to about 48 pending events. `queue_signal` fired repeatedly, including for the
NPP queue (`fd=13` write side), and `onmainthread_resignal_pending` fired 511
times. However NPP only entered `macdrv_ProcessEvents` 25 times and dequeued
only 2 events; after that, mostly `explorer.exe` pid `37741` entered
`ProcessEvents` with its own queue. Therefore the failure is not "pipe never
re-signaled"; the next boundary is `set_queue_fd`/`QS_DRIVER`/`process_driver_events`
ownership and wake/drain routing for the NPP queue.

App-switch freeze boundary from
`reports/phase-h/npp-x64-20260523-220445/stderr.log`: user switched to another
macOS app and Notepad++ froze. Samples:
`reports/phase-h/MILESTONE/event-routing-driver-trace-20260523-220445/pid-40586-switch-freeze-220952.sample.txt`
and `pid-40613-switch-freeze-220958.sample.txt`. NPP pid `40586` was parked in
AppKit `nextEvent` on the main thread and in x64 `NtWaitForMultipleObjects` /
`server_select` on another thread; CPU was 0.0%. NPP queue `0xbeaf90700`
(`read_fd=3`, `write_fd=13`) received `WINDOW_LOST_FOCUS` and `APP_DEACTIVATED`
events, and `queue_signal rc=1` succeeded. After line 11210, NPP thread `tid=24`
did not enter `process_driver_events`; only explorer pid `40613` continued
draining its own queues. Therefore the new boundary is not AppKit event receipt
or pipe write; it is Wine-side wait/message-pump wakeup for the NPP GUI thread
after app deactivation.

Second app-switch repro with wait tracing:
`reports/phase-h/npp-x64-20260523-222805/`. Samples:
`reports/phase-h/MILESTONE/event-routing-wait-trace-20260523-222805/pid-43252-switch-freeze-223254.sample.txt`,
`pid-43275-switch-freeze-223258.sample.txt`, and `pid-43254-switch-freeze-223301.sample.txt`.
NPP queue owner was native thread `6812327` / queue `0x774fa4740`. After line
11732, NPP logged zero `wait_message`, zero `NtUserMsgWait`, zero
`NtWaitForMultipleObjects`, and zero `process_driver_events` on Wine GUI thread
`tid=24`, while AppKit posted 216 events and signaled the queue 236 times. The
sample no longer shows native thread `6812327`; only the AppKit main thread and
unrelated wait threads remain. Next trace must prove whether the GUI thread
returns `WM_QUIT` / runs thread detach, or otherwise exits the message pump while
leaving the Cocoa app and stale event queue alive.

Lifecycle trace repro:
`reports/phase-h/npp-x64-20260523-224712/`. Freeze sample:
`reports/phase-h/MILESTONE/event-routing-lifecycle-trace-20260523-224712/pid-45490-freeze-225148.sample.txt`.
Queue owner was native thread `6825913`, queue `0xac8f9c740`. After line 25489
there are no more Wine-side `wait_message` / `NtUserMsgWait` / `process_driver_events`
entries for GUI thread `tid=24`, while AppKit continues posting/signaling the
queue. `WM_QUIT`, `NtUserPostQuitMessage`, `macdrv_ThreadDetach`, and
`queue_destroy` are absent. The owner thread still runs `OnMainThread`/QUERY_EVENT
until line 25624, then disappears from the macOS sample without normal Wine driver
detach. User cannot continue manual app-switch testing now; freeze is deprioritized
but remains active with this boundary. Next freeze work should trace
`macrunner_hb_BaseThreadInitThunk` / `macrunner_hb_x64_thread_entry` /
`RtlExitUserThread` to identify why the x64 GUI thread leaves the message pump
without unregistering the Cocoa event queue.

Update 2026-05-24 04:58 VLAT: lifecycle trace is now installed in both PE
`ntdll.dll` and Unix `ntdll.so`:
`hb_BaseThreadInitThunk_enter/unix_return/exit_call`,
`hb_x64_thread_entry_begin/return`, and `hb_run_guest_return`. Rebuilt and
installed x86_64/aarch64 PE `ntdll.dll` plus Unix `ntdll.so`; freshness PASS.
Automation attempts did not reproduce the manual freeze:
`reports/phase-h/npp-x64-20260524-044935/` switched Notepad++ away/back through
Finder/Terminal, then sampled pid `90872`; GUI thread was still alive in
`macrunner_hb_x64_thread_entry -> wait_message -> server_select`, and stderr
continued logging `wait_message`/`process_driver_events` through line 15505.
This is NOT closure. Keep freeze open as real-user-only until a fresh manual
freeze supplies the new lifecycle markers.

## Freeze Capture Protocol

Current question: when a real macOS click arrives while the window is frozen,
does it enter `NSApplication sendEvent`, get posted to `WineEventQueue`, wake the
queue/CFRunLoop source, reach `macdrv_ProcessEvents`/`macdrv_handle_event`, and
return from the Wine/server input calls? Instrument the event path and classify
the first missing or non-returning stage. Do not spend cycles on frontmost/focus.

Fresh trace run after killing the stale frozen instance:
`reports/phase-h/npp-x64-20260523-204929/`, live Notepad++ pid `30037`.
Automated 12-cycle sequence (`New -> File -> Edit -> Find`) did not reproduce
the parked freeze: every click reached `ProcessEvents`, `macdrv_handle_event`,
and returned from `NtUserSendHardwareInput`. Leave this trace-enabled fresh
window for manual reproduction; if it freezes, inspect this run's `stderr.log`
from the last click boundary instead of reusing stale-run evidence.

UPDATE3 from swarm: freeze reproduces only from real user clicks, not synthetic
events. Do not use synthetic click loops as repro evidence. Current live
human-in-the-loop run: `reports/phase-h/npp-x64-20260523-212033/`, launched via
detached screen `macrunner-freeze-trace-212033`; Notepad++ pid `35490`, window
`21500`. Trace env is enabled (`MACRUNNER_TRACE_UI_EVENT_PATH=1`,
`MACRUNNER_TRACE_UI_INPUT=1`). User should click/type manually until freeze,
then inspect the tail of this run's `stderr.log` to classify the first missing
stage: NSEvent handler, Wine queue post/signal, queue dequeue/ProcessEvents,
hardware message, or wndproc/message dispatch.

## Closed This Turn

- `wineboot.exe` helper EXE redirect is no longer incomplete: fresh host-exec
  traces show `image_base=0x140000000 machine=0xaa64 x64_guest=0`.
- The remaining bootstrap blocker was the DLL lane inside native ARM64 helpers:
  native `wineboot.exe` was still mapping x64 prefix `gdi32/shell32/user32/
  kernelbase/ntdll/comctl32/win32u`. Fixed in the loader by forcing native helper
  wrong-arch builtin candidates to current-machine `aarch64-windows`, and by
  allowing AMD64 machine mismatch only in the real AMD64 app lane.
- Verify after rebuild: `scripts/verify-build-freshness.sh` PASS; bounded
  `wineboot --init` exited `rc=0` after 2s with zero x64 maps/range registrations.
- Real Notepad++ window appeared. User on-screen screenshot at 2026-05-23 15:25
  shows colored toolbar icons. Window capture
  `reports/visual-regression/notepad-gate-manual-20260523-152238/notepad-window.bmp`
  reports `toolbar_band_colorful=10464`, `colorful=13304`.
- Icon bug is closed in `docs/ENGINE-CHANGE-JOURNAL.md` as ROOT-FIX /
  `verified-onscreen`.
- Secondary-window class-registration bug is closed: `Open` and `Find` dialogs
  create real onscreen windows after registering builtin classes on the
  `get_desktop_window()` early `top_window` return path.
- Open-dialog blank list in the installed/product prefix is closed. Root:
  shell32 filesystem folder init treated a drive PIDL display name as
  `L" (C:)"`, so `CreateFolderEnumList` enumerated a bogus path instead of
  `C:\Program Files\Notepad++\*`. Fix: derive drive paths with `_ILGetDrive` in
  `shfldr_fs.c` before falling back to `SHGetPathFromIDListW`. Verification:
  `scripts/verify-build-freshness.sh` PASS after rebuilding/syncing
  `shell32/comdlg32`; real `MacRunner.app` card `Notepad++ Installed E2E`
  launched `artifacts/installer-e2e/bottles/generic-x86-rosetta-wow64`, and
  on-screen capture
  `reports/installer-e2e/product-open-dialog-after-drive-fix.png` shows the
  Open dialog populated with `autoCompletion`, `plugins`, `langs.model.xml`,
  `notepad++.exe`, etc.

## DISPROVED — Do Not Recheck Without New Evidence

- `__wineboot_event` / service idle was not the root for the latest hang; the
  verified sample pointed at HB signal/range handling, and fresh traces found the
  native-helper wrong-arch DLL lane.
- `wineboot.exe` main image still x64 at `0x140000000` is false in current traces.
- Registered x64 guest range covers `0x140013f84` is false in current traces; old
  x64 ranges were high DLL views.
- `dyld dlopen_from` under `virtual_mutex` remains current is false after the host
  fix.
- `get_startup_info` `env_size` underflow is false for this path.
- Page-sized live-read cache in `hb_memory_read` is false for this path. It made
  `reports/performance/npp-post-live-read-cache-20260524-025717/` fail with
  `EXEC_FAULT pc=0x1400c6b69`; likely stale live memory because Wine/host writes
  bypass `hb_memory_write`. It was reverted and control
  `reports/performance/npp-after-read-cache-revert-20260524-030411/` returned to
  UI-smoke PASS.
- `rc=134` / invalid-free remains current is false after `get_pe_file_info`
  initialization.
- Wine-render/comctl32 toolbar patches are not the fix path; stock Wine render
  baseline is locked.
- The Notepad++ new-tab/menu issue is not a CPU spin or mutex deadlock per
  `sample-npp-hang-newtab-154227.txt`; all sampled threads are parked.
- The Notepad++ new-tab/menu issue is not render/icon: toolbar color is already
  verified on the real screen.
- The Notepad++ menu/Find issue is not general input/message dispatch: manual
  acceptance proved ordinary buttons react and text input works; prior trace
  showed a normal click reaches `WM_LBUTTONDOWN`/`NtUserMessageCall`.
- `winemac.drv` popup/dialog surface presentation is not the first boundary for
  Find: dialog creation failed earlier in user32/win32u class lookup with
  `ERROR_CLASS_DOES_NOT_EXIST`.
- The recurring Notepad++ freeze is not fixed or woken by
  `set frontmost to true`, app switching, or focus-toggle; user verified only
  force-quit recovers. Do not re-run focus-toggle without new counter-evidence.

## Artifacts

- `reports/phase-h/wineboot-x64-range-trace-20260523-150645/`
- `reports/phase-h/wineboot-long-sample-20260523-150819/`
- `reports/phase-h/MILESTONE/ntdll-so-native-helper-dll-lookup-build-20260523-152052.log`
- `reports/phase-h/wineboot-after-native-helper-dll-lookup-20260523-152205/`
- `reports/phase-h/MILESTONE/visual-gate-after-native-helper-dll-lookup-20260523-152237.log`
- `reports/phase-h/npp-x64-20260523-152238/`
- `reports/visual-regression/notepad-gate-manual-20260523-152238/`
- `reports/phase-h/npp-x64-20260523-153552/`
- `reports/phase-h/MILESTONE/sample-npp-hang-newtab-154227.txt`
- `reports/phase-h/npp-x64-20260523-163853/`
- `reports/phase-h/npp-x64-20260523-173253/`
- `reports/phase-h/npp-x64-20260523-174750/`
- `reports/visual-regression/notepad-secondary-verify-20260523-174750/`
- `reports/phase-h/MILESTONE/win32u-builtin-classes-clean-build-20260523-180952.log`
- `reports/phase-h/npp-x64-20260523-181225/`
- `reports/visual-regression/notepad-secondary-verify-20260523-181225/`
- `reports/phase-h/npp-x64-20260523-204929/`
- `reports/phase-h/MILESTONE/event-trace-20260523-204929/`
- `reports/phase-h/npp-x64-20260523-212033/`
- `reports/phase-h/MILESTONE/real-event-freeze-live-20260523-212033/`

## Update 2026-05-29 09:10 (Antigravity, Claude Opus 4.6 Thinking):

**What:** Dirty-tree triage + commit of all pending Codex/Cline/Kimi work (94 files).
No code changes made by this agent. This is a handoff-cleanup session.

**Commits made (4):**
1. `3172f52` — pe32: HyperBridge x86 decoder/lifter/interpreter + WOW64 signal (39 files, 15K+/3.7K-)
2. `aa37d0c` — docs/config: canonical workspace migration + PE32 investigation state
3. `4288c19` — app: control center UI (Cline/Kimi)
4. `51d450a` — docs/scripts/tools: status updates, validation, profiles

**Current blocker (unchanged from Codex 18:59 update):**
PE32 Notepad++ c000001d — process alive but no CG window. `_sigtramp` with
`x13=0xc000001d` (STATUS_ILLEGAL_INSTRUCTION), unknown native target. Codex
identified xtajit trace knobs but didn't get `publish-exception-context`
(guest EIP + instruction bytes) before running out of tokens.

**Next step:** Get `publish-exception-context` via scoped xtajit trace, classify
the instruction, apply family fix per AGENTS protocol.

**ARM64EC research brief:** NOT done. Focus is PE32 c000001d.

## Update 2026-05-29 12:xx (Antigravity Opus + Claude coordinator verify):

**Opus fix landed — commit `6b7886b`:** `win32u: Initialize GdiSharedHandleTable
in PEB32 on 64-bit Unix initialization`.
- Root cause: our single 64-bit `win32u.so` (`_WIN64` always defined) only set
  `peb64->GdiSharedHandleTable`; the `#ifndef _WIN64` upstream branch that would
  set the 32-bit PEB is dead in our unified-win32u design, so 32-bit `gdi32.dll`
  read `peb32->GdiSharedHandleTable == 0` → `c0000005` in `get_gdi_client_ptr`.
- Fix (gdiobj.c:585-595): added `#else` branch — when `NtCurrentTeb()->WowTebOffset`
  set, mirror `gdi_shared` into `peb32->GdiSharedHandleTable`. Verified vs vanilla
  WineHQ + MacRunner baseline: this is a correct symmetric mirror of the existing
  upstream pattern (idiomatic `WowTebOffset`), NOT a hack, NOT masking a HyperBridge
  bug. Bug is genuinely in our Wine OS-layer. CORRECT FIX.

**VERIFIED BY COORDINATOR (clean run, no trace flags):**
- `c0000005` is GONE — loader proceeds far past old crash. Opus fix CONFIRMED.
- BUT: **NO WINDOW.** Opus's "fully launch and execute" claim was overstated — he
  read a 2.2M-line *syscall-trace* log as "launched". It is not.

**NEW BLOCKER (this is the live one):** 100% CPU **hot-spin** (not a syscall block).
- Log freezes at `NtQuerySystemInformation enter class=102` (SystemModuleInformation
  family) with no `leave`. Process pegs 98-100% CPU indefinitely.
- GUI never reached: `0` `NtUser*` / `NtGdi*` / `CreateWindow` calls in log.
- Syscalls reached: NtMapViewOfSection ×128, NtAllocateVirtualMemory ×18,
  NtQuerySystemInformation ×2 (last = class 102, hung).
- Repro: `reports/phase-h/run_window_clean.sh` (clean, WINEDEBUG=-all, no auto-kill).

**Hypotheses for next agent (evidence-first, pick via scoped trace):**
1. WOW64 thunk for QSI class=102 returns malformed 32-bit-layout struct → guest
   loops retrying / iterating bad module list.
2. JIT/interp stuck in a tight loop on an opcode inside the module-enumeration path.
3. QSI class=102 not implemented for WOW64 → returns success with garbage.

**Next step:** scoped xtajit trace around QSI class=102 to capture guest EIP +
instruction bytes at the spin point; classify (WOW64 syscall thunk fix vs opcode
family fix per AGENTS); mirror x64-oracle (how class=102 returns on working x64
path — read-only). DO NOT stop until real Notepad++ x86 window is on screen
(CG-capture), not "process alive".

**Discipline note:** verdict = window pixels, not log length. Confirmed twice now.

## Update 2026-06-09 17:13 (Codex Lane A):

**HK ARM64X callback route fix landed locally:**
- File: `engine/wine/dlls/ntdll/unix/macrunner_hb.c`
- Change: post-normalize callback targets are classified against the metadata-bearing
  ARM64X module; executable ARM64X native targets are called directly instead of
  falling into x64 PE fallback.
- Build/deploy: `dlls/ntdll/ntdll.so` rebuilt from the absolute root and copied to
  `/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`;
  deployed binary was ad-hoc codesigned.

**One HK validation run (absolute paths, no rerun):**
- Report: `reports/phase4-hollow-knight/laneA-postnormalize-arm64-direct-20260609-170857/`
- Counts: `wine-process-primary=1`, `wine-process-primary-installed=1`,
  `dispatch-fallback=0`, `normalize-trace=80`, `NtUserCreateWindowEx=0`.
- Evidence: normalize maps `ntdll.dll` `0x87fffa170c0` -> `0x87fff9f2a60`, and
  callback dispatch targets `0x87fff9f2a60` without PE fallback.

**Current blocker:** route fallback is fixed, but HK still does not reach
`NtUserCreateWindowEx`. The same run enters a post-direct signal loop:
first signal after normalized callback is `pc=0x0 fault=0x0 x26=0x87fffa170c0`,
then repeated bus signals continue with `x26=0x87fffa170c0`.

- 2026-06-09 23:45 Lane A HK: latest blocker after dispatch loop fix is native ARM64EC `NtGetTickCount` during `update_load_config`; patched PE `user_shared_data` high-address init, pending rebuild/run.

- 2026-06-09 23:56 Lane A HK: USD fault cleared; next visible blocker was `set_security_cookie` fault at `loader.c:5601` (`ldr w8,[x18,#0x40]`, fault `0x40`). Patched ARM64EC seed path; pending rebuild/run.
2026-06-10 00:09 · Lane A HK: latest blocker is ARM64 LdrInitializeThunk NtContinue into low x64 PE entry; patched low-PE HB detector to route strict AMD64 executable entries.
2026-06-10 00:18 · Lane A HK: low-PE route run proved macrunner_hb_amd64_main_on_arm64 stayed false; patched build_main_module to force HB for ARM64 process + AMD64 main.
2026-06-10 00:24 · Lane A HK: after AMD64-main gate run, Ldr X0 stayed in non-executable main image data; added main-AEP fallback in signal_arm64.c.
2026-06-10 00:30 · Lane A HK: primary still times out inside loader_init; added no-thread-entry probe to classify helper/primary Ldr handoff misses.
 · Lane A HK · current blocker: primary installs signal handler but does not emit LdrInitializeThunk after-loader; loader_init phase-probe added to locate stall · next run must check macrunner-hb-loader-phase
2026-06-10 00:52 · Lane A HK · current blocker: primary installs signal handler but does not emit LdrInitializeThunk after-loader; loader_init phase-probe added to locate stall · next run must check macrunner-hb-loader-phase
2026-06-10 01:06 · Lane A HK · run /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/phase4-hollow-knight/laneA-loader-phase-probe-20260610-005502: primary=1 fallback=0 pc0=0 NtUser=0; blocker shifted before LdrInitializeThunk/loader_init, last primary signal pc=0x87fff9cd86c fault=0x3004 · next map PC and patch root
2026-06-10 01:12 · Lane A HK · applying fix for primary pre-Ldr stall: signal-entry trace suppressed while x18=0; previous run last PC mapped to ntdll!__wine_dbg_output ldr [x18,#0x3004] · next run should reach loader/Ldr/NtUser
2026-06-10 01:24 · Lane A HK · run /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/phase4-hollow-knight/laneA-x18-signal-log-guard-20260610-011343: signal-entry recursion fixed (count=0), but primary stops after init_syscall_frame normalized=0x87fff9c7418 before any LdrInitializeThunk/loader phase; next target dispatcher return/frame setup
2026-06-10 01:31 · Lane A HK · next probe targets signal_arm64ec.c:LdrInitializeThunk because primary native target maps there, not signal_arm64.c · expect macrunner-hb-arm64ec-ldr markers
2026-06-10 01:43 · Lane A HK · applying init-frame fallback fix: previous run stopped after init frame and never entered signal_arm64ec Ldr; next run should show macrunner-hb-segv-earlyinit-fallback and then Ldr/loader/HB thread entry
2026-06-10 01:55 · Lane A HK · previous run: pending_x64 set but fallback skipped; x16 dispatcher route stole control. Broadened pending fallback to any depth=0 initial fault · next run should show macrunner-hb-segv-earlyinit-fallback/ldr-ctx-xlat
2026-06-10 02:07 · Lane A HK · previous run: fallback fired but ctx-xlat had Rip/Rsp/Rcx=0 and x16 dispatcher stole route. Fixed by saving original arm ctx and forcing pending route · next run should route x64 Ldr thunk correctly

## 2026-06-10 02:19 Lane A HK loader entry
- Previous pending-context run reached good AMD64 context synthesis but still normalized forced x64 thunk to ARM64 native. Current fix keeps raw x64 thunk PC for HB dispatch; build and one HK run pending.

## 2026-06-10 02:36 Lane A HK preserve raw dispatch
- Current blocker: lower callback dispatch normalized forced raw x64 Ldr thunk `0x87fff9f0500` to ARM64 native `0x87fff9c7418` after signal route. Applied one-shot preserve-raw dispatch flag; build and one HK run pending.

## 2026-06-10 03:01 Lane A HK ARM64EC entry context
- Current fix: raw x64 Ldr entry path now keeps the saved ARM64 init context instead of synthetic AMD64 context, and aligns ARM64EC tagged native targets before direct native calls. Build and one HK run pending.

## 2026-06-10 03:15 Lane A HK callback-run probe
- Current state: raw dispatch and ARM context are preserved, but HK still times out before `macrunner-hb-ldr-after-loader-init`. Added bounded callback-run probe; build and one HK run pending.

## 2026-06-10 03:30 Lane A HK native ARM64EC entry
- Current fix: first x64 entry block only tail-jumped to native Ldr; dispatch now calls the native entry directly with saved ARM64 context/stack. Build and one HK run pending.

## 2026-06-10 03:44 Lane A HK native entry stack
- Current fix: native ARM64EC entry now gets saved ARM context plus allocated signal stack pointer instead of raw top-of-stack. Build and one HK run pending.

## 2026-06-10 03:58 Lane A HK Ldr entry probe
- Current state: native entry helper is called, but no PE Ldr marker appears. Added entry/xlat markers in `signal_arm64ec.c`; build and one HK run pending.

## 2026-06-12 21:34 Lane D HK graphics reach check
- Lane D verified the RPC/services fix in real HK x64 DXMT runs:
  `RPC_S_SERVER_UNAVAILABLE=0`, no epmapper missing fault, services/rpcss start
  under `mr-run`.
- HK still does not reach graphics: `CreateDXGIFactory=0`,
  `D3D11CreateDevice=0`, `UnityWndClass=0`, `GfxDevice=0`.
- Evidence:
  `reports/phase4-hollow-knight/laneA-laneD-rpcss-services-try1-212030/`,
  `reports/phase4-hollow-knight/laneD-rpcss-sample2-212855/`,
  `reports/phase4-hollow-knight/laneD-wait-trace-213215/`.
- Current class: `WAIT_DEADLOCK` at rung `mono-init`; wine sample points through
  `macrunner_hb_try_kernel32_handle_semantic -> NtWaitForSingleObject/server_wait`.
  This is pre-DXGI/HyperBridge ownership, not a DXMT implementation gap.
## 2026-06-12 22:55 Lane D HK x64 DXGI pre-entry root evidence

Lane D verified the previous rpcss/epmapper fix: current HK x64 DXMT runs no longer show RPC_S_SERVER_UNAVAILABLE, but still do not reach CreateDXGIFactory/D3D11CreateDevice/Present.

New evidence points at HyperBridge executing DXGI builtin data as code before public DXGI markers:
- lift-probe hot loop in `DXGI.DLL` at runtime RVAs `0x15cc0`, `0x16040`, `0x16054`, `0x16066`, all disk `.rdata` ASCII strings (`%02x...`, `dxgi_device_GetGPUThreadPriority`, etc.).
- runtime guard probe showed synthetic builtin section metadata reports those RVAs as `.text sec_size=0xbaef6 sec_chars=0x60000020`; disk `dxgi.dll` `.text` is only `0x13bd6`, `.rdata` starts at `0x15000`.
- CFG fast path (`macrunner_hb_try_x64_cfg_dispatch_fast_path`) also trusts `RAX` targets with no executable-target validation, but existing predicates were poisoned by the synthetic `.text` size.

Implemented but pending HK validation due active `x18waittrace2` runner:
- `loader.c` now registers original AMD64 executable PE section ranges before Wine rewrites builtin module headers.
- `macrunner_hb.c` keeps a side-table of original exec ranges and makes `macrunner_hb_pc_in_executable_section()` use that table as authoritative for registered modules.
- Diagnostic probes remain gated by `MACRUNNER_HB_TRACE_EXEC_GUARD` / `MACRUNNER_HB_TRACE_LIFT_PROBE`.

## 2026-07-02 10:22 Lane A HK GLM-DXMT deploy check

- Restorable rung-9 snapshot created before GLM deploy:
  `artifacts/milestone-dist/hk-rung9-dxgi-factory-20260702-100320`.
- Deployed GLM/graphics-prep DXMT dist from
  `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-graphics-prep/engine/graphics/dist/dxmt`
  into the current overlay. Hash report:
  `reports/research/dxmt-glm-options4-deploy-20260702-100554.sha256.txt`
  (`source_head=8400e1e`, `main_head=fbb6b25`).
- HK GLM 300s run:
  `reports/phase4-hollow-knight/laneA-dxgi-glm-options4-try1-100619`.
  Raw reached real `CreateDXGIFactory2` boundary but no `macrunner-dxmt-fence`,
  no `CreateSwapChain`, no `Present`, no `CheckFeatureSupport` / `OPTIONS4` /
  `EnumOutputs` / `GetDesc`, no fault. Timeout `exit=143`. Sample showed
  CPU-bound HyperBridge memory-protect sync:
  `macrunner_hb_try_kernel32_handle_semantic -> macrunner_hb_sync_virtual_region
  -> hb_memory_protect -> rebuild_region_tree -> tree_insert`.
- HK GLM 420s confirmation run:
  `reports/phase4-hollow-knight/laneA-dxgi-glm-options4-420-try1-101357`.
  Raw `real_factory=0`, no Mono reload, no device fence, no swapchain/present,
  no fault. Filtered classify reports rung 6 with self-check FAIL because there
  are no real DXGI evidence lines. Sample shows a pre-DXMT busy hang in display
  callback transport:
  `win32u!get_system_metrics -> get_virtual_screen_rect -> lock_display_devices
  -> load_display_driver -> KeUserModeCallback -> macrunner_hb_route_x64_callback_fault
  -> macrunner_hb_pc_in_executable_section`.

Current identity: GLM OPTIONS4 DLLs did not advance HK to `CreateSwapChain`.
The observed silence is not caps-query / `OPTIONS4` / DXGI output enumeration;
it is a CPU-bound pre-DXMT HyperBridge user-callback/display-driver transport
or memory-protection sync path, depending on run timing.

## 2026-07-02 12:08 Lane A HK warm-cache and HB hotspot cleanup

- `scripts/laneA-run-hk.sh` now defaults `MACRUNNER_HB_TRANSLATION_CACHE=1`.
  `MACRUNNER_HK_COLD_RUN=1` is the explicit cold-translation override. If the
  caller does not provide `MACRUNNER_HB_TRANSLATION_CACHE_ROOT`, the wrapper uses
  `artifacts/hb-translation-cache/ntdll-<sha16>` from the deployed
  `aarch64-unix/ntdll.so`.
- Cache hygiene verdict: the persistent block key in HyperBridge does not include
  the engine binary/codegen hash, so build-scoped roots are required. Current
  deployed `ntdll.so` after HB perf fixes is
  `e8e0fabee9b97498bcf15c4accdacf410c513853fead276b7d759841b0123c2f`; cache root
  `artifacts/hb-translation-cache/ntdll-e8e0fabee9b97498`.
- Clean warm A/B verdict: pre-GLM and GLM DXMT behave the same when measured with
  warm cache. Both reach real DXGI factory territory and still do not reach
  `CreateSwapChain`/`Present`; GLM DLLs are not the regression.
- Fixed HB hotspot 1: `hb_memory_protect` no longer rebuilds the full region tree
  for permission-only protects. The rebuild is limited to topology-changing
  splits. Sample terms dropped from `tree_insert=540/787` before the fix to
  `tree_insert=130/205` after the fix.
- Fixed HB hotspot 2: disabled HK diagnostic env gates inside
  `hb_jit_helper_exec_ir_block_once` no longer call `getenv()` per IR block. The
  latest warm sample dropped `getenv` textual hits from ~590 to ~17-22.
- Evidence marker fix: `laneA-run-hk.sh` now enables real D3D boundary and DXGI
  swapchain markers by default. Filtered evidence run
  `reports/phase4-hollow-knight/laneA-postjit-warm-evidence-glm-try1-120655`
  classifies as rung 9 (`D3D11_CREATE_DEVICE_MISSING`, self-check PASS,
  `real_factory=2`).
- Current measured frontier: true cold-no-cache baseline
  `reports/phase4-hollow-knight/laneA-cold-nocache-glm-try1-121112` classifies
  as rung 9 `PRESENT_MISSING` with self-check PASS (`real_factory=2`,
  `d3d11_device_markers=1`, `swapchain=0`, `present=0`). Raw populate run on the
  same GLM closure and `ntdll-e8e0...` cache root also reached
  `macrunner-dxmt-fence` but no `CreateSwapChain`/`Present`:
  `reports/phase4-hollow-knight/laneA-laneA-postjit-populate-glm-try1-115428`.
  True warm runs still vary after real DXGI factory; sample shows no callback
  storm (`route_x64_callback_fault=0`, `pc_in_executable_section` low). Remaining
  warm profile is HB JIT/interp execution during Mono reload/post-Physics, with
  many `mach_msg2_trap` samples, not the old tree rebuild/getenv/callback scan.

## 2026-07-02 13:06 Lane A HK HWND binding cleared

- Binding failure identity: `IDXGIFactory2::CreateSwapChainForHwnd(hwnd=0x20054)`
  was failing below DXMT because winemac had not realized `win_data` for Unity's
  top-level window when swapchain creation arrived. The old run only had the
  fail-loud summary; the fixed run's full field trace shows the original NULL
  step was the `win_data`/realization stage, not macdrv symbol resolution.
- `hwnd=0x20054` facts from raw log: owner thread 36 equals current thread 36,
  `root=0x20054`, `is_root=1`, `parent=0x10020`, desktop-parented, style
  `0x94000000`, full client/window rect `(0,0)-(1512,982)`.
- Fix: `macdrv_ensure_win_data()` creates real macdrv data/Cocoa window on demand
  using current USER rects, and `d3dmetal.c` re-runs client-surface update/present
  after realization so the CAMetalLayer is attached to the real HWND path.
- Verification run:
  `reports/phase4-hollow-knight/laneA-hk-hwnd-bind-fix-125609-try1-125711`.
  Raw fields: `client_cocoa_view=0x72aa20f00`, `ret_view=0x72aa31b80`,
  `ret_layer=0x764179770`, `attached_to_hwnd=1`.
- Progression: `CreateSwapChainForHwnd` returned `rc=0x0` with
  `swapchain=0xedcb05c90`. No real `GetBuffer`/backbuffer marker and no real
  swapchain `Present` yet; the apparent `candidate method=Present slot=8` is
  actually factory `MakeWindowAssociation(hwnd=0x20054, flags=3)`, confirmed by
  DXMT warning `MakeWindowAssociation: Ignoring flags 3`.
- Filtered classifier: `LADDER_RUNG: 11 (swapchain)`, primary
  `SILENT_SPIN_NO_MARKERS` due watchdog timeout after the swapchain advance.
- Snapshot floor:
  `artifacts/milestone-dist/hk-rung11-swapchain-hwnd-bind-20260702-130629-20260702-130629`.

## 2026-07-02 13:48 Lane A GLM latest post-swapchain frontier

- Redeployed latest GLM/graphics-prep DXMT runtime from source head `333c038`;
  hash report:
  `reports/research/dxmt-glm-latest-redeploy-20260702-132040.sha256.txt`.
- Verified rung 11 still holds with latest DXMT:
  `CreateSwapChainForHwnd(hwnd=0x20054)` returns `rc=0x0`,
  `swapchain=0xed8b06380`, and HWND binding remains attached
  (`ret_layer=0xa607e4f00`, `attached_to_hwnd=yes`).
- Last confirmed call after swapchain is factory `MakeWindowAssociation(hwnd=0x20054, flags=3)`,
  returning `rc=0x0`. Unity does not call `GetBuffer(0)`, does not create an RTV,
  and does not make a real swapchain `Present` before timeout.
- Patched `tools/triage/analyze_d3d_gate.py` to ignore
  `macrunner-hb-dxgi-swapchain: candidate method=Present` lines; they are
  unknown-object probes and can be factory slot-8 `MakeWindowAssociation`, not
  real swapchain `Present`.
- Post-swapchain raw fault diagnostic:
  `UnityPlayer.dll+0x2b5605`, `mov rax, qword ptr [r8 + rsi*8 + 0x488]`,
  effective address `0xcfe355638` -> `MEMORY_FAULT`. Sample shows active HB
  `macrunner_hb_sync_virtual_region -> hb_memory_protect`, with helper threads
  waiting in `NtWaitForSingleObject/server_wait`.
- Report: `reports/research/laneA-glm-latest-postswap-20260702-1348.md`.

## 2026-07-02 15:14 Lane A post-swapchain Unity+0x2b5605 / HB bookkeeping

- Snapshot floor before this round:
  `artifacts/milestone-dist/hk-rung11-latest-glm-postswap-20260702-141251-20260702-141251`.
- Old raw fault run:
  `reports/phase4-hollow-knight/laneA-hk-glm-latest-postswap-long-132919-try1-132920`.
  `UnityPlayer.dll+0x2b5605` fired once, not as a repeated recoverable storm, then the thread took runtime exit status `c000007b`; wrapper later timed out with rc 143.
- Fault instruction remains
  `mov rax, qword ptr [r8 + rsi*8 + 0x488]`, with `r8=0x4c1302cb0`,
  `rsi=0x107a0a4a0`, EA `0xcfe355638`, and `hb_memory_read_u64(EA)` returning
  `MEMORY_FAULT`. The regfile makes an unmapped/producer-corruption class plausible
  because `rsi` is pointer-sized, not a sane small array index, but the address-class
  verdict is not closed: instrumented reruns did not replay the fault.
- Added `macrunner-hb-unity-addrclass` trace for the fault site. No lines emitted yet
  because no rerun hit `UnityPlayer+0x2b5605`.
- Fixed HB bookkeeping hotspots exposed while trying to replay the fault:
  `rebuild_region_tree/tree_insert` removed from `hb_memory_protect` split path, and
  `split_all_regions_at` changed to tree lookup under the non-overlap invariant.
- Verified deployed `ntdll.so` hash:
  `a12a7823b7071c248d3bac578255a18ad90bc0d85abcdd29233d8ab90e0ea70a`.
  Filtered classify on
  `reports/phase4-hollow-knight/laneA-laneA-hk-addrclass-splitfast-20260702-150106-try1-150106`
  preserves rung 11: real `CreateSwapChainForHwnd rc=0`, followed by
  `MakeWindowAssociation`, no `GetBuffer`, no real Present.

## 2026-07-02 18:02 Lane A Unity+0x2b5605 race/init-order discriminator

- Outcome distribution on warm uninstrumented runs did not replay the one-shot
  `UnityPlayer.dll+0x2b5605` fault: default N=8 split between pre-swapchain
  timeout (3/8) and swapchain timeout (5/8); `JIT_DIRECT_STORE_FENCE=1` N=8
  reached swapchain timeout 8/8. No run reached GetBuffer/RTV/real Present.
- Producer walk: the faulting consumer gets `rsi` from
  `UnityPlayer.dll+0x2b5587: mov rsi,[rdx]`; the backtrace callsite
  `0x6bbe80` passes `rdx = container + index*24`. Descriptor slots are seeded
  from `container+0x628/0x630`, and normal targeted trace consumed `{0,0,1}`.
  No local `_Init_thread_header/_Init_thread_footer` guard was found around the
  consumer.
- Fixed diagnostic fence coverage in `hb_arm64_codegen.c`: env-gated
  `MACRUNNER_HB_JIT_DIRECT_STORE_FENCE=1` now covers offset/fused direct stores,
  not just the generic direct-store path. Deployed signed `ntdll.so` hash:
  `48566904f9b703831d90bd6d5903e79694a1156fdd3f52b8cba6449057fbaa4a`.
- Patched fence coverage did not advance the frontier. Populate and warm runs
  both classify as rung 11 `PRESENT_MISSING`: real `CreateSwapChainForHwnd rc=0`,
  no GetBuffer/RTV, no runtime-fail, no `+0x2b5605`.
- Cache hygiene: `hb_cache_key_t` does not include codegen env flags. Future
  fence/no-fence A/B must use separate `MACRUNNER_HB_TRANSLATION_CACHE_ROOT`s.
- Report: `reports/research/laneA-unity-2b5605-race-init-order-20260702-1802.md`.

## 2026-07-02 21:55 Lane A HK rung-11 wait-handle naming

- Added diagnostic wait-handle registry in `macrunner_hb.c` and deployed signed
  diagnostic `ntdll.so` hash
  `b8b7d4978dd1bf707026a19e24c94786443e5a17945a7b248c1b836a0776c887`.
- Main raw evidence run:
  `reports/phase4-hollow-knight/laneA-wait-handle-20260702-211155-try1-211257`.
  Filtered classifier stays rung 11 `PRESENT_MISSING`, with secondary
  `WAIT_DEADLOCK`; real `CreateSwapChainForHwnd rc=0` and no `GetBuffer`/real
  `Present`.
- Refuted the prime frame-latency waitable hypothesis for this run: there is no
  `GetFrameLatencyWaitableObject` / `SetMaximumFrameLatency` marker, and the
  durable blocked handles are not created by DXMT swapchain code.
- Durable waits named:
  `0x70,0x7c,0x88,0x94,0xa0,0xac,0xb8` are Unity-created semaphores from
  `CreateSemaphoreExW(initial=0,max=2147483647)`, waited at caller
  `0x87efd167c92`, pending up to ~592s. `0x110` is a separate semaphore from
  `CreateSemaphoreW(initial=0,max=2147483647)`, waited at caller
  `0x87ef2a5ccba`, pending up to ~587s. No `ReleaseSemaphore` targets those
  handles in the captured run.
- The early Unity semaphore waits look like worker/job semaphores rather than a
  DXGI-side frame-latency object. Need a clean post-`MakeWindowAssociation`
  sample that names the main/render-thread wait specifically; post-swapchain
  targeted trace is now in source but the fresh-hash cache runs had not reached
  swapchain before stopping.

## 2026-07-02 23:20 Lane A sync-shortcut kill-switch and WaitOnAddress pairing

- Added diagnostic env gates for the suspected HB semantic shortcuts:
  `MACRUNNER_HB_DISABLE_WAIT_ADDRESS_SEMANTIC`,
  `MACRUNNER_HB_DISABLE_KERNEL32_HANDLE_SEMANTIC`, and
  `MACRUNNER_HB_DISABLE_KERNEL32_HANDLE_SYNC_SEMANTIC`. Built/deployed signed
  diagnostic `ntdll.so` hash
  `6fb3483b0e660fb2f2c0eb3c3435c7f428edecbf674dc4011a7fa45827a959e9`.
- Static audit: `macrunner_hb_try_kernel32_handle_semantic` intercepts event,
  semaphore, mutex, wait, handle/heap/virtual-memory families. The sync branches
  route to Wine NT primitives (`NtCreate*`, `NtSetEvent`, `NtReleaseSemaphore`,
  `NtWaitForSingleObject`, `NtWaitForMultipleObjects`), not synthetic release or
  wake success. `WaitOnAddress`/`WakeByAddress*` are handled by the separate
  wait-address helper, using `NtWaitForAlertByThreadId` and
  `NtAlertThreadByThreadId`.
- Kill-switch run
  `reports/phase4-hollow-knight/laneA-sync-killswitch-20260702-225918-try1-225918`
  bypassed the suspected paths (`kernel32-handle=39843`, `wait-address=53`;
  top bypasses include `WaitForSingleObjectEx`, `WaitOnAddress`,
  `WakeByAddressSingle`, `ReleaseSemaphore`, `SetEvent`) but still classified as
  rung 11 `PRESENT_MISSING`: real `CreateSwapChainForHwnd rc=0`, no `GetBuffer`,
  no real Present.
- Baseline wait-address run
  `reports/phase4-hollow-knight/laneA-waitaddr-baseline-20260702-231255-try1-231255`
  also reaches rung 11. It shows paired delivery, not a swallowed wake:
  `tid=00f4` waits on `addr=0x3a0414cb0` from caller `0x87efce9ee71`,
  `WakeByAddressSingle` from caller `0x87efce9eebb` finds `tid=00f4`, and the
  waiter returns `status=00000101 (alerted)` before the semantic wrapper reports
  success. Worker wait `addr=0x3002e7200` similarly receives a wake and re-waits.
- Verdict: the COM-like fake-success shortcut hypothesis is refuted for the
  observed HK wall. Disabling sync semantics removes the hot
  `macrunner_hb_try_kernel32_handle_semantic` stack but the wall remains; the
  creator thread shifts into guest/JIT special read/write work. Next lever is HB
  special-memory / virtual-region sync/protect behavior, not WakeByAddress
  delivery.

## 2026-07-02 23:55 Lane A in-flight import naming

- Added `MACRUNNER_HB_TRACE_INFLIGHT_IMPORT` to log native import entry/return
  pairs with seq id, guest/native tid, dispatch path, `module!function`, target,
  guest PC/return address, last error/status, and args0-11. Default trace is now
  swapchain-creator-thread only; `MACRUNNER_HB_TRACE_INFLIGHT_IMPORT_ALL=1`
  enables the full flood. Final deployed diagnostic `ntdll.so` hash:
  `5b637d73740031b4a2e88f9ddf0e530203da787b3b4609488cf8fc2a3da18f1f`.
- Evidence run before the creator-only hygiene tweak:
  `reports/phase4-hollow-knight/laneA-inflight-import-20260702-233300-try1-233300`
  used deployed hash
  `c2c3297b4ac9bab1062dd78501e93362c347f43ddadf909f05ed1988708f9b3c`
  and explicit warm root
  `artifacts/hb-translation-cache/ntdll-b8b7d4978dd1bf70-jitstorefence1-waithandle`.
  Sanitized classifier copy strips only `macrunner-hb-inflight-import` lines and
  classifies rung 11 `PRESENT_MISSING`, self-check PASS: real
  `CreateSwapChainForHwnd rc=0`, no `GetBuffer`, no real Present.
- The swapchain creator is `tid=0x24`, native `Thread_46229431`
  (`native_tid=0x2c167b7`). No in-flight import remains unmatched at timeout,
  including the creator thread: the "API X never completes" framing is not
  supported by this run.
- Creator post-swapchain import profile is a tight SRW loop, all returning:
  `ReleaseSRWLockExclusive=3834`, `AcquireSRWLockExclusive=2070`,
  `TryAcquireSRWLockExclusive=1764`, `AcquireSRWLockShared=890`,
  `ReleaseSRWLockShared=890`, plus condition-variable and registry calls. The
  direct `pe_call12` path executes the callee on the same native thread via
  `blr target`; there is no separate executor thread for these imports.
- All-thread sample
  `sample-pre-swapchain-allthreads-233938.txt` maps the creator to
  `Thread_46229431`, CPU-active rather than parked. Dominant creator stack is
  `macrunner_hb_run_x64 -> hb_jit_runtime_run`; recursive counts show
  `hb_memory_read=441`, `macrunner_hb_special_read=310`,
  `mach_vm_read_overwrite=300`, `hb_memory_write=183`,
  `macrunner_hb_special_write=66`, with no creator `NtWaitForSingleObject`,
  `NtWaitForAlertByThreadId`, `NtDelayExecution`, or `server_wait`. Other worker
  threads are parked separately. Verdict: slow/starvation, not deadlock or a
  non-returning import. Next lever is the special_read/special_write fast path and
  SRW/import-call overhead in the post-swapchain creator loop.

## 2026-07-03 01:12 Lane A special-memory throughput split

- Added `MACRUNNER_HB_TRACE_SPECIAL_ACCESS_SAMPLE` to sample the swapchain
  creator's guest PC and special read/write addresses every five seconds. The
  baseline run
  `reports/phase4-hollow-knight/laneA-special-sample-baseline-20260703-000016-try1-000016`
  reached `CreateSwapChainForHwnd rc=0` and then timed out with no `GetBuffer`,
  no RTV, and no real Present. Sampler verdict: progress, not spin. At the end
  it had `total=48,411,932`, `pc_unique=64+`, `addr_unique=64+`,
  `page_unique=64+`, and an advancing address range; there is no single polled
  flag/writer to chase.
- `MACRUNNER_HB_DIRECT_MEM=1` in
  `reports/phase4-hollow-knight/laneA-special-directmem-ab2-20260703-002219-try1-002219`
  proved the lazy-path direct-copy code is effective but insufficient. Final
  special-io counts were `read_direct_ok=85,646,969` vs `read_mach=7,246` and
  `write_direct_ok=17,245,545` vs `write_mach=240`, but the run still timed out
  before `GetBuffer`. The remaining wall is lazy helper/JIT memory coverage and
  volume, not the mach copy syscall itself.
- `MACRUNNER_HB_JIT_DIRECT_MEM=1` with `MACRUNNER_HB_JIT_NATIVE_MEM_IR=0`
  is not a safe partial unblock in current HEAD. It reopens the known
  `mono-2.0-bdwgc.dll+0x385e73` `c000007b` fault before swapchain. A no-cache
  block trace showed the exact consumer block is `CMP word ptr [RBX], R8W; JNE`;
  native fault instruction is the direct scalar halfword load (`ldrh`) with
  `RBX=0xffffffff01000166`.
- Added two codegen isolation gates and cache hygiene: explicit
  `MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN=0` now wins over `JIT_DIRECT_MEM=1`;
  new `MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM=0` disables scalar GPR direct-memory
  lowering. Persistent translation-cache version is now 19, with scalar-mem in
  the key. Final deployed `ntdll.so` hash:
  `6531b51a7439d85d60f6e3f22419f4fbeb124ca9143185146426eeed5f673e4c`.
- Isolation run
  `reports/phase4-hollow-knight/laneA-special-jitdirect-scalarmem0-20260703-005639-try1-005639`
  used `JIT_DIRECT_MEM=1`, `NATIVE_MEM_IR=0`, `DIRECT_SCALAR_SCAN=0`,
  `DIRECT_SCALAR_MEM=0`. Result: no `mono+0x385e73` fault and real DXGI/D3D11
  COM calls reached, but no swapchain before timeout. Verdict: the directmem
  bad-pointer class is scalar GPR direct-memory lowering, not qword native-mem
  IR and not XMM/direct-stack. Next root-cause is the producer inside scalar
  direct-memory lowering; until then, use `DIRECT_SCALAR_MEM=0` for honest
  isolation and keep separate cache roots per codegen flag combo.
- Reproducibility check on 2026-07-03 found the earlier `fc1a6924...` local
  rebuild was a stale/hybrid build-dir artifact, not a valid clean-source floor.
  A true clean HyperBridge + forced ntdll rebuild from HEAD produced signed
  `ntdll.so` `6fbf685b6c4373744b043f4fd9b4cc839ed03310eb223a87196d914727b8a5fa`
  and reached filtered rung 11 in
  `reports/phase4-hollow-knight/laneA-reconcile-head-clean-safe-try1-120422`
  when scalar/native JIT direct-memory paths were explicitly off. The runner now
  exports `MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM=0`, and the codegen default only
  enables that path when `MACRUNNER_HB_JIT_DIRECT_MEM=1` is explicit.

2026-07-04 · ABZU ARM64X view saga status
- CodeMap-aware in-image dispatch closed the ARM64X nonexec/wrong-view class in the 900s ABZU run: nonexec=0, refuse=0, dxgi+0x12c78=0, c0000005=0, RSS max 1.07GB.
- Remaining blocker is no longer ARM64X nonexec: c000007b=3 runtime exits in ABZU/dynamic callback path, and graphics triage reports no real D3D11CreateDevice marker.

## 2026-07-04 - HK Mono throughput plateau / region-fusion WIP

Current state: region-fusion lever-2 is built and deployed in `dist-arm64ec-spike` behind default-off `MACRUNNER_HB_REGION_FUSION`. Gate is Mono-only for HK metadata/type-resolution plateau. A/B wrapper exists at `scripts/laneA-run-hk-region-fusion-ab.sh`, but first launch refused because game processes were already present; no new HK run was started. Floor is blocked by driftcheck until source is clean and expected WIP-token policy is resolved.

## 2026-07-04 - HK region-fusion A/B result

`hk-region-fusion-ab` completed, but the comparison is invalid: both base (`MACRUNNER_HB_REGION_FUSION=0`) and region (`=1`) timed out before `CreateSwapChainForHwnd rc=0`, so rung 11 did not hold. Region installs/rejects were 0/0 because the post-swapchain Mono plateau was never reached. Do not tune region detector from this run; first restore default-off rung 11 on the current WIP deploy or isolate the pre-swapchain regression/setup issue.

## 2026-07-05 - HK clean ntdll/dxmtpoll restore verify

Clean floor `ntdll.so` (`1cf6f31c...`) and dxmtpoll `d3d11.dll` (`b9eb822c...` aarch64 variant) were restored without rebuild. Post-run hashes stayed correct. HK verify `hk-dxmtpoll-restored-verify` flaked before DXGI: no `CreateDXGIFactory`, no `CreateSwapChainForHwnd`, no heartbeat, `time_to_swapchain=UNKNOWN`, timeout rc=143. Do not rerun region-fusion A/B until this restored baseline reaches rung 11 again.

## 2026-07-05 - HK full dist restore blocked by incomplete clean-head floor

Full `rsync --delete` from `hk-rung11-clean-head-jetsam-attributed-20260704-105034/payload/dist` restored the correct unix/PE `ntdll` pair (`1cf6f31c` / `64570dd0`) and dxmtpoll `d3d11` was overlaid (`b9eb822c` / `303572e0`). Verify run exited before HK because `wineserver` failed to load `l_intl.nls`. The clean-head payload dist contains zero `*.nls` files, so it is not a complete dist for destructive restore. Need full external clean archive before rerunning baseline/region-fusion.

## 2026-07-05 - HK trusted dxmtpoll baseline restored

Restored full dist from dxmtpoll milestone, then overlaid only clean unix `ntdll.so`. Hash gate: unix `ntdll.so=1cf6f31c`, PE `ntdll.dll=64570dd0`, d3d11 aarch64 `b9eb822c`, d3d11 x86_64 `303572e0`, `l_intl.nls` present. Verify `laneA-hk-trusted-dxmtpoll-full-restore-verify-try1-040234` is valid: rung 11 yes, `time_to_swapchain=143s`, DXMT bins `0xeee00/0xef049=0/0`, no regression classes. Region-fusion A/B can be rerun from this baseline when requested.

## 2026-07-05 - HK region-fusion valid A/B verdict

Force-clean rebuilt HyperBridge+ntdll from the applied region-fusion WIP and deployed ntdll-only: `libhyperbridge.a=be7cfa8e`, unix `ntdll.so=33d54abb`, PE `ntdll.dll=27530080`; d3d11 stayed `b9eb822c` / `303572e0`. Valid control is base retry `laneA-hk-region-fusion-base-retry-20260705-111613-try1-111613`: swapchain 156.399s, DXMT bins `0xeee00/0xef049=0`, no fusion traces, no GetBuffer/RTV. Region leg `laneA-hk-region-fusion-valid-20260705-105238-region-try1-105718`: swapchain 147.936s, installs/rejects/trace lines `0/0/0`, no GetBuffer/RTV. Verdict: detector does not catch Mono plateau bins `0x392609/0x392653/0x3926fb`; trace enabled but no candidate/reject lines emit. Report: `reports/phase4-hollow-knight/hk-region-fusion-valid-verdict-20260705-113259.md`.

## 2026-07-05 - HK isolated region-fusion A/B

Non-fusion loader/signal drift was stashed; isolated region-fusion ntdll built as `d70eed1a...`. A/B on trusted dxmtpoll baseline is valid. Base rung 11 holds (`tts=154s`, DXMT bins 0/0). Region rung 11 also holds (`tts=165s`, DXMT bins 0/0) but `region_installs=0`, `region_rejects=0`; Mono plateau unchanged at 30 RVA/min and no GetBuffer. Detector did not reach candidate stage for Mono bins, not a threshold win. Next work: add pre-candidate region-fusion trace or redesign detector for Mono metadata/name-resolution chain shape.

2026-07-07 10:01 · HB EH-dispatch Patch C · implemented RaiseException guest KiUserExceptionDispatcher delivery; targeted ntdll build rc=0; build sha ecf3c8d3a79b55971edb85f445e38e3428124965d854d1156c6ad25eaa942b58; no game run
10:04 · PatchA/PatchB build · start ntdll + hyperbridge artifact build; no runs/no dist install · pending
10:04 · PatchB build · ntdll.so built, SHA recorded · next PatchA hyperbridge build
10:04 · PatchA build · hyperbridge built, SHA recorded (engine/hyperbridge/libhyperbridge.dylib) · no runs/no dist install

2026-07-07 10:25 · PATCHC verify · PARTIAL: no Galaxy64+0x831682/e06d7363, swapchain rc=0 at +146.157s tid=0x64, process to +930s; GetBuffer/RTV/Present1=0; separate ntdll EH fault code=406d1388 at +33s

10:57 · HK ntdll+0x6229c diagnosis · root cause: guest x64 ntdll __wine_unix_call_dispatcher cell is zero; virtual_unwind WINE_UNIX_CALL hits jit-nullcall at 0x622d3 · next: patch dispatcher-cell init before/at AMD64 ntdll load

11:24 · Patch D verify · PARTIAL: ntdll dispatcher cell fixed (unix 0->native, syscall 0->native), old 0x622d3 nullcall=0; next blocker KiUserExceptionDispatcher tail INT3 after unhandled Mono 406d1388; GetBuffer=0

11:47 · Patch E verify · PARTIAL: KiUser tail INT3 gone, old nullcall gone, semantic RaiseException uses RtlRaiseException for 406d1388 and e06d7363; new blocker ntdll!RtlCaptureContext rva=0x618dc unsupported opcode 0x9c pushfq; swapchain +177s, GetBuffer=0
11:56 · Patch F · implemented dedicated HB JIT PUSHF/POPF helper path; build/verify pending
12:00 · Patch F cleanup · main worktree cleaned of Patch A leftovers in hb_runtime.c/hb_context.h; C/D/E/F files preserved
12:01 · Patch F cleanup · remaining Patch A resume_exact_pc references removed; rebuild retry
12:02 · Patch F build · SUCCESS; libhyperbridge=956afa4ba0ff6675f5ddfcaf8aa4d64bf0fa2cc05386b90394116beedd774d4e ntdll.so=2bf8885930ac09df784d998b139e5141887eb79818a0e308bca8a54334fbf01d
12:02 · Patch F deploy · dist ntdll.so SHA256=2bf8885930ac09df784d998b139e5141887eb79818a0e308bca8a54334fbf01d; libhyperbridge static in ntdll.so
12:22 · Patch F verify · PARTIAL; RtlCaptureContext block now fails at FXSAVE rva=0x6197a after pushfq, swapchain reached, GetBuffer/RTV/Present1=0
12:27 · Patch G build · FXSAVE/FXRSTOR decode+lift built; libhyperbridge=a0c4053fb11ce113cff505d9a33ead88ba5704c5175a2dafd924b167dcb58dac ntdll.so=2bf8885930ac09df784d998b139e5141887eb79818a0e308bca8a54334fbf01d
12:27 · Patch G relink · forced ntdll.so relink; SHA256=e29b5af1fab5f4abf03494b3ef2da8b8047b7c0d52a75f0251fc0085b64d0fe0
12:27 · Patch G deploy · dist ntdll.so SHA256=e29b5af1fab5f4abf03494b3ef2da8b8047b7c0d52a75f0251fc0085b64d0fe0; HB static in ntdll.so
12:45 · Patch G verify · PARTIAL; RtlCaptureContext FXSAVE fixed (unsupported/runtime-fail=0), run reaches Unity D3D init/Mono reload, GetBuffer/RTV/Present1=0
13:34 · Patch G verify2 · PARTIAL_COLD; warm root ntdll-e29b5af1fab5f4ab did not reach real swapchain in 900/1800s; EH remains clean

14:01 · HK PatchG verify2 regression · report written: cache poisoning not supported; likely benign 0x406d1388 EH completion/resume semantics block Mono reload before swapchain.

14:08 · HK PatchG EH A/B · ntdll rebuilt and deployed with MACRUNNER_HB_EH_COMPLETION_PROBE and MACRUNNER_HB_EH_BENIGN_NOOP gates.

14:24 · HK PatchG EH probe · 406d1388 did not hit expected Mono resume PC after RtlRaiseException; starting MACRUNNER_HB_EH_BENIGN_NOOP A/B.

14:43 · HK WINSHOW F2+F3 · applied show_window on-demand path and WS_POPUP activation gate; native artifacts deployed for verify.

15:13 · HK PatchG EH A/B · report written. Benign 0x406d1388 EH completion confirmed as swapchain regression; no-op restores swapchain, WINSHOW verify inconclusive because UI trace produced 23.9GB log and was scoped-stopped.

16:22 · HK cleanup · removed invalid 23.9GB winshow-verify run artifacts before bounded verify2; disk 97Gi free -> 97Gi free.

16:30 · HK WINSHOW verify2 · report written. Bounded run FAIL: F2/F3 reaches d3d_on_demand but CreateSwapChain does not return; no WM_ACTIVATE/Present1. Disk: /dev/disk3s1   460Gi   336Gi    98Gi    78%    2.5M  1.0G    0%   /System/Volumes/Data

16:38 · HK WINSHOW trace · built/deployed MACRUNNER_HB_TRACE_WINSHOW artifacts for verify3. Disk: /dev/disk3s1   460Gi   337Gi    97Gi    78%    2.5M  1.0G    0%   /System/Volumes/Data

16:46 · HK WINSHOW defer · built/deployed deferred show_window variant for verify4. Disk: /dev/disk3s1   460Gi   337Gi    97Gi    78%    2.5M  1.0G    0%   /System/Volumes/Data

16:53 · HK WINSHOW verify3/4 · report written. Deferred show restores swapchain but marker is not consumed by WindowPosChanged, so activation/render gate remains closed. Disk: /dev/disk3s1   460Gi   337Gi    97Gi    78%    2.5M  1.0G    0%   /System/Volumes/Data
17:09 · HK WINSHOW queue activation deployed · winemac.so SHA recorded · next bounded verify5
17:16 · HK WINSHOW verify5 · current-thread queue post not consumed; swapchain OK, activation missing · next retarget show/activate off DXGI thread
17:18 · HK WINSHOW queue2 · window-owned queue post deployed · next bounded verify5b
17:26 · HK WINSHOW async · Cocoa-main async show deployed · next bounded verify6
17:34 · HK WINSHOW delayed async · deployed delayed Cocoa-main show · next bounded verify7
17:40 · HK WINSHOW verify7 · orderBelow blocks in Cocoa main callback; swapchain OK, activation missing · next split focus/order path
17:41 · HK WINSHOW direct-orderFront · deployed direct orderFront+focus path · next bounded verify8
17:47 · HK WINSHOW verify8 · direct orderFront ok, makeFocused blocks · next post Wine focus event without makeKeyWindow
17:48 · HK WINSHOW windowGotFocus · deployed nonblocking focus event path · next bounded verify9
17:55 · HK WINSHOW direct WM_ACTIVATE · deployed direct guest activation post · next bounded verify10
18:02 · HK WINSHOW verify5 report · activation queue/direct paths failed; orderBelow/makeFocused blocking isolated · next render-consumer probe after swapchain
18:09 · HK post-swapchain consumer probe · report written; current lane before GetBuffer, rung13 before Present1 on 0xe0 second signal · next scoped PE-layer phase gate probe
2026-07-07 18:37 · HK unity phase probe: H1 refuted; swapchain +148.595s, phase65=0, signal_plain@0xa14420 hit=5, GetBuffer/RTV/Present=0. Next target downstream signal/event consumer after signal_plain; note lane runner used ntdll-37b849 cache root despite e793 deploy.
2026-07-07 20:21 · HK post-signal probe: final run laneA-post-signal-probe3-try1-201420, ntdll SHA 42e21a22/root ntdll-42e21a22c3d7bcaf. signal_plain@0xa14420 armed; SetEvent/NtSetEvent=0, is_e0 waits=0, GetBuffer/RTV/Present1=0. Triage POST_SWAPCHAIN_WORKER_THREAD_DEATH at ntdll+0x714fd MEMORY_FAULT. Next: disasm/probe UnityPlayer+0xa14420..0xa144a0 and callee 0xa02780, plus root-cause ntdll+0x714fd.
20:35 · HK SIGNAL-SETVENT-GAP-CAUSE · wrote report; A=SetEvent import exists but probe misses/indirect dispatch unresolved, B=Galaxy e06d7363 unwind fault at RtlVirtualUnwind2 with Context.Rsp=0x202 · no fixes/runs
20:35 · cleanup check · run.log 516K, /Users free 96Gi; no cleanup needed after read-only diagnosis · next decision: Unity+0xa14492 probe vs EH unwind probe
20:58 · HK SetEvent/EH probes patched · call-site sample around Unity+0xa14492 and e06d7363/RtlVirtualUnwind2 context sample added; default-off · next build/deploy/run
20:58 · HK probes build/deploy · ntdll.so SHA=7fb6dd0aaec4ba149f4393bd019d16cb7b49ad0988d1f54956cc793cf4e37a38 deployed; probes default-off until env · next laneA run
21:07 · HK setvent-eh-probe try1 invalid · no swapchain/signal/EH, cold cache root ntdll-7fb6dd0aaec4ba14; WINSHOW-off not runnable without touching Patch H · next warm rerun
21:19 · HK EH probe tightened · fixed probe overhead: RtlVirtualUnwind2 sampler armed only after e06d7363 delivery; previous probe runs invalid/no swapchain · next rebuild
21:20 · HK probes tightened build/deploy · ntdll.so SHA=b93fc9e9b0f3849534ad1349519647de3a48183fdaddc471a09c500d208ce76e deployed; EH sampler arm-only · next final probe run
21:33 · HK SETVENT-EH report · A resolved: SetEvent(0xe0) executes and returns success; B not reproduced in valid run; GetBuffer regression now handle/consumer mapping, not missing SetEvent · cleanup next
21:33 · cleanup done · invalid probe dirs removed, valid tight run retained; disk free=97Gi; translation cache/build tree untouched · next handle-lifecycle map 0xe0/0x1d0/0x1e0
21:39 · HK handle lifecycle probe patched · default-off import-boundary probe for create/set/wait handle map; no behavior change · next build/deploy/run
21:39 · HK handle lifecycle build/deploy · ntdll.so SHA=fa9975ce67fb773c93cbcec37231cbe79180e0050eda3902e82aff7903371626 deployed; cache root will change, no cache deletion · next run
21:47 · HK handle lifecycle probe extended · 0x1d0 appears not event-created; added thread-handle create mapping · next rebuild/deploy/run2
21:47 · HK handle lifecycle2 build/deploy · ntdll.so SHA=d75b7a5a94aad93fdb766edbac5f97da9f80ad5ef58dee4683b1c70889567291 deployed with CreateThread mapping · next run
21:55 · HK handle lifecycle · report written; 0xe0 signaled TRUE, visible 0x1d0 waits are success-returning and separate; no fix applied
22:19 · HK GetBuffer path · rung12 producer identified: Unity backbuffer block 0x90bb70..0x90c330 / GetBuffer call 0x90bc6b; current stops before block after factory association/signal_plain; no fix applied
22:33 · HK backbuffer gate · BACKBUFFER-GATE-PROBE-RESULT.md: current enters 0x90bb00..0x90c330, count33c=1 swapchain!=0, hits GetBuffer/RTV return sites; blocker moved downstream of RTV setup
23:08 · HK post-RTV · POST-RTV-PRESENT-GATE-RESULT.md: current reaches GetBuffer/RTV/OMSet(1) success; no Present1; next target post-signal present scheduler

## 2026-07-07 23:25 HK Present gate probe
- Run: reports/phase4-hollow-knight/laneA-present-gate-probe-try1-231557
- Result: OMSet(1) reached with rax=0; real Present1 not reached; runtime-fail/UNSUPPORTED/e06d/run-exit all zero.
- Rung12 also had Present1=0, so this is legacy post-RTV scheduling blocker, not regression.
- Report: reports/phase4-hollow-knight/PRESENT-GATE-PROBE-RESULT.md

## 2026-07-07 23:45 HK Present scheduling cause
- Run: reports/phase4-hollow-knight/laneA-present-scheduling-probe-try1-233459
- Result: CreateSwapChain+OMSet reached, real Present1=0, runtime-fail=0.
- Root cause narrowed to Unity scheduler gate: [0x181f31a08] non-null but [object+0x13c]==0 and [object+0x138]==0, so branch to submit/present path is not taken.
- Report: reports/phase4-hollow-knight/PRESENT-SCHEDULING-CAUSE.md

## 2026-07-08 03:35 HK Present-ready watch
- Run: reports/phase4-hollow-knight/laneA-present-ready-watch-try1-032748
- Result: [UnityPlayer+0x1f31a08]=0x34000d0f0; +0x138=0 and +0x13c=0 throughout; no event=change; 15 gate hits all skip_ready138_zero.
- Late residual: e06d7363 -> ntdll+0x714fd MEMORY_FAULT at +324s, after present-ready evidence.
- Report: reports/phase4-hollow-knight/PRESENT-READY-WATCH-RESULT.md

## 2026-07-08 03:46 HK Focus-present chain
- Run: reports/phase4-hollow-knight/laneA-focus-present-probe-try1-033918
- Result: broadened watch saw object install at UnityPlayer+0x15a4d2 with +0x138/+0x13c zero; diagnostic force set +0x138=1 and branch became submit_ready138_nonzero, but real swapchain/GetBuffer/Present1 still did not appear.
- Verdict: focus chain remains unproven; ready138 alone is not sufficient.
- Report: reports/phase4-hollow-knight/FOCUS-PRESENT-CHAIN-RESULT.md
04:12 · HK isFocused/companion probe · adding env-gated scheduler field snapshot probe; no force path · next build/run
04:15 · HK isFocused/companion probe · default-off bounded probe patched · building ntdll.so
04:16 · HK isFocused/companion probe · deployed ntdll sha=f379f949ebac2ea364fe85f8b6f268a47eaaa3cd11c026ade97f56af8980146b · next run
04:16 · HK isFocused/companion probe · starting 300s run · env gates bounded, no UI_INPUT
04:23 · HK isFocused/companion probe · try1 cold/no real swapchain; object fields stable, no failures · rerun warm
04:31 · HK isFocused/companion probe · report written; no real swapchain in reruns, no runtime failures · cleanup next
04:32 · HK isFocused/companion probe · cleanup done; report complete; next static setter/vtable ownership if requested
06:46 · HK byte138 writer search · adding MACRUNNER_HB_STORE_WATCH_0x138 diagnostic, no force writes · next build/run
06:47 · HK byte138 writer search · store-watch patched default-off · building ntdll.so
06:48 · HK byte138 writer search · deployed ntdll sha=dffc633549e59cebaf7e8c3857c22fa11b7b064f0aed8084ff500707e4b11a42 · next run
06:48 · HK byte138 writer search · starting 300s run, bounded store-watch only · no UI_INPUT
06:56 · HK byte138 writer search · report written; no runtime failures; next caller-xref of 0xaebe0/0x10ef9d if continuing
06:56 · HK byte138 writer search · cleanup done; report complete; no cache deletion/no make clean
08:09 · HK setter caller xref · starting static caller/xref analysis, no code changes yet · next xref pass
08:16 · HK setter caller xref · byte+0x138 setters identified as table-dispatched (0xaebe0, 0x10ef80/0x10ef9d); no direct call xrefs; missing upstream dispatcher invocation, no fix applied.
08:31 · HK descriptor dispatch · MACRUNNER_HB_DESCRIPTOR_DISPATCH_PROBE showed no execution of descriptor-reader band or setter targets after swapchain; 0x1a53de0 path likely metadata, not live present scheduler.
2026-07-08 09:40 · HK: PRESENT-GATE-STATE-MACHINE-RESULT.md written. Live present scheduler gate is UnityPlayer+0xa15d80, called from 0x7d637a; parent consumes AL at 0x7d637f. Current object 0x34000d0f0 has byte+0x138=0, dword+0x13c=0, qword+0x138=0x100, so submit path 0xa15e2a is skipped. No fix applied; next probe should target parent state-machine 0x7d62ec..0x7d639c.
2026-07-08 09:58 · HK EH RFLAGS: implemented MACRUNNER_HB_EH_REAL_RFLAGS materialization before semantic RtlRaiseException and KiUser frame construction. Built/deployed ntdll.so SHA 3dbc107fe24e889e6d3c2973d90a1dccd1d7a8162a8dc5d76ff497a104d51217. Verify attempts invalid: services/rpcss/HK killed with rc=137 at +23..24s before HB runtime/cache; same result with EH_REAL_RFLAGS disabled, so no conclusion on present gate. Report: reports/phase4-hollow-knight/EH-RFLAGS-PRESENT-RESULT.md. Invalid run dirs removed; /tmp/macrunner-* 0; disk 78Gi free.
11:57 · HK EH #3-B · replaced tactical RtlRaiseException trampoline with HB-owned x64-domain dispatch loop over AMD64_CONTEXT · build next
11:58 · HK EH #3-B build/deploy · ntdll.so SHA d4085265053a91898375e29732c471d6320d09faad5e9ce860a427a4b6254cd5; rung13 floor restored; drift=only ntdll.so · verify next
12:08 · HK EH #3-B verify · report reports/phase4-hollow-knight/EH-X64DOMAIN-FULL3B-RESULT.md; partial: x64-domain loop owns e06d7363 but returns c0000144 after 128 frames; old 0x714fd memory-fault absent; Present1 unchanged · next bounded frame trace
12:23 · HK EH frame trace patch · bounded #3-B per-frame log: pc/module/rva/RtlIsEcCode/lookup/unwind/frame/handler/new RipRsp · build next
12:24 · HK EH frame trace build/deploy · ntdll trace SHA 40a4a08e95b603f614d0d2645e4cff2a1afbf253cbe081de6bfae0bdb2a7a312; rung13 floor drift=only ntdll.so · verify next
12:32 · HK EH frame trace verify1 · no Galaxy EH reproduced; swapchain +154.779s, Present1=0; need longer bounded trace for handler=0 cause · running 900s
12:50 · HK EH frame trace verdict · report EH-X64FRAME-TRACE-RESULT.md; #3-B loop works, but lookup misses .pdata (function=0 128/128), causing leaf-walk junk and c0000144; next fix HB-local AMD64 pdata lookup · Present1 unchanged
2026-07-08 13:05 · HK EH #3-B pdata lookup: macrunner_hb x64 dispatch now uses HB-local AMD64 .pdata lookup; build/verify pending.
2026-07-08 13:06 · HK EH pdata build: sha e91a302b863512e498aba08a3f920b6d3020f518897600def801ee1892bf9612; rung-13 drift exactly ntdll.so.
2026-07-08 13:23 · HK EH pdata verify PARTIAL: .pdata lookup fixed; Galaxy handlers found; x64 language-handler call exits non-application-target, e06d still c0000144; no commit yet.
2026-07-08 13:31 · HK EH handler target: diagnosed 0x87fffcb7ea0 as aarch64 ntdll!RtlUnwindEx reached from Galaxy x64 handler; reject is x64 target classifier, fix candidate is scoped x64 EH import retarget to x64 ntdll!RtlUnwindEx.
2026-07-08 15:21 · HK EH bridge patch: scoped retarget from native ntdll!RtlUnwindEx to x64 ntdll!RtlUnwindEx added for x64-eh-language-handler.
2026-07-08 15:22 · HK EH bridge build: sha c33b1487755def9442c98bb118c3dcb3f29a551019c345ee94ea9723a0eec13c; rung-13 drift exactly ntdll.so.
2026-07-08 15:39 · HK EH dispatcher bridge: scoped x64-eh-language-handler native unix/syscall dispatcher bridge added after RtlUnwindEx retarget.
2026-07-08 15:40 · HK EH bridge2 build: sha 7e317d007a240e3940d59468da5334adfd69287fb2fb3a3b51a850816f4797bf; drift exactly ntdll.so.
2026-07-08 15:57 · HK EH bridge2 verify: valid rc=143, no run_exit/runtime_fail/c0000144, e06d not observed, swapchain +144.638, GetBuffer/RTV 0; no commit because clean point not proven.
2026-07-08 16:04 · HK EH chain bank: saved current macrunner_hb EH diff to reports/phase4-hollow-knight/HK-EH-CHAIN-BANK.patch; switching to present-pump.
2026-07-08 16:07 · HK present pump: restored macrunner_hb.c to HEAD after banking EH diff; applying pump-only change next.
2026-07-08 16:08 · HK present-pump patch: env-gated MACRUNNER_HB_PRESENT_PUMP for tid 0x148 handles 0xe0/0xec before WFSO/WFMO.
2026-07-08 16:10 · HK present-pump build: sha 95a879742c03e318821949d7f12d9d4ce961f2346de9c67f32ad4b9f32ab1b5a; clean rung-13 dist drift exactly ntdll.so.

2026-07-08 17:04 · HK present-pump verify · pump fires on 0xe0 and EH snapshot suppresses crash, but Present1 remains 0; no pixel claim · next: post-pump present scheduler gate

## 2026-07-10 — HK native delay-import boundary

- Exact root: after the validated Galaxy unwind/continue bridge, native sechost delay thunk address `+0x34210` belongs to the adjacent ARM64X delay-IAT view while the live AMD64 descriptor points at the sibling table starting `+0x34258`. `LdrResolveDelayLoadedAPI` rejected the native slot before `NdrClientCall2` lookup and returned NULL.
- Metadata proof: PE32+ load config stores `CHPEMetadataPointer` at `+0xc8`; ARM64EC V2 metadata stores `AuxiliaryDelayloadIAT`/`Copy` at `+0x50/+0x54`. Built hybrid sechost reports both fields and a terminated 9-entry rpcrt4 family with `0x50` table span.
- Fix/test state: generic bidirectional sibling selection is implemented in `ntdll/loader.c`, guarded by metadata Version >= 2 and full table/count/terminator/address validation. Focused winedump, Unix ntdll, PE ntdll builds and four regressions pass. Deployed arm64ec-spike Unix/PE SHAs match build: `c7b02ab0...` / `95356460...`.
- Runtime gate: focused SCM trigger is next, followed by canonical bounded HK only if the process preflight is clear. At 10:55 an independent bounded ABZU run was active; it is not touched and no parallel Wine/HK run is started.
- Acceptance remains real `Present1 > 0` followed by `pixel-truth-gate.sh` nonblank onscreen PASS; no pixel claim before both.

## Heartbeat 2026-07-10 — ARM64X delay-IAT focused PASS; Present creator pthread disappears

- Focused SCM boundary: PASS for the deployed loader fix. The resolver selected sechost's adjacent native delay-IAT view and resolved `rpcrt4!NdrClientCall2` plus three sibling imports with no invalid/fail/PC0. The fixture later hit an independent rpcrt4 JIT memory fault, so it is not an end-to-end SCM pass.
- Canonical 440s HK gate: valid rung 11, swapchain at +164s, no regression/fault/PC0, but Galaxy/sechost did not execute; therefore HK validation of the delay fix is not claimed. Real `Present1`, GetBuffer, and pixel truth remain zero.
- Follow gate: real OMSet-return armed at UnityPlayer+0x9054ff; 128 blocks traversed the WM_IME_COMPOSITION callback through +0x7d6364 -> +0xa15d80..+0xa15e75, then stopped. Native sample shows creator native tid 26719711 absent while the process main CFRunLoop and 52 workers remain; `present-follow-exit` and runtime-fail are both absent.
- Next evidence gate: trace-only/default-off lifecycle lines are deployed in Wine `thread.c` for abort/NtTerminateThread/exit/pthread-exit. Do not re-run pump, wait observer, or force Present. One bounded lifecycle run must identify whether tid 0x24 exits normally, is terminated, aborts, or bypasses all Wine termination paths.
- Deployed SHAs: Unix `5fc46eb6476d7cdeaf481c482aef7c278ca1e2889dc66e07ff602fc06edeaa20`; PE `95356460bf1e40180f420fb98e4bd8456781fc9b2c6fb71c8966c8ef59c9cd9c`.
- Acceptance remains natural real `Present1 > 0`, then nonblank/colorful pixel-truth artifact. No pixel claim.

## Correction 2026-07-10 — creator thread is live; item-ready gate is the blocker

- **DISPROVED/NONDETERMINISTIC:** the previous run's absent native creator thread is not a stable cause. In `laneA-present-thread-lifecycle-20260710-try1-133352`, all 437 follow records belong to `tid=0x24` / native `0x1982adb`, advancing through `seq=20,250,624` at +159.681s after OMSet. Lifecycle records total 19 but none name `0x24`; timeout cleanup names only `0x38/0x3c/0x34` at +468.633s.
- Execution is active, not a fixed-PC spin: 309 sparse samples = Mono 299 (96.76%), Unity 9 (2.91%), unknown 1; ~126,860 guest blocks/s and 6.59M steps/s. The generic `SILENT_SPIN_NO_MARKERS` classifier is overridden by this direct evidence.
- **Earliest stable gate:** current-run samples hit `UnityPlayer+0x587295` twice with identical `RAX=1`, `RSI=0`. Disassembly: `+0x587273` loads `ESI=[item+0x40]`; vfunc60 returns in AL; `+0x5872dc` compares `ESI` to 1 and branches to failure `+0x5874d8`. The vfunc succeeds but the render item is not ready, so submit/Present is skipped.
- The same window hits `+0x584105/+0x584131` with sync `0x320d610d8`, 1ms timeout, caller `+0x5878b0`, matching the render producer/consumer queue—not a DXMT Present stall.
- Exact run counts: swapchain=1, follow=437, follow-present-candidate=0, lifecycle=19 (creator=0), runtime-fail=0, real Present1=0, pixel=0, delay-IAT/NdrClientCall2=0.
- No semantic lifecycle patch. If further evidence is requested, the sole next probe is existing throttled `MACRUNNER_HB_TRACE_RENDER_TIME_ALIGN=1` by itself, to correlate producer `+0x586da3` publication with consumer `+0x5872d8/+0x5872dc` after-ready revisits. No rerun yet.
- Full result: `reports/phase4-hollow-knight/PRESENT-THREAD-LIFECYCLE-RESULT.md`.

## Correction 2026-07-10 — HK PreloadManager load completes; gate moves downstream

- Exactly one 440s bounded run used only `MACRUNNER_HB_TRACE_RENDER_TIME_ALIGN=1`; optional enrichment and all competing/forcing probes were off. Run: `laneA-producer-vfunc58-time-align-20260710-try1-145409`, bounded `rc=143`.
- Producer tid `0x8000` promoted item `0x34008a310` at +258.718s, returned from vfunc58 at `0x586d68` at +270.928s, then changed the same item's `+0x40` from 0 to 1 and latched it. Consumer tid `0x3000` observed the identical `active0=r14=item` with `item+0x40=1` within 113us and again within 213us.
- **A/B/C/D/E result: none of the failure branches.** A is contradicted by producer entry/return, B by the 0→1 publication, C by return, D by identical item identity, and E by consumer read=1. Literal `0x586d65=0` and `0x586da3=0` are exact-RVA trace gaps; adjacent block before/after observations prove the semantic events.
- Exact TIME_ALIGN total=186: consumer=134, producer=16, wait=36. Exact gates: `586d65=0`, `586d68=2`, `586da3=0`, `587273/5872dc/5874cc/5874d8=0`. Runtime fault/abort-thread/pthread-exit=0; real Present1=0; pixel=0.
- Unity emitted `UnloadTime: 103.485000 ms` at +271.033s. The PreloadManager non-completion hypothesis is disproved for this run; no lifecycle/coherence/opcode/timeout semantic patch is justified.
- Earliest unresolved boundary is post-ready consumer/main-loop continuation. If authorized, next evidence is one default-off bounded follow armed on the same-item ready consumer observation; no forcing, timeout change, DXMT/Metal work, or pixel claim.
- Full result: `reports/phase4-hollow-knight/PRODUCER-VFUNC58-TIME-ALIGN-RESULT.md`.

## Update 2026-07-10 — HK post-ready consumer follow and terminal RPC root

- The default-off, read-only `MACRUNNER_HB_POST_READY_CONSUMER_FOLLOW` arms only after `consumer-after-active-peek` observes the same latched manager/item with `item+0x40=1`. It is one-shot with 64 sample lines and hard 8M-block/60s bounds; TIME_ALIGN and all competing/forcing probes were off.
- Sole run `laneA-post-ready-consumer-follow-20260710-try1-152423`: arm=1 at +271.363s on tid `0x24`/native `0x19adf6e`, item `0x34008a310`, ready age 145us; samples=64 (Unity 43, Mono 20, combase 1); stop=1 at +60.113s / 7,766,016 blocks. First resumed PC is Unity+`0x587289`, then Mono by +3.332ms. No consumer-tid render import.
- **First downstream class: POST_READY_MANAGED_INTEGRATION.** Control is active and varied; no conclusion is drawn from duration alone. Present1=0 and pixel=0.
- Separate tid `0x124` later ends the run at `rpcrt4!RpcAssoc_Alloc+0x75`: HB's InitializeCriticalSectionEx fastpath ignores `flags` and always sets `DebugInfo=-1`, while rpcrt4 requested `FORCE_DEBUG_INFO` then writes `DebugInfo->Spare[0]`, producing address `0x27`. Class: `CRITICAL_SECTION_EX_FORCE_DEBUG_INFO_IGNORED`, not JIT STORE failure.
- No semantic critical-section patch in this probe-only task. Next authorized work must audit/fix the complete critical-section initialization fastpath family with sibling tests before another HK run.
- Pre-change snapshot: `reports/phase4-hollow-knight/POST-READY-CONSUMER-FOLLOW-20260710-NOT_GOLDEN/` (explicitly NOT_GOLDEN, no tag/log/cache copies).
- Full result: `reports/phase4-hollow-knight/POST-READY-CONSUMER-FOLLOW-RESULT.md`.

## 2026-07-10 16:41 — HK critical-section FORCE_DEBUG family

- Implemented the proven `CRITICAL_SECTION_EX_FORCE_DEBUG_INFO_IGNORED` family fix only in the public kernel32/kernelbase semantic path. FORCE now receives a checked guest-visible `RTL_CRITICAL_SECTION_DEBUG` with canonical back-pointer/list/counters; default/NO_DEBUG retain the sentinel; invalid/conflicting public flags fail before mutation; delete releases only exact fastpath-owned state. Direct `Rtl*` siblings remain on Wine's native implementation.
- Focused x64 public/native-wrapper fixture: first run `20 passed, 2 failed`; both failures were a test oracle using `GetSystemInfo` instead of Wine's PEB processor policy. No semantic patch followed. Corrected comparison run: `22 passed, 0 failed`, exit 0, with valid FORCE objects and writable `Spare[0]` on tid `0x24`.
- Built/installed/signed current ntdll artifacts. Deployed SHAs: Unix `b8183a007bc3ce2e17cfe2257d36e271117618918c0ed0678bd2e16c17c5f87f`; ARM64X `91a44392e38b9e370b092f4e55622e350473aac61c932e74b7580343cfc079b3`; x86_64 PE `f661dcb682d07826ea4c27dab41f00d86ccd2ad7f4373996825a915f1d4dfdae`. Unix/ARM64X strict codesign verification passes.
- Required full HB floor blocks runtime: `444 passed, 28 failed` (16 size thresholds, 8 execution-result assertions, 2 guest32 map failures, 2 counter assertions), all outside this Wine-only family diff. Per brief, Hollow Knight attempts here = 0; latest canonical truth remains Present1=0/pixel=0. Runtime count after cleanup = 0; ABZU untouched.
- Classification: `FAMILY_FIX_BLOCKED_BEFORE_RUNTIME`. Report: `reports/phase4-hollow-knight/CRITICAL-SECTION-EX-FORCE-DEBUG-FAMILY-VERIFY-20260710.md`. Checkpoint: `CRITICAL-SECTION-EX-FORCE-DEBUG-FAMILY-20260710-NOT_GOLDEN/`, no tag/log/prefix/cache copies.

## Update 2026-07-10 — HB floor-28 provenance

- Final classification: `NONDETERMINISTIC_FLOOR`. The recorded current run was `444 passed, 28 failed`; the sole authorized identical-current rerun was `443/29`, retaining all original 28 assertions in order and adding `interp_x64_rep_movsq_icon_memcpy_forward`.
- An isolated detached clean checkpoint `2ddb605f` baseline was `443/26`: 24 exact test+assertion pairs match the original 28, four current failures are absent, and two clean-baseline failures are absent from the original current list. The temporary worktree was removed after preserving compact JSON evidence.
- Lane regression is disproved: current HB sources, object, runner, and library were built at 08:12–08:21, before the critical-section Wine lane's 16:10 edit, and the HB build graph has no dependency on that Wine file or fixture. Original-28 classes: 24 `IDENTICAL_PREEXISTING`, four `UNCLASSIFIED`, zero `LANE_REGRESSION`, zero `ENVIRONMENT_INFRA`.
- The two guest32 failures use unaligned `0x10100`/`0x10200` bases and are rejected as invalid arguments before mmap; they are not the separately logged errno-13 event.
- `FAMILY_FIX_BLOCKED_BEFORE_RUNTIME` remains. ABZU owns the next runtime slot; no further build/runner/Wine/game action is authorized here. Next coordinator decision is a separate deterministic HB-floor investigation before HK release.
- Canonical report: `reports/phase4-hollow-knight/HB-FLOOR-28-FAIL-PROVENANCE-20260710.md`.

## Update 2026-07-10 — conditional differential waiver is void

- The conditional runtime brief requires provenance verdict `PREEXISTING_IDENTICAL_FLOOR_DEBT` and all exact-set conditions. Proven verdict is `NONDETERMINISTIC_FLOOR`; four original-current failures remain unclassified, and baseline-only MOVDQU lacks nondeterminism evidence.
- Result: `WAIVER_CONDITIONS_NOT_MET`. The pass-count and lane-influence subconditions alone cannot activate the waiver. Absolute HB floor remains red; historical `FAMILY_FIX_BLOCKED_BEFORE_RUNTIME` is unchanged.
- ABZU still owns the runtime slot, but no later slot release can override the failed evidence gate. HK attempts=0; no build/test/deploy/Wine/game action; Present1=0 and pixel=0 remain canonical truth.
- Conditional result: `reports/phase4-hollow-knight/CRITICAL-SECTION-FAMILY-CANONICAL-RUNTIME-RESULT-20260710.md`.

## Update 2026-07-10 — critical-section causal-isolation comparison

- A separate causal waiver proved the prior/current ntdll relinks used the same HB archive `b4690115…`; ABZU V2 then recorded its sole `NO_HIT_CANONICAL_RUN`, cleanup, and explicit slot release. The absolute HB floor remains red/nondeterministic and the earlier exact-baseline waiver failure remains historical truth.
- Exactly one 540-second-bounded diagnostic HK attempt ran with existing FORCE-init, post-ready, dispatch/cache/hot-block, D3D/DXGI markers only. No ledger or forcing; no retry or extension.
- Run `laneA-cs-family-causal-isolation-diagnostic-only-20260710-try1-210930` exited rc 253 at +44.779s. FORCE marker=0 and the prior rpcrt4/0x27 boundary=0 because neither path was reached. A replacement `c0000005` repeated 22 times from +44.383s at `kernelbase+0x3a91c`, fault address `0x88`.
- D3D truth regressed to real_factory/device/swapchain=0; OMSet=0, Present1=0, pixel=0. Mono/post-ready/dispatch/hot samples=0, so managed-state classification is also signal-insufficient, but explicit replacement fault makes the final enum `CS_FAMILY_RUNTIME_REGRESSION`.
- Own prefix and duplicate overlay removed; zero scoped processes, 272 KiB compact evidence retained. No semantic patch or next-fault action. Canonical result: `reports/phase4-hollow-knight/CRITICAL-SECTION-FAMILY-CANONICAL-RUNTIME-RESULT-20260710.md`.

## Update 2026-07-10 — kernelbase+0x3a91c static regression triage

- Final static enum: `BINARY_ROUTE_DRIFT`; secondary corridor: `EXCEPTION_REENTRY_CORRIDOR_LOCALIZED`. The current PC maps exactly to native ARM64X kernelbase's inlined `wcstombs_dbcs` count-only loop: `ldrh w16,[x9,x16,lsl #1]`, where `x9=info->WideCharTable` and the pre-load `w16=*src`. Recorded EA is `0x88`.
- Exact table/source values are not proven because the immutable run did not capture `x9`, `x16`, `x0`, or codepage. NULL table plus WCHAR `0x44` is the leading equation solution, not a root claim.
- Earliest proven divergence is prior `skip reason=not-arm64x` versus current ARM64X locale dual-view mirror `delta=0x27d8`. Current deployed kernelbase SHA is `0dc7ba3afc6ca79e16448cd5f23f0f6a0319a399953b3ee4c4ff07085deb020b`; the exact prior kernelbase SHA was not preserved.
- All 22 native SEGVs share PC/LR/fault while SP drops exactly `0x1690` each time. Static signal setup redirects into the exception dispatcher, so this is nested exception-dispatch re-entry ending in callback-domain stack exhaustion, not immediate sigreturn retry or 22 normal calls.
- Public flags-zero critical init can precede the FORCE-only marker, but the prior successful run has the same public import marker and the codepage cache uses direct `RtlEnterCriticalSection`. FORCE/rpcrt4 remains unreached.
- No patch is justified. If later authorized, the sole next evidence is the report's one-shot default-off pre-load owner/view record; no optional enrichment first.
- Full report: `reports/phase4-hollow-knight/KERNELBASE-3A91C-REGRESSION-STATIC-TRIAGE-20260710.md`.

## Update 2026-07-10 — dist provenance before wcstombs

- The wcstombs owner/view implementation was stopped on supersession. Its interrupted source/header/fixture state is frozen only as `WCSTOMBS-OWNER-VIEW-PROBE-INTERRUPTED-20260710-NOT_GOLDEN/` (manifest, owned patch, checksums); it was never installed, deployed, or exercised in either game.
- Primary verdict: `UNEXPECTED_DIST_DRIFT_PROVEN`. Both comparison runs selected `dist-arm64ec-spike`, but the prior run explicitly skipped the non-ARM64X kernelbase while the current route is `coff-arm64x` with `.a64xrm`. Two broad installs refreshed all 4,701 regular dist files; current versus rung13 changes 2,334 of 4,705 paths, while all 76 NLS payloads are byte-identical.
- `a29dd3c6…` resolves only to the rung13 `aarch64-unix/ntdll.so`, never to a complete floor. The June 30/July 1 manifests cover only three ntdll artifacts; no capture-time manifest binds kernelbase plus all ntdll identities. `FULL_RESTORABLE_FLOOR=NO`.
- `CS_FAMILY_RUNTIME_CLOSURE_PROVEN=NO`: the +44-second current run never reached the prior +351-second rpcrt4 boundary. Mono discrimination, Present1, and pixels remain unproven.
- A future one-file side-by-side staged comparison is provenance-safe (`SAFE_TO_STAGE_SIDE_BY_SIDE=YES`) only in a new isolated copy and only after separate runtime authorization; the active dist must not be overwritten.
- Canonical audit: `reports/phase4-hollow-knight/HK-DIST-PROVENANCE-BEFORE-WCSTOMBS-20260710.md`.

## Update 2026-07-10 — pure-kernelbase route-isolation release

- Release preflight passed: checkpoint checksums, active/candidate route identities, 66.350-GiB free-space gate, 44/44 strict codesign checks, and two host-isolation samples. Only the explicitly permitted idle, childless PID 99890 orphan remained; its `_mr-run.cEzXmo` cwd identity was proven read-only.
- The exact fenced materialization recipe failed closed before stage creation or `COPYFILE_CLONE_FORCE`. Its stored awk predicate searches for a double-escaped `Type \\(Bundle\\)` and returns empty, while the same device's real `Type (Bundle)` value is `apfs`.
- Final enum: `STAGE_MATERIALIZATION_ABORTED`. Stage, recursive manifest, clone-helper temporary directory, and runtime run directory are absent. Active/candidate hashes remain `0dc7ba3a…020b` / `23b420d2…62a8f`.
- Per the release's no-rewrite/no-retry gate, runtime wrapper attempts=0 and HK attempts=0. All kernelbase-fault, ladder, Mono, DXGI/D3D, Present1, pixel, and rpcrt4 signals are unmeasured; no closure or acceptance inference is allowed.
- Compact result: `reports/phase4-hollow-knight/HK-PURE-KERNELBASE-ONE-DIAGNOSTIC-RUNTIME-20260710-NOT_GOLDEN/RESULT.md`.

## Update 2026-07-11 — pure-kernelbase temporary full-copy diagnostic

- The one authorized `rsync -a` full copy passed strict inventory gates: active/stage pre-payload inventories were byte-identical across 4,731 relative entries; post-payload path/type/symlink/mode sets remained exact and only the authorized kernelbase SHA changed (`0dc7ba3a…020b` → `23b420d2…62a8f`).
- Exactly one runtime attempt reached prefix sync but stopped at +20.870s on `wine: ntdll export __wine_unix_call_dispatcher_arm64ec not found`; `mr-run` exited 1 at +21.795s and the wrapper returned 1/`ALL_TRIES_FLAKED`.
- Final enum: `FULLCOPY_DIAGNOSTIC_NO_REACH`. Kernelbase fault/0x88, not-arm64x route, Mono growth/SCC, factory/device/swapchain/RTV, rpcrt4, Present/Present1, and pixels are all non-reach—not causal negatives. The pure-kernelbase hypothesis remains untested.
- Prefix, full-copy stage, and inventory scratch were deleted; active kernelbase is unchanged, disk recovered to 67.111 GiB, and only the permitted idle PID 99890 orphan remains.
- Result: `reports/phase4-hollow-knight/HK-PURE-KERNELBASE-FULLCOPY-DIAGNOSTIC-V4-20260711-NOT_GOLDEN/RESULT.md`.

## Update 2026-07-11 — V5 post-Mono static discriminator ready

- V5 remains `DIAGNOSTIC_ONLY`: the explicit x64 loader cleared V4, produced five `imach=8664` identities and reached Mono path/config at `+33.344s/+33.346s`, then gave no execution classification before exit 143 at `+571.022s`. Zero kernelbase/rpcrt4/growth/SCC/graphics markers are non-reach or unmeasured, not closure.
- Exact x64 Unity map: config emitter `0x72f887`, logger return `0x72f893`, local path handling through `0x72fa08`, and resolved `mono_set_dirs` call `0x72fa29`. Resolver RVA `0x7591ba..0x7591c6` binds literal `mono_set_dirs` to slot `0x1f50658`, which that call loads. `Begin MonoManager ReloadAssembly` belongs to a separate later function (`0x743540+`) and was not reached.
- Existing heartbeat/progress/lift/legacy-Mono probes cannot boundedly distinguish unique Mono translation growth from a repeating SCC. The new `MACRUNNER_HB_TRACE_POST_MONO_DISCRIMINATOR` is default-off, host-getenv/Q6-safe, atomic one-shot, module-AMD64 checked, fixed at 262,144 lookups/65,536 PCs, and max three success-path logs with zero semantic mutation.
- Static validation: 11/11 tests, Python syntax pass, owned whitespace pass; builds/runs/deploys=0. Future authority, if granted, is one run only and stops at first summary or the external bound; productive-growth, pre-Mono, repeating-SCC/retranslation, and inconclusive branches are defined in the checkpoint.
- Canonical checkpoint: `reports/phase4-hollow-knight/HK-POST-MONO-DISCRIMINATOR-STATIC-READY-20260711-NOT_GOLDEN/REPORT.md`. Stop state: `HK_POST_MONO_DISCRIMINATOR_STATIC_READY`; no Present/Present1/pixel claim.

## Update 2026-07-11 — post-Mono discriminator late-load hardening

- Opus audit SHA `bbd103d7…19333` reproduced with verdict `HK_POST_MONO_PROBE_READY_FOR_ONE_RUNTIME`, HIGH confidence/no blockers. Its only residual ambiguity was arm-time `mono_base=NULL` with a later Mono load.
- Hardening is confined to one state field and one armed-path retry block: retry at sample 1, then +4,096 while NULL; accept only AMD64; refresh `mono_size` before current-PC classification. Maximum is 64 post-arm retries inside the unchanged 262,144-sample window. No observer/log/cache/guest/timeout/runtime semantic changed.
- Mandatory precedence is unchanged: any `armed` line with `mono=0x0` is Branch E, never B, even if a later retry produces `mono-latched`. The retry preserves downstream evidence without permitting a false pre-Mono conclusion.
- Validation: existing 11/11 plus 4/4 new focused cases = 15/15; syntax, whitespace, and delta apply-check pass. Build/deploy/runtime/ABZU access=0.
- Canonical delta: `reports/phase4-hollow-knight/HK-POST-MONO-DISCRIMINATOR-HARDENED-STATIC-READY-20260711-NOT_GOLDEN/REPORT.md`. Stop state: `HK_POST_MONO_DISCRIMINATOR_HARDENED_STATIC_READY`; independent delta verification and separate runtime serialization remain required.

## Update 2026-07-11 — post-Mono one-runtime preflight route mismatch

- All cheap gates passed: 15/15 static tests, exact hardened identities, prior actual post-Mono attempts0, 67.230331 GiB free, no non-waived Wine/game/build process, and stable permitted PID99890 (`PPID/PGID=1/99890`, 0%, childless, `_mr-run.cEzXmo` cwd).
- Fail-closed route proof stopped before backup/build. The observer source `dlls/ntdll/unix/macrunner_hb.c` is compiled by the ARM64 Mach-O `dlls/ntdll/ntdll.so` target. Exact x64 PE target `dlls/ntdll/x86_64-windows/ntdll.dll` is up to date, does not compile that source, and has observer/gate strings `0/0`.
- Building/deploying only the authorized x64 PE cannot change the executing observer binary or prove host-getenv/schema presence; running would waste the sole attempt. No implicit substitution to `aarch64-unix/ntdll.so` was made.
- Final enum: `PREFLIGHT_ROUTE_MISMATCH`; build/deploy/runtime `0/0/0`, attempt unconsumed. Frozen matrix retains mandatory `armed mono=0x0` → E, never B.
- Result: `reports/phase4-hollow-knight/HK-POST-MONO-ONE-RUNTIME-PREFLIGHT-ROUTE-MISMATCH-20260711-NOT_GOLDEN/RESULT.md`. Resume requires explicit authority to build `build-arm64ec-spike/dlls/ntdll/ntdll.so`, deploy only to `dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`, and leave x64 PE ntdll untouched.

## Update 2026-07-11 — post-Mono sole runtime completed Branch A

- Corrected Unix binary scope passed: private backup exact; focused object/library only; diagnostic Unix ntdll `7a0bca64…f91` was Mach-O arm64, strict ad-hoc signed, contained gate/armed/latch/summary schemas, and was the sole dist drift. x64 PE `f661dc…dae` remained byte+metadata exact.
- The sole 540s-bounded attempt `laneA-hk-post-mono-one-diagnostic-20260711-try1-024553` exited rc253 naturally at +43.699s (triage +44.583s), so no summary-stop signal was needed. No retry/extension.
- Frozen verdict: Branch A. Observer armed/latch/summary `0/0/0`; Mono path/config `0/0`; no armed row, so the mono=0x0→E rule was not triggered. This run does not classify Mono growth or SCC behavior.
- Pre-emitter fault evidence: kernelbase base `0x87efe7e0000`, PC `0x87efe81a91c` = RVA `0x3a91c`, fault `0x88` repeated22 at +43.158..43.255s, followed by c00000fd stack exhaustion. rpcrt4+24945=0.
- Graphics truth: dxgi IAT/real factory `7/0`, d3d11 IAT/device `1/0`, swapchain/GetBuffer/RTV/OMSet/Present/Present1/pixel all0; non-reach only, no acceptance claim.
- Restoration exact: build inventory `4539980b…fdd28`, full dist `1a23c87a…5bab`, dist-other `cc349683…ad401`; restored Unix ntdll `b8183a…f87f`, x64 PE unchanged. Prefix/backup/overlay/diagnostic cache absent; host clean except waived PID99890.
- Final result: `reports/phase4-hollow-knight/HK-POST-MONO-ONE-DIAGNOSTIC-RUNTIME-20260711-NOT_GOLDEN/RESULT.md`. Stop state `HK_POST_MONO_ONE_DIAGNOSTIC_BRANCH_A_COMPLETE`.

## Update 2026-07-11 — Branch-A/V5 kernelbase static differential

- Final classification: `BINARY_SEMANTIC_DEFECT`, not timing or an opcode/unwind/import defect. Active ARM64X uses one `0x27d8` locale mirror delta, but all 57 linked native/EC pairs split exactly into 24 at `0x27e8` and 33 at `0x1b00`; none matches the configured delta.
- Exact causal collision: native `ansi_cpinfo` `0x60f8` copies to EC `0x3920` instead of `0x3910`; later native `geo_ids_count` `0x60e8` copies exactly onto EC `ansi_cpinfo`. Immutable `locale.nls` SHA `868e5d3a…e403115` has `ids_count=301`, forcing a nonzero header while the shifted copy places native `WideCharTable` into EC `DBCSOffsets`.
- Active/pure `wcstombs_codepage` have identical 654-instruction mnemonic sequences, identical fault block SHA `0f542001…15bdb`, identical unwind semantics, identical 429-name import sets, and zero family-range base relocations. Active CHPE redirections/DVRT contain no `.data` repair.
- The Branch-A diagnostic Unix ntdll/observer difference is explicitly retained as a runtime confounder; static symbol/data proof, not duration or zero observer rows, establishes the root.
- Design only: future family patch must use both proven delta groups (a one-line `27d8→27e8` is incomplete), with a 57-symbol post-link destination gate, synthetic guard test, pure no-copy test, then one separately authorized bounded owner/view HK gate. No source semantic edit/build/deploy/runtime/ABZU access occurred.
- Canonical report: `reports/phase4-hollow-knight/HK-BRANCH-A-V5-KERNELBASE-3A91C-STATIC-DIFFERENTIAL-20260711-NOT_GOLDEN.md`, SHA-256 `7f3be6ff0cb9cf978d6ac01037a71055beec0645fa3a0e51beff5b99bba5809c`.

## Update 2026-07-11 — ARM64X locale mirror family fix static-ready

- Implemented only the proven `locale.c` mirror family: helper delta is explicit; exact membership is 24 core/NLS/codepage objects at `0x27e8` and 33 `unix_cp`/registry-entry objects at `0x1b00`. All 57 appear once, original copy order and CHPE-zero guard remain, and the forbidden one-delta `0x27e8` shortcut was not used.
- Added `tests/test_arm64x_locale_mirror.py`: source membership/legacy-path checks, exact CPTABLEINFO pointer/sentinel synthetic copies, two registry siblings, pure zero-copy, and opt-in post-link symbol proof. Pre-link result 6 pass/1 expected skip; post-link result 7/7.
- Targeted ccache ARM64X kernelbase build passed. Final source/test/build DLL SHAs are `7f17f073…e3be`, `3b3f2cf6…a4ee`, `893c6359…58c84`; active dist kernelbase remains `0dc7ba3a…020b` and was not deployed.
- Inherited HB floor inputs remain exact (`runner 377453a8…160`, `lib b4690115…c7c`); result `442/30`. It is the known identical-binary `443/29` set plus the already-proven nondeterministic movdqu blit failure, and the union of the documented current/clean sets. No floor debt was changed or waived.
- Three-file `FORENSIC_CHECKPOINT / NOT_GOLDEN` is checksum-verified at `reports/phase4-hollow-knight/HK-ARM64X-LOCALE-MIRROR-FAMILY-FIX-FORENSIC-CHECKPOINT-20260711-NOT_GOLDEN/`; manifest SHA `559a1946…0185`, owned patch SHA `2131d50b…e20d7`, checksum-file SHA `ea2a3e9a…406d2`. Stop state: `HK_ARM64X_LOCALE_MIRROR_FAMILY_FIX_STATIC_READY`.
- No selected/dist mutation, Wine/game runtime, HK attempt, ABZU access, commit/tag, or Present/pixel/golden claim occurred.

## Update 2026-07-11 — locale mirror family one-runtime diagnostic

- Consumed the sole authorized canonical HK attempt from an exact full-copy stage. Pre-replacement inventories matched at 4,732 entries; the only staged delta was ARM64X `aarch64-windows/kernelbase.dll` `0dc7ba3a…020b -> 893c6359…8c84`. Stage route/`.a64xrm` and 44/44 launch-Mach-O strict codesign gates passed; active dist was never mutated.
- Run `laneA-locale-mirror-family-one-diagnostic-20260711-try1-040141` reached Mono path/config at `+38.860/+38.861s`, emitted two `0x27e8/0x1b00` locale family-sync groups (six rows), and had zero `kernelbase+0x3a91c fault=0x88`, `0xc00000fd`, and stack-overflow markers.
- The bounded `wcstombs` owner/view marker count was zero. Thus owner/index, executing native/EC `CPTABLEINFO`, `WideCharTable`, and `DBCSOffsets` were not observed: this is non-reach, not locale-fix closure. No rerun is authorized.
- Downstream remained non-reach: `rpcrt4+0x24945=0`, DXGI IAT/real factory `7/0`, D3D11 IAT/real device `1/0`, swapchain/Present/Present1/pixel `0/0/0/0`. No graphics or pixel acceptance follows.
- Attempts are 1/1. Scoped prefix, stage, overlay, pointer, and diagnostic scratch are absent; active inventory returned to `3a755615…ef32`, PID99890 is stable/childless/untouched, and host scoped runtime/build hits are zero. Sealed result: `reports/phase4-hollow-knight/HK-ARM64X-LOCALE-MIRROR-FAMILY-ONE-DIAGNOSTIC-RUNTIME-20260711-NOT_GOLDEN/`.

## Update 2026-07-11 — guest-PEB control observer static implementation

- HK now has a default-off static observation path for the finalized initial guest environment: executing PE `ntdll` hashes a bounded native/WOW64 PEB block before main imports, and Unix atomically publishes privacy-preserving per-record evidence only when explicit host controls are present.
- A strict C7 collector accepts exactly one complete shard and matches environment/view/Wine/ntdll/pre-main identities without using arrival order. The canonical ledger remains guest-PEB `UNKNOWN`; no publication enablement occurred.
- G0-G31 and the retained 78-test identity suite pass. Product/sanitizer build and real control/title observation remain required and were not authorized in this task.

## Update 2026-07-11 — guest-PEB build-control gate blocked before runtime

- Independent audit authorized a focused build and one control child only after all gates passed. Preflight was clean, and both observer translation units compiled to the expected arm64 Mach-O and amd64 COFF objects.
- The x86_64 PE ntdll link failed rc=2 because `winegcc` resolved the compiler basename to Homebrew GCC 15.2.0 instead of the repo llvm-mingw clang wrapper; GCC rejected `--no-default-config` and `-fms-hotpatch`. No retry was authorized.
- Sanitizers, retained post-build floors, and the control child were not entered. Attempts/shards remain zero, canonical guest-PEB remains UNKNOWN, cache/source/dist are unchanged, and final process/PID99890 proof is clean. Blocker: `BLOCKED_FOCUSED_X64_PE_LINK_TOOLCHAIN_RESOLUTION_GCC15_REJECTS_CLANG_FLAGS`.

## Update 2026-07-11 — guest-PEB control-name repair awaiting Opus audit

- The plain reproducer proved a SOURCE-boundary privacy defect: `L""`/`wchar_t` controls accessed as `WCHAR` with runtime `wcslen` never matched. The bounded repair now uses explicit 16-bit arrays and compile-time lengths only.
- Cross-build acceptance is identical and green across plain O0/O1/O2, ASan+UBSan, and TSan; G0-G31 is 32/32, retained identity is 78/78, and the focused production llvm-mingw compile/link passes with cache unchanged.
- Runtime remains unauthorized: no Wine, control child, title/game, deploy, ledger publication, or A/A. Control attempts remain zero. Stop state: `STATIC_BUILD_ONLY / NOT_GOLDEN`, pending independent Opus audit.

## Update 2026-07-13 — ABZU pre-D3D11 loader/heap lock inversion proven

- One 90-second `NOT_GOLDEN` `+sync` observer run (no build/source edit/retry) proved a two-thread deadlock: game main `0x00a4` owns the main-process-heap critical section and waits for `loader_section` owned by `0x00a8`; new thread `0x00a8` owns `loader_section` during ThreadInit and waits for the heap section owned by `0x00a4`.
- Both threads were parked in `NtWaitForAlertByThreadId -> __ulock_wait2` at age 58s. The wait addresses are the respective critical-section `LockSemaphore` fields (`section + 0x18`), not an event or device-completion object.
- `D3D11CreateDevice` was not entered: the sole D3D11 record is pre-main IAT resolution; probe entry/return/device-created and downstream DXMT/Metal remain zero. Obligated producer is `0x00a8`, not prior run-local `0x0068/0x00a0` phantoms.
- Evidence: `reports/research/checkpoints/abzu-game-diagnostic-rtlwait-producer-20260713-NOT_GOLDEN/ANALYSIS.md`. Next source question: which `0x00a4` path retains the heap critical section while triggering loader/thread initialization.

## Update 2026-07-13 — ABZU ABBA source-only lock-order audit

- Worker `0x00a8` reaches the common Wine `fls_alloc_data -> RtlAllocateHeap(GetProcessHeap())` while `loader_section` is held; MacRunner xtajit64 `ThreadInit` has already returned. TLS allocation and DLL thread attach can also allocate before loader-lock release, so moving one allocation or HB ThreadInit is not a complete correction.
- Main executes guest `CreateThread` from a static initializer nested under the HB `_initterm` semantic. MacRunner uniquely substitutes direct `NtCreateThreadEx` plus immediate resume for Wine's kernelbase CreateThread path. The proposed minimal correction is to yield same-process CreateThread to a proven native ARM64/ARM64EC builtin target, retaining the semantic only as fallback.
- The sealed data proves main owns the heap CS but does not expose its exact acquire caller; the visible guest allocation and Wine `RtlAllocateHeap` are balanced before CreateThread. Sufficiency is therefore not claimed until a later authorized observer run. Full audit: `reports/research/ABZU-ABBA-SOURCE-AUDIT-20260713.md`.

## Update 2026-07-13 — heap-CS observer attempt invalid before ABZU spawn

- The env-gated ARM64X PE observer built cleanly, but the one authorized attempt never spawned ABZU: it stopped in prefix service setup (`seed-com-rpcss-registry` then `regsvr32-actxprxy`), with zero heap-CS observer, blocked-by, game-entry, and D3D11 boundary records.
- The launcher's BSD-awk serialization preflight was malformed and continued because the diagnostic script lacked `set -e`; a separate Hollow Knight run became live during the attempt. The result is fail-closed `INVALID_PRE_GAME_SERIALIZATION_COLLISION`, not evidence for either CreateThread timing or mixed-view CS imbalance.
- Only the owned `_mr-run.dz9hr2` prefix was stopped; the unrelated title was untouched, and the overlay ntdll was restored byte-exact. No retry is permitted without new authorization. Result: `reports/dualdata/ABZU-ABBA-HEAP-CS-OBSERVER-RESULT.md`.

## Update 2026-07-13 — Fix A CS native-forward implemented; ABZU verify FAIL before ABBA

- Added default-off `MACRUNNER_HB_CS_FORWARD_NTDLL`: only Enter/Leave/TryEnterCriticalSection bypass the HB semantic and fall through to the existing native PE target. Kernel32/kernelbase specs forward all three directly to the corresponding ntdll Rtl implementation; Init/Delete/SpinCount remain unchanged. Static family guard passes all three siblings.
- Main `ntdll.so` build passed (only `unix/macrunner_hb.o` rebuilt), SHA `3d7d65c1…`; 13 pre-existing warnings, zero fix-line diagnostics, no install-skipped.
- The sole 300s-capped ABZU attempt exited after 55s with terminal `c000007b` at `pc=0x7ffd020044c`, last translated block in `winemetal.dll`. CS-forward hits=0 and the original ABBA boundary was not reached, so blocked-by=0 is non-classifying. Verdict: `FAIL_PRE_ABBA_C000007B_CS_FORWARD_NOT_EXERCISED`; no retry or unrelated fix. Report: `reports/dualdata/ABZU-CS-FORWARD-FIX-A-VERIFY.md`.
- Final integrity/cleanup: static family guard, checkpoint checksums, and targeted whitespace check pass (apart from the pre-existing `gitlab-generated` attribute warning). The ABZU overlay was restored byte-identically to SHA `14d3563d…`; owned disposable prefix and `/tmp/macrunner-*` are absent, no `run.log` exceeds 5 GiB, translation cache is preserved, and 58 GiB remains free.

## Update 2026-07-13 — 55s `winemetal` crash is not Fix A

- The single same-binary OFF control reproduced treatment exactly: 65 child-env variables differed only in `MACRUNNER_HB_CS_FORWARD_NTDLL=1→0`; both arms ended at `pc=0x7ffd020044c`, last block `winemetal+0x46b0`, `c000007b/non-application-target`, before ABBA. Fix A is not causal.
- The loaded early-overlay winemetal SHA `7aa914d6…` has `+0x46b0 = ___chkstk_ms` IAT stub, called from MinGW `_pei386_runtime_relocator`; `0x7ffd020044c` is an RW ARM64EC thunk, not winemetal code. `c000007b` is HB's own unclassified-target status, not a bad-PE loader error.
- Source delta is decisive: ABZU Unix HB identifies the same `ff 25` IAT jump as `ntdll.dll!___chkstk_ms registered=0` and performs the correct x64 no-op; Main lacks that dynamic-IAT recovery and falls through to `non-application-target`. Fix candidate is to merge that class-level recovery/registration path into Main, not exclude winemetal from CS forwarding. Report: `reports/dualdata/ABZU-CS-FORWARD-55S-CRASH-CAUSE.md`.
- Cleanup: one control only, no rebuild; overlay restored SHA `14d3563d…`, prefix/tmp/owned processes absent, no >5 GiB log, cache retained, 58 GiB free.

## Update 2026-07-13 — dynamic-IAT family merged; Fix A runtime PARTIAL

- Main now registers the complete ntdll x64 stack-probe family (`___chkstk_ms`, `__chkstk_ms`, `__chkstk`) as preserve-RAX semantic no-ops. A bounded PE/IAT `ff 25` recovery remains as defense for late bindings; no winemetal special case was added. Both family source guards pass.
- A coherent Main pair was built and deployed together: Unix ntdll `af774f21…`, ARM64X PE ntdll `a62ea85a…`; successful build log `7195ffba…`, no errors or install-skipped.
- Sole 300s ABZU verify verdict: `PARTIAL`. CS-forward is exercised (`>=64` logged hits), reciprocal heap/loader `blocked by` is zero, seven ThreadInit calls succeed, and `c000007b/c0000005/pc=0x60` are zero. The old ABBA and 55s crash are absent.
- Dynamic-IAT/stack-probe runtime hits are zero because graphics execution never begins: CreateDXGIFactory1 and D3D11CreateDevice appear only during IAT registration, with no HRESULT; EnumAdapters1/CopyAllDevices/swapchain/render are absent. Main `0x00a4` now signals event `0x98` and waits indefinitely on `0x94`, while `0x00a8` polls `0xa0`.
- Report: `reports/dualdata/ABZU-DYNAMIC-IAT-CS-FORWARD-VERIFY.md`. One attempt, both overlay halves restored byte-identically, cache inventory unchanged, tmp/prefix/>5GiB cleanup zero, 58 GiB free.
16:34 · GFXDEVICE-STALL-CAUSE complete · proven terminal boundary is post-MonoManager synchronization quiescence, not D3D11 creation; b818 source hunk not isolated because historical +142 oracle is 5bfc (not 3dbc) and old binary is absent · no rebuild/source changes; invalid sample removed, /tmp=0, >5GiB logs=0, cache preserved, 58GiB free

## Update 2026-07-13 — event `0x94/0xa0` stall is a worker-idle phantom

- The sole 120s `NOT_GOLDEN` lifecycle observer corrects the thread identity: game TID `0x009c` is the creator/main thread; `0x00a4` is a TaskGraph worker, `0x00a8` is a `PoolThread`, and `0x00c8` is another idle worker. All observed create-suspended/resume/ThreadInit sequences succeed.
- Event `0x98` is the reusable thread-start handshake. Each new worker signals it and main `0x009c` wakes successfully, including `0xa4`, `0xa8`, and `0xc8`; this proves the shared event handle and Set/Wait protocol are coherent.
- Event `0x94` is the `0xa4` TaskGraph idle/work event; `0xa0` is the auto-reset 10 ms pool-work event polled by `0xa8`; `0xc0` is `0xc8`'s private 50 ms idle event. None has an obligated signal while its queue is empty. The missing producer/thread-creation/event-duality theories are disproved, so no event-layer fix is justified.
- The true unresolved boundary is main `0x009c` after its final successful startup handshake; graphics remains registration-only. Report: `reports/dualdata/ABZU-EVENT-94-A0-STALL-CAUSE.md`. One observer attempt only; coherent overlay restored, cache unchanged, owned tmp/prefix/invalid/>5 GiB artifacts absent, 58 GiB free.

## Update 2026-07-13 — ABZU main 0x009c terminal boundary is ws2 unixlib lifecycle/view incoherence

- The sole 120s `MACRUNNER_HB_MAIN_009C_PROBE=1` run proves main completes its third `0x98` handshake and never enters another Wine wait. It executes 450 observed guest blocks, then the outer `thread` frame synchronously enters native `ntdll.so!__wine_unix_call_dispatcher` and never returns.
- The terminal guest module is uniquely x64 `ws2_32.dll`: `gethostname` issues Unix-call code 3 with `handle=0`. Native dispatcher dereferences `[handle + code*8]`, so the null module-local `__wine_unixlib_handle` traps inside the synchronous native bridge.
- Root cause is loader lifecycle/execution incoherence: most AMD64 Wine builtin DllMains are skipped on the assumption that exports route to native ARM64, but mixed-view routing later executes the guest ws2 body. Thus ws2 process attach never calls `__wine_init_unix_call`, while guest `.data` is consumed. This is the same class documented for winemetal; the existing winemetal-only exception is incomplete.
- Graphics is not reached: CDF1/D3D11 remain registration-only and the terminal operation is pre-RHI hostname/network initialization. Correct fix is capability/view-based coherence for every builtin (native export only, or same-view x64 lifecycle + Unix funcs), not a ws2 allowlist and not an event fix. Report: `reports/dualdata/ABZU-MAIN-009C-POST-HANDSHAKE-CAUSE.md`.
- Observer build/run safety: coherent Unix `c27cf057…` + PE `a62ea85a…`, build errors/install-skipped zero, overlay restored, cache byte inventory unchanged, no invalid/tmp/>5GiB cleanup required, 58 GiB free.

## Update 2026-07-13 — ws2 Unixlib null-handle boundary fixed; downstream JIT helper fault

- General AMD64 builtin DllMain was tested and rejected: ws2_32/dnsapi capabilities were nonzero, but arbitrary loader-time side effects stalled before game main. That candidate was removed immediately.
- New env-gated class boundary `MACRUNNER_HB_BUILTIN_UNIXLIB_RESOLVE=1` resolves a null guest Unix-call handle from the caller return PC through `MemoryWineUnixFuncs`; it contains no module allowlist and uses Wine's cached builtin Unix handle.
- Sole replacement verify hits ws2_32 with `status=0`, handle `0x1174840d0`; main returns from the synchronous call, continues event activity, and completes 12 more ThreadInit operations (19 total). Fix A records 64 hits; ABBA remains absent.
- Graphics is still registration-only. The next real blocker is a worker JIT helper `MEMORY_FAULT` at UE4 PC `0x1405bd441`/RVA `0x5bd441`, surfaced as one `c000007b`; `c0000005` and `pc=0x60` are zero. Report: `reports/dualdata/ABZU-WS232-UNIXLIB-CLASS-FIX-VERIFY.md`.
- Coherent pair restored, cache byte-identical, no cleanup candidates, 57 GiB free. Golden `e7cca2e3` untouched.
- 2026-07-13 22:52: rebuild paused to floor the proven ABBA-cleared + class-Unixlib coherent runtime. Source dist is the exact `abzu-builtin-unixlib-resolve-verify-20260713-NOT_GOLDEN` manifest dist; current build outputs still hash Unix `b60e43c3…` / PE `38841810…`. Next: ditto full dist, replace only its ntdll pair, preserve resolver/candidate-a-revert source patch, then byte-verify before JIT-fault work resumes.
- 2026-07-13 22:58: breakthrough floor complete at `artifacts/milestone-dist/abzu-abba-cleared-unixlib-resolve-BREAKTHROUGH-20260713/` (2.1 GiB). `MILESTONE-METADATA/SHA256SUMS` verifies 6/6, including deployed Unix `b60e43c3…` and PE `38841810…`; complete final loader/HB sources, guard and patch are colocated. Resolver gate present, rejected general-DllMain candidate absent. Free space remains 57 GiB; no rebuild occurred before the floor.

## Update 2026-07-13 — ABZU UE4 JIT fault is missing WIC COM registration

- Sole 120s NOT_GOLDEN observer hit: TID `0xd0`, block `0x1405bd441`, `special-read address=0 size=8`, `RAX=0x80040154`, `RCX=0`. Guest disassembly proves `CoCreateInstance(CLSID_WICImagingFactory2)` failed with `REGDB_E_CLASSNOTREG`, output remained NULL, and the unchecked dereference is `mov rax,[rcx]` at RVA `0x5bd471`.
- Not a JIT/opcode/chkstk/stack fault. Outer `c000007b` is HB's generic runtime-failure mapping; it is not an invalid PE status here. Worker entry is RVA `0x5c16e0`, caller returns at `0x5c179b`; path is WIC image initialization, pre-RHI.
- Prefix root: `mr-run` creates a new template-less prefix, skips `wineboot -u`, copies `windowscodecs.dll`, and registers only DXMT/RpcSs + actxprxy. DLL implementation exists but its COM class mapping does not. Proper fix is the generic coherent prefix lifecycle: initialize fresh prefixes or clone a dist-matched initialized template; do not ship a WIC/ABZU or JIT workaround.
- Fix-A/Unixlib state remains good: resolver hit 1, CS-forward 64, ThreadInit success 19, `blocked by` 0. Graphics remains IAT-only; real CDF1/D3D11/swapchain calls zero. `c0000005=0`, `c000007b=1`, `pc=0x60=0`.
- Runtime restored `14d3563d…` / `8e2a5691…`; cache inventories identical, no tmp/invalid/>5GiB cleanup needed, no owned process remains, 57 GiB free. Breakthrough floor remains 6/6. Report: `reports/dualdata/ABZU-UE4-JIT-FAULT-5BD441-CAUSE.md`.
- 2026-07-13 23:23: WIC registration fix started. Selected minimal diagnostic treatment: default-off `MACRUNNER_MR_RUN_REGSVR32_WINCODECS=1`, registering x64 `C:\\windows\\system32\\windowscodecs.dll` with the existing bounded/fail-fast regsvr32 pattern. `wineboot` remains unchanged so the sole verify isolates WIC registration rather than reopening the prior loader/DllMain boundary.
- 2026-07-13 23:42: sole WIC verify is **NO-HIT before title launch**. The first script version chose guest x64 regsvr32; it returned `1`, which Wine defines as `INVALID_ARG` (load/export/DllRegister failures are 3/4/5). The immediately preceding native ARM64 actxprxy registration accepted the same argument form and succeeded. Both runner copies now use that proven ARM64 launcher while retaining the x64 System32 WIC payload, bounded rc propagation, and default-off gate. Source guard and syntax checks pass; no second run or build was performed. Report: `reports/dualdata/ABZU-WIC-REGSVR32-VERIFY.md`.
- 2026-07-14 03:47: explicitly authorized native-launcher verify is **PARTIAL**. ARM64 regsvr32 registers x64 System32 windowscodecs successfully; the game loads it and all prior WIC signatures (`80040154`, `5bd441/5bd471`, special-read NULL, `MEMORY_FAULT`) are zero. ABZU survives to the 300s title timeout with 19 ThreadInit successes, CS-forward 64, no ABBA or runtime status faults. Graphics is still IAT-only: no real CDF1/EnumAdapters1/D3D11CreateDevice/swapchain/render call. Runtime/cache/floor integrity restored and verified; cleanup candidates zero. Report updated: `reports/dualdata/ABZU-WIC-REGSVR32-VERIFY.md`.
- 2026-07-14 04:20: HK CS-forward verify is **NO-HIT**. Dist Unix ntdll updated from `b8183a00…` to byte-valid/codesigned `4b16eda1…`; Fix A is exercised (`64` Enter/Leave CS forwards), but the 600s run remains after MonoManager with swapchain/GetBuffer/RTV/OMSet/Present1 all zero through `+630.850s`. Valid 5s Wine sample proves UnityGfxDeviceWorker plus DXMT encode/finish remain parked `3616/3616` in wait-address paths, active graphics frames zero. Invalid first sample deleted; `/tmp=0`, `>5GiB=0`, cache retained, 57 GiB free. Report: `reports/phase4-hollow-knight/CS-FORWARD-HK-VERIFY.md`.

## Update 2026-07-14 — HK wait/wake boundary is upstream producer stop

- Added default-off, 5000-line-budget `MACRUNNER_HB_WAIT_WAKE_TRACE`; coherent HyperBridge/ntdll build and strict deploy PASS at Unix SHA `d1d48b5b…`, with no synchronization semantic change. One 600s marker-gated run plus one short capture remain NOT_GOLDEN diagnostics.
- Exact current boundary: UnityGfxDeviceWorker TID `e0` waits on guest `0x3a0414c60` / host `0x118ce40dc`; DXMT encode `cc` and finish `d0` wait on distinct per-thread host futexes `0x118ce40c8` / `0x118ce40cc`. All compare `0 == 0` and correctly block.
- Unity main TID `3c` successfully releases the worker five times via `WakeByAddressSingle` at UnityPlayer `+0x2aeebb`, last at `+47.600s`; the worker re-waits at `+47.608s`, then no further wake occurs. Mono reload begins at `+48.589s`. Encode alerts finish twice before Mono; neither receives later work signaling. DXMT is a downstream idle consumer, not the missing-wake owner.
- Disassembly binds `+0x2aee20` to semaphore acquire/WaitOnAddress and `+0x2aee80` to release/WakeByAddressSingle. Rung12 continued post-Mono through Physics `+68.441s`, DXGI `+95.313s`, input `+141.659s`, and swapchain `+142.187s`; current does not. Old `5bfc2b41` binary is unavailable, so source causality remains unproved.
- Correct next discriminator is bounded producer TID `3c` PC movement plus JIT signal-fault identity around sampled guest address `0x87fff9d507c`; do not force-wake Unity/DXMT. Cleanup candidates zero, cache preserved, no make clean, 57 GiB free. Result: `reports/phase4-hollow-knight/WAIT-WAKE-BOUNDARY-RESULT.md`.
- 2026-07-14 08:44: implemented default-off `MACRUNNER_HB_MAIN_PRODUCER_TRACE=1` in the existing diagnostic layer. It filters guest TID `0x003c`, arms from `MACRUNNER_HB_MAIN_PRODUCER_TRACE_ARM_FILE`, and shares one configurable default-5000 budget across block PC/module-RVA, import, and exact wait-object records. No wait/wake behavior, Fix A, Patch C-H, or EH behavior changed. Next: coherent ntdll build/deploy and one 600s NOT_GOLDEN capture armed at the Mono boundary.
- 2026-07-14 08:51: producer observer build/deploy PASS at Unix ntdll `c0e0fe48…`; build/dist byte-equal, strict codesign valid, errors and `install skipped` zero. Immutable NOT_GOLDEN manifest pins wrapper `600c3192…`, complete env delta, runtime/graphics/game inputs, cold absent `c0e0` cache root, preserved 4,081,288-byte prior `d1d` cache, arm-on-Mono watcher and exact-Wine-PID sample. Free space 57 GiB; consume one 600s attempt next.
- 2026-07-14 09:03: main-producer verdict is `PARTIAL - MONO_RELOAD_ACTIVE_THEN_LATE_STATE_UNRESOLVED`. After Mono began at `+51.683s`, guest TID `0x003c` remained active in Mono metadata/assembly code: 2,751 blocks, 2,249 imports, 1,005 changed transitions, 364 distinct blocks, zero waits before the shared budget exhausted 87 ms after arming. No reload completion, Physics, DXGI, or swapchain followed through 600s. Exact late park/exit is unresolved because imports consumed the budget and the trace omitted `native_tid`; forcing a wake is unsupported. Cleanup deleted nothing, preserved both caches, and left 57 GiB free. Evidence: `reports/phase4-hollow-knight/MAIN-PRODUCER-STOP-CAUSE.md`.
- 2026-07-14 09:18: started the refined default-off main-producer observer requested by the previous result: native host TID mapping, independent wait/exit capacity, import aggregation, a 200-block initial burst, and approximately 2-second PC samples across the full 600-second budget. No synchronization behavior or Patch C-H/Fix A semantics will change.
- 2026-07-14 09:31: refined observer source is complete. The main producer now records guest/native TID, a 200-block burst followed by approximately 2-second wall-clock PC samples, periodic top-3 import aggregates, a separate 1024-record wait budget, exact `NtWaitForAlertByThreadId` entry/return address/host-futex/expected/timeout/status, and armed `run_x64` exit reason. The new wrapper fixes the prior `comm=wine` PID-selector error. All additions are default-off and observation-only; next step is coherent build/deploy plus one sealed 600-second diagnostic.
- 2026-07-14 09:39: refined build/deploy PASS at Unix ntdll `2782f3bb…`; build/dist are byte-equal, strict codesign passes, errors and `install skipped` are zero, warnings are 15, free space is 57 GiB. Contract preflight found and removed the wrapper-PID-dependent arm path; the wrapper now uses a fixed scoped marker and aborts before Wine unless its full child environment byte-matches the pre-run sealed capture.
- 2026-07-14 09:47: the sole refined 600-second attempt is sealed as `DIAGNOSTIC_ONLY/NOT_GOLDEN`. Manifest pins HEAD and dirty observer scope, Unix/PE ntdll, all runner hashes, full 51-line parent env, executable, deterministic warm-prefix inventory, 10 graphics overlay sources, cold absent `2782` cache root, complete 127-file cache inventory, host/locale/timezone, timeout, branch map, and corrected exact-argv sample. Any drift fails closed before comparative claims.
- 2026-07-14 10:20: refined verdict is `PARTIAL - X64 MONO CORRIDOR RETURNS; NATIVE POST-RETURN STATE UNRESOLVED`. Guest main `0x003c` maps to native TID `34681011`, stays active in Mono through `+58.150s`, completes infinite-timeout `NtWaitForSingleObject(0x118)` immediately with `STATUS_SUCCESS` at `+59.986s`, then unwinds ten x64 frames with successful guest-return by `+59.996s`. No later x64 main record or real D3D11/swapchain/Present occurs through timeout. Physics and successful real DXGI factory work occurred before Mono, so the next missing stage is D3D11 creation after native return, not Mono hot work or a blocking wait. Host state remains fail-closed because the watcher sampled launcher bash and runner final-child flight artifacts were absent. Cleanup deleted nothing, preserved caches, and left 57 GiB. Report: `reports/phase4-hollow-knight/MAIN-PRODUCER-REFINED-RESULT.md`.
- 2026-07-14 10:34: started a default-off post-`run_x64` native-boundary observer. Scope is final guest-return destination, nesting depth, native caller module/RVA, and the next wait/callback/guest-entry on main TID `0x003c`; `mr-run.sh` must hand off its exact Wine child PID and emit mandatory run-contract/flight artifacts. No synchronization or Patch C-H/Fix A behavior will change.

- 10:04 · POST_RUN_X64 observer patched: independent return/wait/re-entry/exit records plus exact wine-child.pid and mandatory run-contract/flight; next build/deploy and one 600s HK run.

- 10:06 · Observer build/deploy: ntdll SHA256 f5f5c6c8c30c202f2ad52a51b69f39909988893a0d03f3a51a10b016447a4f87; no build errors/install skipped. Cleanup trimmed=0 invalid=0 old_tmp=0, free=43Gi; translation cache preserved.

- 10:21 · First launch was fail-closed before Wine (ledger BLOCKED: save/guest_peb/3 branch UNKNOWNs), therefore not a runtime attempt. Removed invalid dir; ledger now encodes out-of-scope guest PEB as ABSENT_OPTIONAL and wrapper pins empty save snapshot plus branch defaults. Free=43Gi.

- 10:23 · Ledger prelaunch hit duplicate schema validator still requiring guest PEB UNKNOWN; repaired second validator, removed invalid 102145 dir. Game child/PID was never launched.

- 10:25 · Ledger reached READY; final-child capture then failed before game launch because lane dir was 0755. mr-run now forces mandatory artifact parent to 0700; removed invalid 102346 dir.

- 10:40 · POST_RUN_X64 result: GUEST_MAIN_THREAD_TERMINATED (inference). Ten DLL returns were depth 2->1, never final; exact-PID sample 12.6s later has no native TID 34827787 while Cocoa main 34827786 is in CFRunLoop/mach_msg. D3D11 real call/swap/Present=0. Contract READY but flight absent, so NOT_GOLDEN. Cleanup: earlier invalid prelaunch dirs 100643/102145/102346 removed; current trimmed=0 invalid=0 old_tmp=0, free=44Gi; translation cache preserved. Report: POST-RUN-X64-NATIVE-RESULT.md.

- 11:01 · HK build-only lane started: preserve ec510a37 and snapshot 20260714-105634-main-terminated; extend existing POST_RUN_X64 observer exit coverage and capture-only runner tests; no HK runtime authorized.

- 11:06 · Build-only source patch: existing POST_RUN_X64 gate now covers NtTerminate request/result, exit_thread, abort_thread, pthread_exit_wrapper with ordered chain/state/native caller; adversarial static and fake-Wine capture-only tests added.

- 11:08 · Build-only verification stopped: static exit coverage 6/6 PASS; capture-only suite exposed a test-only misplaced blocker diagnostic (`run_dir` NameError). Production observer/runner not implicated; awaiting authorization to correct harness and continue.

- 11:13 · Build-only status: ntdll SHA256 da69bb793a634c9b26939ca4664eea567530563c988d055990a1cc5480fa2e77, static exit observer 6/6 PASS, no deploy/HK run. Capture-only audit found production mr-run PID wrapper parity regression: actual fake child lacks DYLD_LIBRARY_PATH/DYLD_FALLBACK_LIBRARY_PATH/_ captured pre-exec. VERIFIED checkpoint blocked pending runner-fix authorization.

### 2026-07-14 11:44 +10 - HK run_x64 termination boundary

- `da69bb79` observer runtime proves guest main `0x003c` follows `RtlExitUserThread(0xc0000005) -> self NtTerminateThread -> exit_thread -> pthread_exit_wrapper` at +57.722s; no abort/remote/direct-pthread path.
- Outer label=thread run_x64 does not return. D3D11CreateDevice/swapchain/Present1 remain unreached; CreateDXGIFactory2 returns 0.
- Origin of status `0xc0000005` before RtlExitUserThread remains UNKNOWN. Next evidence-only candidate is a bounded last-guest-PC/context origin probe.
- Result: `reports/phase4-hollow-knight/RUN-X64-EXIT-TRACE-RESULT.md` (`VALID_DIAGNOSTIC_NOT_GOLDEN`).
- 12:30 · HK exit-origin probe · NATIVE_CALLBACK_BOUNDARY_AV: main TID 003c hit native write AV at ntdll.so!run_jit_block_with_signal_guard+832, address 0x0fff85f08b49fa8b; handle_syscall_fault returned c0000005 as WINE_UNIX_CALL status and macrunner_hb_BaseThreadInitThunk called RtlExitUserThread; no vectored/SEH dispatch, D3D11/real swapchain/Present1=0 · nearest upstream I386 callback rejection is correlation only; next bounded probe is target/address provenance; report EXIT-ORIGIN-PROBE-RESULT.md.
- 12:43 · HK signal-guard AV disassembly · REJECT_RESUME_SKIPPED_JIT_EPILOGUE: +832 is `STR X8,[X22]` restoring TLS `g_jit_signal_fault_frame=frame.prev`; callback reject resumed at LR with SP still 0x30 below guard frame, so JIT epilogue did not restore SP/X22 and TLS teardown wrote through x86-byte payload 0x0fff85f08b49fa8b · I386 reject is now causal; root fix is handled/result separation in callback routing, not accepting I386 or patching SP; no new probe/build/run; report SIGNAL-GUARD-AV-CAUSE.md.

## 2026-07-14 - HK callback reject root fix verified PARTIAL (pre-swapchain)

- ntdll SHA `db8394149516458e478701eeae284c5075a814cec8aab662a64a602c6c46b4ee`; static callback contract 6/6 PASS.
- Valid NOT_GOLDEN run `laneA-callback-fix-try1-131447`: old guard +832 AV and RtlExitUserThread(c0000005) absent; Wine survived full 600s.
- Timeline: D3D11CreateDevice IAT only +48.715s; Physics +61.054s; CreateDXGIFactory2 rc=0 +61.252s; Begin MonoManager ReloadAssembly +64.253s; no completion/swapchain/GetBuffer/RTV/Present1/pixel.
- Current boundary: post-Mono main activity/wait remains unobserved in this ungated run. Do not widen callback fix; next authorized action is the existing bounded main producer PC/wait observer.
- Cleanup: invalid blocked pre-Wine run-dir 49 MiB removed; `/tmp/macrunner-*` none; no oversized valid log; translation cache preserved; 43 GiB free.

## 2026-07-14 - Post-callback main trace: observer gap after +63.728s

- Valid NOT_GOLDEN run `laneA-post-callback-main-trace-try1-133610`, ntdll callback-fix SHA `db8394149516458e478701eeae284c5075a814cec8aab662a64a602c6c46b4ee`.
- Main `0x003c` -> native TID `35091573`; Mono begins `+62.457s`, then import aggregate `+63.716s` and x64 block `0x87ef277cf38` (module base `0x87ef2450000`, RVA `0x32cf38`) at `+63.728s`.
- All captured waits finish before Mono. No observer records at +100/+200/+300/+400/+500 despite unused block/wait budgets; exact main state after +63.728 remains UNKNOWN.
- Physics +59.797s and CreateDXGIFactory2 rc=0 +59.963s; no actual D3D11CreateDevice call, swapchain, Present1, or pixel.
- Next probe must be an independent host-native timed sampler, not another event-hook-only trace. Callback fix/Patch C-H/fix A remain untouched.
- Cleanup: oversized logs 0, invalid dirs 0, `/tmp/macrunner-*` 0, translation cache preserved, 43 GiB free.
2026-07-14 · HK host-native sampler · main `0x003c`/native `35165983` stayed alive/running through `+633.475s`; no post-Mono wait or main termination, 52 PCs/21 symbols across 284 post-65s samples. Physics and `CreateDXGIFactory2 rc=0` reached, but actual `D3D11CreateDevice`, swapchain, and Present1 remained zero. Boundary is active HyperBridge native callback/signal-server corridor before D3D11; next action is bounded callback/server transition evidence, not a speculative fix. See `reports/phase4-hollow-knight/HOST-NATIVE-SAMPLER-RESULT.md`.
2026-07-14 · HK callback/signal root cause · callback-loop hypothesis disproved: main has 13/13 balanced trampoline/run_x64/ret-LR sequences, status=0 and depth=1. Signal storm is native `ntdll!RtlEnterCriticalSection+0x5c` (`RVA 0x70ed8`) executing `ldr w8,[x18,#0x48]` with fault address `0x48`, proving `x18=0`. Generic `macrunner_hb_arm64_pe_call12` seeds `x18`, then crosses host C at `macrunner_hb_trace_arm64_pe_call12_lr` and fails to reload TEB immediately before `blr x20`. Correct next work is a family ABI-bridge fix plus adversarial x18 tests; preserve fix A and callback fix. See `reports/phase4-hollow-knight/CALLBACK-SIGNAL-LOOP-CAUSE.md`.

## 2026-07-14 - HK x18 PE bridge fix: NO-HIT

- Build/deploy: ntdll SHA `d9b6ea94e6fdef6da47acf7827e8b256fdb7fe31190a4e58c072803c66a1ae88`; static suites `3/3 PASS`; disassembly confirms final `mov x18,x19` before both audited `blr x20` sites.
- Run: `laneA-x18-fix-try1-160838`, contract `READY`, flight exit `124` after the requested 600-second budget.
- Exact TEB incident persisted: 73 `pc=0x87fff9e0ed8 fault=0x48` episodes from `+53.527s` through `+625.932s`; 699 signal-enter records. Verdict `NO-HIT`.
- Progress: Physics, `CreateDXGIFactory2 rc=0`, and Mono reload reached; real `D3D11CreateDevice`, swapchain, and `Present1` not reached.
- Next evidence boundary: correlate bridge ID/target/LR/x18/saved-TEB/native-TID at final PE `blr` with the first following signal. Do not patch another transition until provenance identifies where x18 changes.
- Cleanup: no oversized logs, invalid current-label dirs, or `/tmp/macrunner-*` were removed; translation cache untouched; 43 GiB free.

## 2026-07-14 - HK x18 fix2: consumer proven, producer bridge unknown

- Exact consumer: runtime PC `0x87fff9e0ed8` is PE RVA `0x70ed8`, `RtlEnterCriticalSection+0x5c`, instruction `ldr w8,[x18,#0x48]`; fault address `0x48` proves entry with `x18=0`.
- Valid 600s run `laneA-x18-fix2-final-try1-171046` remained `NO-HIT`: 1,500 bounded `fault=0x48` records, no real D3D11CreateDevice, swapchain, or Present1. Physics, CreateDXGIFactory2 rc=0, and Mono were reached.
- A syscall-return correlation saw valid saved/current TEB but zero service x18. Register, stack, seeded-stack, and dispatcher-owned return implementations did not remove the fault and two regressed; all experimental syscall changes were removed. Syscall causality is not claimed.
- Stable restored dist: Unix `7b8e887b792a07b454b45cd8bbbab1901c4d590d4d675117ce3f23d03a38b274`, PE `c52154865375fcdb1cb6404ed403b775ddac566cb7f362e439f2d13b139e6a40`. PE syscall stub is back to the original no-frame form.
- Next evidence-only action: trace the first x18 mutation across all PE/native transition families with before/after values and bridge identity. No further bridge patch is justified yet.
- Cleanup: two invalid/flaked run dirs removed; oversized logs 0; `/tmp/macrunner-*` 0; translation cache preserved; 42 GiB free. Report: `reports/phase4-hollow-knight/X18-FIX2-VERIFY.md`.

## 2026-07-14 — HK x18 producer bridge adjudication

- Verdict: `NO-HIT / FALSE PREMISE`; complete PE `blr` audit found only call12/call20_direct and both restore x18 immediately before transfer.
- Exact consumer is `RtlEnterCriticalSection+0x5c` (`ldr w8,[x18,#0x48]`); caller chain is heap allocation, but x18 is valid immediately before that corridor.
- A syscall/unix-call final-thunk experiment (SHA `0d8ebd6e...`) left the 600s fault population unchanged and was removed.
- Residual x18 loss is the documented macOS/xnu context/sigreturn ABI behavior, not another producer bridge. Clean build/dist restored to `ef5b0819...`.
- Runtime boundary remains Physics +58.148s, GfxDevice +58.347s, CreateDXGIFactory2 rc=0 +58.354s; no real D3D11CreateDevice or swapchain.
- Evidence: `reports/phase4-hollow-knight/X18-PRODUCER-BRIDGE-RESULT.md`.

## 2026-07-14 — HK main termination build-only checkpoint

- Status: `VERIFIED_NOT_GOLDEN`; no Wine/Hollow Knight runtime and no dist deployment.
- Existing `MACRUNNER_HB_POST_RUN_X64_OBSERVER` now covers remote process-wide `NtTerminateThread` callers with a bounded 256-record budget and an explicit `natural-return/x64_thread_entry` stage.
- Existing `exit_thread`, `abort_thread`, and final `pthread_exit_wrapper` sink coverage remains independent of `run_x64 pending` state.
- `RtlExitUserThread` remains a separately gated PE marker (`MACRUNNER_TRACE_PROCESS_EXIT`); no synthetic PE-to-Unix coverage claim was added.
- Focused suites PASS: termination 7/7, host sampler 8/8, post-Mono 15/15, capture-only contract 6/6.
- ntdll-only build SHA: `5dd1d7f946d763d0b1764a05d21152a88cfe5ad05944e5fcef191d63415d3c7a`; deployed dist unchanged at `ef5b081900965c72112fe3bba480393a4f623c175adf069e85d2add0fbbc1054`.
- Sealed checkpoint: `reports/phase4-hollow-knight/checkpoints/20260714-202010-main-termination-build-only`.
- Stop state: waiting for explicit runtime authorization.

2026-07-14 20:34 +10 · HK x18 producer 120s NOT_GOLDEN attempt · deployed ntdll `5dd1d7f946d763d0b1764a05d21152a88cfe5ad05944e5fcef191d63415d3c7a`, but run contract failed closed before Wine: missing save-snapshot manifest and branch-map inputs actxprxy/crt_case_fusion/wwise_observer. Runtime gates NOT MEASURED; producer remains UNKNOWN. Evidence: `reports/phase4-hollow-knight/checkpoints/20260714-203458-main-termination-prelaunch-blocked/`. Next: repair contract inputs, then obtain explicit runtime reauthorization.

2026-07-14 22:28 +10 · HK x18 producer contract-fixed 120s NOT_GOLDEN · READY/zero-UNKNOWN/VALID_RUN; fault=0x48 raw 1412, signal-enter 732, dominant LR/x19 reproduce RtlEnterCriticalSection/heap_allocate_large consumer chain, no missing producer bridge identified. HK main 0x003c survives; DXGI rc=0, D3D11 real=0, swapchain/Present1=0. Run: `reports/phase4-hollow-knight/laneA-x18-producer-contract-fixed-try1-222528`.

2026-07-14 23:45 +10 · HK x18 self-heal 600s · requested post-sigreturn resume thunk was already present in source/build; rebuild SHA unchanged `5dd1d7f9`. READY/VALID_RUN, recovery confirmed by continued execution after faults through +527.464s, but raw fault=0x48=1416 and signal-enter=992; therefore NO-HIT for zero-fault elimination. HK main survives; DXGI rc=0; D3D11 real/swapchain/Present1=0. Report: `reports/phase4-hollow-knight/X18-SELFHEAL-VERIFY.md`.

2026-07-15 00:16 +10 · HK x18 progress-impact 600s · main is not heap-resident: 0 heap samples. From +57.851 to +623.759, 114/114 consecutive samples stay at anonymous JIT PC `0x11d27c040`, running and not waiting. Boundary is SIGBUS at +53.070, `str x20,[x21]`, fault `0x168e90010`; fallback fails to escape or immediately re-enters the block. X18 is transient overhead, not direct D3D11 blocker. Report: `reports/phase4-hollow-knight/X18-FAULT-PROGRESS-IMPACT.md`.

- 2026-07-15 JIT SIGBUS root fix staged: faulting native cache entry is quarantined across runtime cache resets; context resumes from the complete pre-block snapshot and the same native block cannot re-enter. Build/600s HK verification pending.

- 2026-07-15 JIT SIGBUS verify (`laneA-jit-sigbus-fix-try1-033647`): PARTIAL/no-exercise. READY NOT_GOLDEN contract; SIGBUS=0, invalidations=0; main alive/running through +623.497s without prior same-PC dominance. Physics, DXGI rc=0, Mono reached; real D3D11/swapchain/Present1 remain 0. See `reports/phase4-hollow-knight/JIT-SIGBUS-INVALIDATE-VERIFY.md`.

- 2026-07-15 module-predicate probe (`laneA-module-predicate-loop-try1-035735`): predicate loop disproved. Guest counters advanced to 6,974,646 block updates / 3,423,375 PC changes, then froze after outer `run_x64` returned `0xc000007b` at pseudo import index 951. The next 114 samples are `#RtlExitUserThread+8`, not classifier progress. JIT SIGBUS invalidation exercised exactly once with mapped quarantine and no re-entry. See `reports/phase4-hollow-knight/MODULE-PREDICATE-LOOP-RESULT.md`.

## 2026-07-15 04:34 · HK import-thunk 951 bounded probe
- ntdll probe build/deploy SHA: 0b9b509e8d00101f48c3a73c6a6ea1988525a32b05a059a635df5c839e7c021e.
- Valid 120s NOT_GOLDEN run: laneA-import-thunk-951-runtime-try1-042914, contract READY.
- Index 951 did not reproduce: probe=0, import-thunk/EXEC_FAULT=0. Main instead returned c000007b via runtime fast-fail at guest PC 0x87ef293c20c (+60.043s).
- SIGBUS invalidation exercised once (mapped=1, quarantined=1, count=1); D3D11 IAT registered but no D3D11 invocation/swapchain/Present1.
- Verdict/report: reports/phase4-hollow-knight/IMPORT-THUNK-951-CAUSE.md. Root cause remains UNKNOWN; next evidence action is one identical 120s A/A repeat.

## 2026-07-15 08:28 · HK fastfail/import-951 offline correlation verdict
- Import 951 is closed: `KERNEL32!RaiseException`; Mono calls `RaiseException(0xe0000001, EXCEPTION_NONCONTINUABLE, 0, NULL)` after fatal `mono_x86_patch_inline` assertion (`x86-codegen.h:403`, unexpected patchable opcode).
- The earlier `int 0x29` is secondary to a corrupted nonvolatile frame RBP: expected `0x11cac79b0`, observed `0x59d9b5f20`; the cookie was loaded from unrelated mapped memory, so true cookie-slot corruption is not proven.
- The immediate terminals are independent, but both follow the same mapped JIT SIGBUS/quarantine in Mono SIMD-copy RVA `0x4ee14b/0x4ee150` within milliseconds. Current recovery restores `hb_context_t` but cannot roll back native memory side effects; shared causality remains unproven.
- No code fix authorized by this evidence. Next is an immutable A/A pair with fastfail, import-951, SIGBUS pre/post-state, RBP-invariant, and Mono patch-byte probes; only then a one-variable interpreter-from-entry A/B.
- Full verdict: `reports/phase4-hollow-knight/FASTFAIL-IMPORT951-VERDICT.md`. Cache untouched; Data volume had 41 GiB available.
2026-07-15 09:17 · HK fastfail/import-951 A/A in progress. Five probes are built/deployed together: terminal fastfail cookie/stack, import semantic-dispatch, bounded JIT SIGBUS state/recovery, Mono RBP invariant (0x425870..0x425c8e), and Mono patch assertion. A1/A2 use one fixed prefix pathname seeded from the same immutable template, one reset cache pathname seeded byte-identically, one graphics overlay, clean child env, and 120s timeout. Next: execute A1, cleanup+df, restore cache clone, execute A2, exact compare.
2026-07-15 09:27 · Added default-off B-only gate before A1 so A/A and later A/B can share one runtime artifact. `MACRUNNER_HB_AA_FORCE_MONO_SIMD_COPY_INTERP=1` forces the Mono SIMD-copy signature block through interpreter fallback before native execution; A1/A2 leave it unset. Next: rebuild/redeploy, then execute A1/A2.
2026-07-15 09:29 · Rebuild/relink/deploy PASS for final A/A artifact set: `libhyperbridge.a=cbeb5421`, build `ntdll.so=9e80a02f`, signed deployed `ntdll.so=4cbef8c3`. Next: A1 then cleanup/df, A2 then exact compare.
2026-07-15 09:44 · A/A pair completed with deterministic non-reproduction: A1/A2 prestate, final child, run contract, and wrapper rc are byte-identical; both ended `rc=124` with no SIGBUS/import951/fastfail/patch records and identical normalized RBP probe sequence (`mismatch=0`). Result: `reports/phase4-hollow-knight/AA-PAIR-RESULT.md`. Next: limited B gate check; causal strength depends on whether the force-interpreter gate is exercised.
2026-07-15 09:49 · Limited B gate check completed but is not Mono-causal: `MACRUNNER_HB_AA_FORCE_MONO_SIMD_COPY_INTERP=1` hit 8 times, then produced a JIT signal fallback/runtime MEMORY_FAULT in `UnityPlayer.dll` (`rva=0x19e94b9`). The byte-signature matcher is too broad for the requested Mono-only intervention. Updated result file; next valid causal attempt needs a Mono module/RVA-scoped matcher and fresh A/A seal.
2026-07-15 10:12 · Mono/RVA-scoped gate patch in progress: HyperBridge context now carries current module base from `macrunner_hb` block classification; force gate requires Mono module and RVA `0x4ee14b/0x4ee150`; probe-only gate added for fresh A/A. Next: rebuild/deploy, create fresh sealed run root, A/A then B.
2026-07-15 10:13 · Narrowed gate build/deploy PASS (`libhyperbridge=814ae6e9`, build `ntdll=cafe0dea`, deployed signed `ntdll=558eaef2`). Next: fresh A/A seal with probe-only narrow matcher enabled, then one-variable B force.

2026-07-15 10:31 · Mono/RVA-scoped A/A+A/B completed. Fresh A/A passed with deterministic non-reproduction: A1/A2 prestate, run contract, final child, and wrapper rc byte-identical; gate hits only Mono RVA `0x4ee14b/0x4ee150`; no SIGBUS/import951/fastfail/runtimefail. A/B one-variable check passed (`MACRUNNER_HB_AA_FORCE_MONO_SIMD_COPY_INTERP=1` only in B), but B forced only `0x4ee14b` and exited the path early with `macrunner-hb-run-exit-indirect` MEMORY_FAULT fields before reaching `0x4ee150` or RBP probes. Verdict: no JIT-causality proof; next is forced-interpreter transparency diff at Mono RVA `0x4ee14b`. Result: `reports/phase4-hollow-knight/MONO-SIMD-NARROW-AB-RESULT.md`.

2026-07-15 11:03 · Mono `0x4ee14b` forced-interpreter transparency diff completed. Same-prestate native JIT versus IR-helper comparison produced equal architectural PC advance, destination/source windows, XMM hash, and key registers; the raw RIP/full-GPR mismatch is expected helper synchronization timing. RVA `0x4ee14b` is a five-byte NOP immediately before the SIMD-copy body at `0x4ee150`. The prior B MEMORY_FAULT was caused by routing the forced block through the outer unsupported-feature fallback, not by interpreter semantics. Replacing that diagnostic plumbing with inline `hb_jit_helper_exec_ir_block()` preserved a byte-identical A/A seal; B reached `0x4ee150` and the RBP probes with no MEMORY_FAULT. Causality for the original RBP corruption/bad patch remains unproven because A is still a deterministic non-reproduction. Result: `reports/phase4-hollow-knight/MONO-4EE14B-TRANSPARENCY-RESULT.md`.

2026-07-15 11:31 · Current Hollow Knight wall localized on actual-current deployed `ntdll.so=7cc24c21` (requested historical `0b9b509e` is no longer preserved). Two READY, byte-identical-prestate 120 s runs reach real `CreateDXGIFactory2` rc=0, then freeze at `+20.045s` on Mono guest PC `0x87ef26c91bc` / RVA `0x2791bc`; guest progress stays fixed through timeout with no main termination, run-exit, import fault, fastfail, c000007b, or pc=0x60. A five-second host sample proves main is inside a JIT SIGILL: `run_x64→hb_jit_runtime_run→run_jit_block_with_signal_guard→_sigtramp→ill_handler→ARM64X hexpthk redirect→module_from_pc/Mach VM scan`. Root cause is signal ownership order: `ill_handler` performs expensive ARM64X PE classification before allowing HyperBridge to claim its active JIT fault. D3D11 real call and swapchain remain downstream/unreached. Fix candidate: JIT ownership check first, generalize quarantine to owned SIGILL, then capture native word and fix the evidenced codegen family. Result: `reports/phase4-hollow-knight/CURRENT-WALL-CAUSE.md`.

2026-07-15 12:54 · JIT SIGILL ownership implementation verified but end-to-end result is FAIL. The first slab-only implementation missed because LLDB found the guarded Mono `0x4ee150` block had transferred to an out-of-slab zero RX page (`UDF #0`). Corrected default-off ownership now uses the active TLS JIT guard before ARM64X scan, carries the signal-safe word/ucontext across longjmp, and quarantines the guarded source on native-PC miss. In the READY 600 s corrected run it fired once (`mapped=0 quarantined=1 active_guard=1 disable_jit=0 word=00000000`), with zero ARM64X scan records after the claim; main progressed past +20 s. It then reached the earlier Mono `int 0x29` at guest PC `0x87ef293c20c`, returned `0xc000007b`, and terminated by +22.037 s. Real D3D11/swapchain/Present remained 0; c0000005/pc60/import951 remained 0. The source branch decoder found no B/BR edge to the zero target, so no codegen-family patch is justified yet. Next evidence: preserve fault-time hb_context before snapshot restore, bind the true predecessor outside the guarded entry, and diff already-committed SIMD source/destination writes before replay. Full result: `reports/phase4-hollow-knight/JIT-SIGILL-OWNERSHIP-FIX-VERIFY.md`.

2026-07-15 14:02 · Null-emission hypothesis disproved and actual root fixed. Full VM/store provenance bound the zero word to Mono's external R-X code-copy destination, not HB-generated native code: the `0x4ee150` source block emitted eight valid raw 128-bit stores and the all-cache scan found zero branches to the destination. The shared XMM-store emitter now routes through checked W^X-aware `hb_memory_write` semantics; cache v21 rejects stale unsafe blocks. READY 600 s verification: UDF/SIGILL/SIGBUS/fastfail/import951/main-c0000005/c000007b/pc60 all zero; real DXGI factory rc0, swapchain rc0/non-null, Present rc0. Main remained alive until timeout at Mono RVA `0x2791bc`. Strict pixel capture found the 1024x768 window but failed in `screencapture`; no snapshot. Classification PARTIAL (root fix PASS, pixel/end-to-end incomplete). Result: `reports/phase4-hollow-knight/CODEGEN-NULL-EMIT-FIX-VERIFY.md`.

2026-07-15 17:08 · Pixel verification corrected the graphics endpoint. Replaced the failing window-id-only capture with exact native ScreenCaptureKit capture (control strict gate COLORFUL). Sealed 600 s HK run was READY and fault-clean; main remained alive through +587 s. CreateSwapChainForHwnd returned rc0/non-null, but the earlier `method=Present rc=0` was factory slot 8 `MakeWindowAssociation`: object equals factory, differs from created swapchain, and `known_swapchain=0`. Robust known-object GetBuffer/Present/Present1 plus RTV/OMSet are all zero. Thirty-seven exact 1024x768 captures were uniformly BLACK (zero nonblack/colorful pixels). Current wall: post-swapchain/pre-backbuffer-method; verdict FAIL/no pixel. Result: `reports/phase4-hollow-knight/PIXEL-VERIFY-FINAL.md`.
2026-07-15 17:56 · HK current post-MakeWindowAssociation wall is now byte-proven. Object-aware DXGI tracing records `CreateSwapChainForHwnd rc=0`, factory slot 8 `MakeWindowAssociation rc=0`, and no real GetBuffer/RTV/OMSet/Present. Main advances 13.3M post-association block entries, then remains for 51 consecutive 5-second samples at Mono RVA `0x2791bc`. Native `sample` places 781/786 main samples in `hb_jit_helper_store_u128`; its `hb_jit_helper_write_bytes_tso` still accepts `hb_jit_live_host_ptr(HB_PERM_WRITE)` and raw-memcpy's metadata-RWX/current-Mach-RX memory, bypassing `hb_memory_write` and looping through signal callback/ARM64X routing. Proper next fix: make executable/current-nonwritable 128-bit stores unconditionally use the authoritative W^X write/generation/invalidation path; retain raw memcpy only for proven RW non-executable pages. Evidence: `reports/phase4-hollow-knight/POST-MAKEASSOC-TRACE-RESULT.md`.
2026-07-16 10:05 · Unity callback `+0x4784d0` is not the current wall. READY 600 s cross-thread proof records Gfx tid 00e8 plan/enqueue, scheduler tid 0078 callback entry before enqueue return, coordinator pending `+0x18e8` transition 1→0, tail reschedule, and normal return to scheduler `+0x51dc03`. Static executor gate `[task+7]&1` took the direct `call [task+0x30]` path. The callback is a coordinator work-drain/reschedule function, not Present or a D3D Clear completion callback; all 114 bounded flow edges stayed in Unity. Game PID remained fault-clean/alive to rc124; factory/swapchain creation remained rc0; strict visible-window capture stayed BLACK and no real Present occurred. Next evidence boundary is the new coordinator work object installed at `+0x18d0=0x3002342b0` after callback: follow its event-driven consumer/re-arm path to frame submission. Report: `reports/phase4-hollow-knight/CALLBACK-4784D0-CAUSE.md`.
2026-07-16 11:33 · The `+0x18d0` “work object → Present” hypothesis is disproved. A bounded READY 600 s run reproduced the allocation at a new heap address `0x300230070`: it is a 32-byte Unity buffer/arena descriptor with null head/cursor, 4 MiB backing, capacity `0x400000`, and allocator tag 16. Scheduler tid 0078 constructs/publishes it, immediately calls `Unity+0x2b6fc0` on the same thread, receives `AL=1` for empty state, and intentionally skips rescheduling because descriptor capacity equals `coordinator[+0x262c] << 20`. It is neither a DXMT frame object nor a missing consumer boundary and has no Present edge. Target remained fault-clean/alive to timeout; real Present stayed 0 and strict pixel was BLACK. Next evidence must trace the actual Gfx-owner submission vcall at `Unity+0x478a65` (vtable `+0x688`), not patch this correct housekeeping object. Report: `reports/phase4-hollow-knight/WORK-OBJECT-3002342b0-RESULT.md`.

2026-07-16 12:43 · Gfx-owner vtable `+0x688` is resolved and is not a Present/submit method. Static receiver tracking proves the object is Unity-internal; the slot resolves to `UnityPlayer+0x6c6960`, a generic `mov rax,rdx; mov edx,1; jmp rax` queued-callback adapter. In the READY 600 s run, both exact owner cycles reached the queue pop at `UnityPlayer+0x478a46`, but `coordinator+0x18` returned `RAX=0`; execution therefore branched at `+0x478a4f` to the alternate `coordinator+0x10` queue path and never reached the `+0x688` callsite. Main stayed alive/fault-clean through timeout, real Present remained 0, and strict pixel capture was BLACK. No fix belongs at `+0x688`; next evidence boundary is alternate queue pop/indirect callback `UnityPlayer+0x478abf`. Report: `reports/phase4-hollow-knight/GFX-OWNER-VTABLE-688-RESULT.md`.

2026-07-16 13:27 · Alternate queue `+0x10` is also empty; `UnityPlayer+0x478abf` is an indirect `call rax` site, not a callback function or Present wrapper. In the READY 600 s run, `coordinator+0x10=0x300b302c0` and primary `+0x18=0x300b304c0` both held lock-free empty state `0x2`; the alternate pop returned `RAX=0` and branched `Unity+0x478a7a→+0x478b88`, so no payload, dynamic target, callback entry/return, or Present existed. Static flow shows `+0x20` deferred items are recycled toward `+0x10` before return. Main remained alive/progressing to timeout, target fault counters stayed zero, strict pixel remained BLACK. No consumer fix is warranted; next evidence boundary is the producer publication path into coordinator queues `+0x10/+0x18/+0x20`, correlated to frame finalization. Report: `reports/phase4-hollow-knight/ALTERNATE-QUEUE-478ABF-RESULT.md`.

2026-07-16 14:36 · Producer publication is working; the earlier empty-queue state was sampled after consume. A READY 600 s event-driven run caught `Unity+0x478718→+0x4787a0` construct item `0x150003f50`, then scheduler `Unity+0x478650` atomically publish it to `coordinator+0x10` and wake the owner. A second publish of the same item advanced the queue tag `1→2` and was drained before the wake snapshot. The payload contains TextCore/Font bytes and targets `Unity+0x46f320`; no frame-level item, `+0x478de0` frame path, real GetBuffer, or Present appeared. Main was alive through +590.892 s; strict pixel remained BLACK. No MPSC/consumer fix is warranted: next boundary is the upstream Unity end-of-frame predicate that should create a Present-bearing item. Report: `reports/phase4-hollow-knight/PRODUCER-PUBLISH-RESULT.md`.
## 2026-07-16 — Hollow Knight current wall: pre-frame Mono/managed initialization

- Current wall is upstream of frame finalization: main remains alive and changes guest PC continuously, but the post-`MakeWindowAssociation` sample population is dominated by Mono/managed code through the 600 s timeout. No real GetBuffer/Present item is expected before the first frame-end phase.
- `CreateSwapChainForHwnd rc=0`, `MakeWindowAssociation rc=0`, and the actual swapchain vtable slot 8 is non-null. Focus is unknown/unproven and WINSHOW is not causal; the on-screen 1024x768 window is byte-exact BLACK.
- Corrected `MACRUNNER_HB_PRESENT_ITEM_CREATION_PROBE` disproved its initial callback assumption: table `+0x728/+0x918/+0x920/+0x930` values are RW self-linked sentinels and do not map to live JIT blocks. `Unity+0x8f4510` is a counter helper and `+0x45b949→+0x478de0` is generic scheduling, not proven Present production.
- Do not force queues, focus, or Present. Next: a low-observer performance seal with completed per-block probes off, host-native sampling retained, and an exact first-PlayerLoop/real-GetBuffer event marker; fix only the measured HB/Mono-init mechanism.
- Verdict: `reports/phase4-hollow-knight/PRESENT-ITEM-NOT-CREATED-CAUSE.md`.

## 2026-07-16 — Hollow Knight Mono init is slow, not stuck

- A READY 600-second host-only sample run kept main tid 003c alive through +595.910 s. It was `running` in 115/119 samples and advanced 147,062,147 block observations / 73,312,594 PC changes; there is no fixed guest PC or permanent wait to break.
- Guest samples span Mono internal JIT/metadata/type work (70/119), pseudo-import dispatch (31/119), and UnityPlayer (10/119). Confirmed Mono exports include `mono_class_get_checked` and `mono_metadata_type_hash`; no assembly-load error/retry evidence exists.
- Immediate engine bottleneck: `__findenv_locked` is 28/119 host samples. Exact frame chains bind 12 to disabled `macrunner_hb_aa_rbp_probe` and one to disabled `macrunner_hb_aa_mono_patch_probe`; both env flags are absent, yet helpers are invoked per block (RBP twice). `hb_memory_protect` is 22/119 and Mono module/loader classification 7/119.
- Target fastfail/SIGILL/SIGBUS/c000007b/pc60/main-c0000005 are zero. x18 appears only as four TLS-set context fields, not faults. Real frame methods remain unreached and strict capture is BLACK because first frame-end has not occurred.
- Next root fix: a family-wide zero-cost-disabled diagnostics config cached once per run, then loader-generation-safe Mono module range caching. Fresh A/A and one-variable pre/post comparison must precede any W^X batching work. Full verdict: `reports/phase4-hollow-knight/MONO-INIT-STALL-CAUSE.md`.

## 2026-07-16 — Minimal-overhead endpoint crossed managed lifecycle; new JIT-helper wall

- The env/module hot-path fix is effective: final host profile sampled `__findenv_locked=0/118`
  and `module_name_matches=0/118`; main reached 175,551,734 blocks / 87,714,727 PC changes by
  590.475 s. Remaining `hb_memory_protect=24/118` is localized to
  `sync_virtual_region -> kernel32 semantic` and is not safe to bypass generically.
- A probe-free 1800 s endpoint entered managed scene/object/camera lifecycle by +416.118 s and
  continued with a Unity marker at +1398.514 s. The prior “Mono init through timeout” wall is no
  longer current.
- New exact wall: at +633.070 s one managed worker returned `EXEC_FAULT reason=JIT helper fault`,
  status c000007b, at managed JIT PC `0x5297f9610`. The process remained CPU-active but strict
  pixel stayed fully black through timeout (255 BLACK, 0 PASS).
- Next probe must be bounded at the JIT-helper fault itself: capture helper ID/IR op, operand
  address, HB region/protection, owning JIT block, and fallback eligibility. Do not reopen
  DISPROVED loader/queue/focus theories and do not weaken W^X.
- Verdict: `reports/phase4-hollow-knight/MINIMAL-OVERHEAD-1800S-RESULT.md`.

## 2026-07-16 — +633 JIT-helper wall fixed; current wall is render PSO

- The managed `c000007b` was an x87 helper fault, not W^X/x18/signal: x64 x87 state aliased
  the x86/x64 GPR union. Independent x64 state plus complete FST/FSTP register/memory masked-
  underflow semantics closes the family; targeted interpreter+JIT tests are 19/19 PASS.
- Sealed 1800 s run `laneA-jit-fault-633-x87masked-try1-012020` crossed Mono `0x56c8f0`, entered
  managed rendering, and remained alive through +1843.898. Runtime-fail/JIT fallback/c000007b/
  c0000005/SIGILL/SIGBUS/low-PC gates are all zero.
- Current wall is graphics-owned: WineMetal repeatedly reports `skipping draw commands because no
  render PSO was set` from +901.069 through +1842.642. Strict capture: 264 BLACK, 35 NO_WINDOW,
  0 PASS. Do not reopen Mono-init, queue, focus, x18, or helper-fault theories.
- Full verdict: `reports/phase4-hollow-knight/JIT-FAULT-633-CAUSE.md`.

## 2026-07-17 — Render-PSO boundary ready for one-run classification

- Offline source truth narrows the black frame to a missing `WMTRenderCommandSetPSO` before
  Draw. The WineMetal warning alone does not prove shader compilation failed: it can also mean
  a successful PSO was not serialized into that render command list.
- The exact upstream branch is `MTLCompiledGraphicsPipeline::GetPipeline`: null state omits
  `SetPSO`; a non-null state should encode it. The prior final D3D11 log contains no existing
  `Failed to create PSO` error, so compile failure remains unproven.
- The 1800 s diagnostic completed. Five observed PSOs compiled successfully, 8182 non-null
  `get-ready` events were captured, and Present/Present1 each returned `S_OK` 1521 times. Pixel
  remained strictly BLACK and all CPU/JIT fault gates stayed zero.
- Root cause is now concrete: WineMetal's local `has_pso` is scoped to one `encodeCommands` call.
  DXMT emits SetPSO and a later Draw in separate calls on the same encoder/head, so the Draw sees
  a false empty state and is skipped. Next action is per-encoder state lifetime through
  `endEncoding`, with split-call/no-PSO/two-encoder regression tests before A/A runtime sealing.
- Full evidence: `reports/phase4-hollow-knight/RENDER-PSO-CAUSE.md`.

## 2026-07-17 — PSO lifetime fixed; 120 s runtime did not reach the boundary

- WineMetal now persists PSO-bound state on each native render encoder across split
  `encodeCommands` calls and clears it at `endEncoding`. Build and deployment passed; deployed
  `winemetal.so` SHA is `6b47b2c7…`.
- The requested no-probe 120 s Hollow Knight verification timed out alive before real swapchain,
  GetBuffer, RTV, OMSet, Draw, or Present. All CPU/JIT fault gates stayed zero, but semantic
  runtime verification is **NO-HIT**, not PASS.
- The follow-up 1800 s run reached the managed frame phase. Against the prior probes-off baseline,
  the unconditional missing-PSO Draw-skip diagnostic changed from 1,648 to 0 while Unity frame
  messages remained active (1,563 in the fixed run). This closes the specific PSO-forgetting
  symptom without re-enabling observer probes; all CPU/JIT fault gates stayed zero.
- Pixel remains blocked: 146 strict on-screen captures were 100% black. Because Draw/Present
  method probes were deliberately off, successful method counts are unobservable and no Present
  `S_OK` claim is allowed. Current status is **PARTIAL**.
- Next evidence-bearing step is a bounded Draw/Present counter run or direct split-call harness,
  followed separately by the now-dominant Unity out-of-frustum/black-frame investigation.
- Verdict: `reports/phase4-hollow-knight/PSO-PERSIST-FIX-VERIFY.md`.

## 2026-07-17 — Draw and Present pass; backbuffer/output remains black

- The bounded 1800 s method run reached GetBuffer=1, RTV/RTV1=4, OMSet=2,782,
  Draw=3,046 with missing-PSO=0, and Present1=1,390 with `S_OK`.
- PSO persistence is directly proven by 96 Draw-bearing split calls with no local SetPSO and no
  missing-PSO rejection. No nil drawable, skipped presentDrawable, Metal command error, or
  CPU/JIT fault occurred.
- Pixel truth remains FAIL: 147 on-screen captures were 100% `#000000`; no non-black capture.
- Current boundary: the D3D backbuffer/pixel-output contents. Missing scene managers/camera and
  one out-of-frustum warning per frame are correlated, not yet causal.
- Next: bounded GPU readback at Clear/Draw/pre-Present plus matching draw state
  (counts, viewport/scissor, RTV, shaders, buffers, textures). Patch only after this separates
  zero coverage, black shader output, and presenter-copy failure.
- Verdict: `reports/phase4-hollow-knight/DRAW-EXECUTED-BLACK-CAUSE.md`.

## 2026-07-17 — Backbuffer is already RGB-black before Present

- A sealed native GPU-readback run reached the render boundary at +885.650 s and completed all
  bounded Metal blits. After-Clear is all-zero. Four after-Draw and four pre-Present samples are
  786,432/786,432 black with identical hash `d69a0ed1d32d0383`; RGB min/max/mean is zero.
- This rules out the presenter/window copy as the black-screen cause. It does not prove zero
  fragment execution: alpha changes from zero to max 5 / mean 4.167, consistent with GPU coverage
  inside the 1024×640 scene viewport while visible colour remains black.
- First 32 Draws contain 31 indexed scene Draws with non-null PSO/VS/PS/IB and argument buffers,
  plus one fullscreen `Draw(3)` presenter pass that samples the black render target. Viewport and
  scissor are valid; direct texture slots on scene Draws are intentionally represented through
  argument buffers and remain undecoded.
- Managed evidence precedes rendering: Game Manager missing at +370.544 s, GameCameras at
  +529.122 s, UIManager 11 times at +547 s, then 1,758 out-of-frustum warnings through timeout.
  Core Unity data files and 501 level/shared-assets pairs exist, so missing extraction files are
  not established. All CPU/JIT/signal fault gates remain zero.
- Next boundary is shader input/output: decode argument-buffer textures/constants and camera
  matrices, then capture one Draw's VS clip output. Patch only the layer whose bytes first diverge;
  do not add a presenter workaround.
- Verdict: `reports/phase4-hollow-knight/BACKBUFFER-READBACK-RESULT.md`.

## 2026-07-17 — Shader inputs valid; black introduced after input binding

- A sealed 1800 s run reached Draw at +925.074 s with no CPU/JIT/signal faults. All 344 bounded
  CB records are CPU-visible and nonzero; active VS matrices include identity, projection, and
  transform values, while PS constants include white/nonzero color/time values.
- DXMT sampler/SRV records and native Metal slot-29/30 qwords agree. Live resource-ID lookup
  read back two bound RGBA8 textures: 27,825/27,825 and 137,504/140,238 pixels have nonblack RGB.
  Argument-buffer loss and black texture content are therefore not the cause.
- Strict pixel truth is still BLACK (146 on-screen captures, zero nonblack). Earlier GPU evidence
  remains RGB zero with alpha max 5 before Present. The generic D3D11/Metal write-mask bit-order
  suspicion is excluded by `kColorWriteMaskMap`; actual active PSO blend/write-mask values remain
  unknown.
- Do not revisit camera, CB, texture, or argument-buffer binding. Next single-variable diagnostic:
  capture active PSO RT0/blend/write-mask/fragment-function identity, then replace only fragment
  output with opaque magenta. Magenta isolates DXBC→Metal output translation; continued black
  isolates PSO/attachment state.
- Verdict: `reports/phase4-hollow-knight/SHADER-INPUTS-CAUSE.md`.

## 2026-07-17 — Fragment-output vs attachment-state A/B is build-ready

- The next boundary is instrumented without presuming blend culpability. D3D11 and native
  WineMetal now emit a shared stable PSO ID with original fragment identity,
  `PSValidRenderTargets`, D3D RT0/blend values, final Metal attachment descriptor, and the
  actual bound Draw.
- An exact-ID, default-off control replaces only the fragment result with opaque magenta.
  It fails closed and cannot silently target a different PSO.
- Build-only verification is PASS: source contract, Metal shader compile, blend/write-mask
  fixture compile, target-clean arm64/x86_64 builds, and diff checks. No runtime was run.
- Current verdict remains `UNKNOWN`. Next is a sealed 1800 s diagnostic A followed by B;
  no fix is authorized unless the original PSO/Draw matches and GPU readback separates
  magenta from continued black.
- Handoff: `reports/phase4-hollow-knight/FRAGMENT-OUTPUT-MAGENTA-AB-BUILD.md`.

## 2026-07-17 — Black-frame boundary is fragment output, not attachment state

- Sealed 1800 s A/B PASS: identical original PSO IDs/state and 64 bounded Draws were
  observed in both runs. A remained RGB zero; B's sole independent diagnostic variable
  forced opaque magenta and GPU readback became 786,432/786,432 RGBA 255,0,255,255 after
  Draw and pre-Present.
- Runtime blend/write-mask/RTV propagation is removed from the suspect set. BGRA/RGBA
  ordering is likewise not an all-RGB-zero explanation.
- The open root-cause boundary is inside the original fragment-stage result path:
  translated MSL return semantics/output ABI versus the constant/texture arguments read
  by that function. Do not patch until a matched failing PSO provides byte-level evidence
  separating those two cases.
- Keep `MACRUNNER_HB_FORCE_FRAGMENT_MAGENTA` default-off and diagnostic-only.
- Verdict: `reports/phase4-hollow-knight/FRAGMENT-OUTPUT-MAGENTA-AB-RESULT.md`.

## 2026-07-17 — Fragment output ABI disproved; stage-link value is next

- A valid 1800 s diagnostic captured exact DXBC, preopt AIR, postopt AIR and metallib for
  five real pixel shaders. Every DXBC output is float `SV_Target0/o0.xyzw`; every AIR
  function returns the computed float4 in render target 0, with no compile error.
- `ps_bef43b20` is the decisive minimal shader: DXBC is `mov o0.xyzw,v1.xyzw; ret`,
  and postopt AIR returns fragment argument `user(reg1_0)` plus only the pre-existing
  `-1/127500` UNORM bias. No common semantic DXBC opcode exists across the five shaders.
- Correct CBs, textures and argument tables do not prove correct fragment interpolants.
  The first unresolved boundary is now the paired vertex output to fragment input value,
  or shader-local value production before `o0` for the complex variants.
- Do not patch output mapping, blend/RTV, or an arbitrary opcode. Next proof must isolate
  the same identity PSO/draw, compare the vertex `reg1_0` output with fragment `%0`, and
  inject one known nonzero interpolant value before immediate GPU readback.
- Verdict: `reports/phase4-hollow-knight/FRAGMENT-TRANSLATION-CAUSE.md`.

## 2026-07-17 — Exact C0–C3 ladder remains UNKNOWN: readback is not per-draw

- The sealed run matched exact logical PSO `0x9d2b46c27af732f4`, exact
  `ps_bef43b20`, its paired VS, and the indexed draw signature. C2/C3 AIR injection and
  all four physical PSOs compiled successfully; target CPU/JIT fault counts stayed zero.
- The exact draw is draw 2 of 8 in the active Metal render encoder. The current GPU copy
  is legal only after ending the encoder, where six later draws contaminate the result.
  The probe failed closed before readback and did not execute C1–C3.
- C0 black, final-fragment forcing, fragment-input forcing and paired-VS forcing are all
  runtime `UNKNOWN`. Neither original VS value production nor stage-link/interpolation
  is causal yet. Do not apply a semantic fix.
- Required next boundary: a dedicated storage side channel written by the exact draw and
  read after encoder completion, with the same original logical PSO/descriptors/draw.
- Verdict: `reports/phase4-hollow-knight/CAUSAL-LADDER-PS-BEF43B20-RESULT.md`.

## Hollow Knight fragment RGB=0 — exact-draw ladder status (2026-07-17)

The encoder-end readback ambiguity is closed: a default-off slot-28 fragment
storage side-channel captured only exact draw 2/8. C0 wrote 655,360 finite black
pixels, exactly the 1024x640 viewport, proving immediate per-draw coverage.

The causal ladder itself is still unresolved. The next C1 candidate had the
same logical PSO but failed the sealed draw signature comparator, so magenta was
not tested on the same logical draw. The ladder stopped fail-closed; C2/C3 have
no runtime results. Current verdict is UNKNOWN, not VS production, stage-link,
or UNORM causality.

Next evidence: add bounded field-wise expected/observed telemetry to the draw
signature rejection, then perform a fresh sealed run. Do not weaken exact-draw
matching and do not implement a shader translation fix before C1–C3 complete.
Authority: `reports/phase4-hollow-knight/CAUSAL-LADDER-SIDE-CHANNEL-RESULT.md`.

## Hollow Knight ps_bef43b20 ladder drift result (2026-07-18)

Field-wise telemetry identified the C1 rejection as a selector problem, not a
shader result: the next same-PSO candidate changed `index_buffer_offset`,
`base_vertex`, `vertex_bindings_hash`, and `color_load_action`. WineMetal now
skips bounded nonmatching same-PSO candidates and waits for the saved C0 draw
signature; null physical PSO and missing baseline remain fail-closed.

Fresh sealed run `laneA-causal-ladder-drift-fixed-try1-034616` reached C0
BLACK (`655360` viewport pixels), skipped four nonmatching C1 candidates, and
timed out with no exact C1 recurrence. Faults were zero and invalids were zero.
Verdict remains UNKNOWN: C1/C2/C3 did not execute on the same logical draw, so
no shader semantic fix is justified.

Next evidence must use per-control fresh processes with a sealed baseline draw
signature, or choose a repeated draw target.

## Hollow Knight ps_bef43b20 per-process controls ready (build only, 2026-07-18)

The repeated-draw alternative is rejected. The only active design is four strictly
serialized fresh processes over one binary, selected by the default-off
`MACRUNNER_HB_CAUSAL_CONTROL=C0|C1|C2|C3` environment variable.

The former process-local C0 baseline and stage advance are gone. Each process waits
for the checksum-sealed canonical C0 logical signature, comparing 54 semantic fields
and no runtime pointers, then claims one exact occurrence and applies only its chosen
control. Same-PSO or other repeated draws remain nonmatching candidates and cannot be
substituted. Canonical artifact:
`reports/phase4-hollow-knight/PS-BEF43B20-C0-LOGICAL-SIGNATURE.json`.

Focused contracts and ARM64/x86_64 builds passed; build-to-dist bytes and WineMetal
codesign passed. No runtime was performed. C0/C1/C2/C3 are all `UNKNOWN`, forced
magenta remains `NOT_GOLDEN`, and no production semantic fix is authorized. The next
runtime must restore the same immutable cache seed before every process, use matched
save/config/data and child environments except selector/ephemeral paths, take the
exact-draw side-channel readback, clean up and re-run headroom after every process,
and stop immediately if C1 is not MAGENTA.

## Hollow Knight ps_bef43b20 per-process runtime verdict (2026-07-18)

Runtime completed with four strictly serialized fresh processes and one binary. All
accepted runs had `run-contract.json` status `READY`, zero `UNKNOWN`/`BLOCKED`, one
checksum-matched signature artifact, and one exact side-channel completion.

Results: C0=`BLACK`, C1=`MAGENTA`, C2=`MAGENTA`, C3=`MAGENTA`.

Verdict: `C1 PASS + C2 PASS + C3 PASS => original VS value production causal`.
Stage-link/interpolation is not the causal bucket for this draw. UNORM is not declared
causal by this diagnostic. Forced magenta remains `NOT_GOLDEN`; production snapshot is
still disallowed until a semantic VS fix lands and regression/runtime gates pass.

Result artifact:
`reports/phase4-hollow-knight/PS-BEF43B20-PER-CONTROL-RUNTIME-RESULT.md`.

## Hollow Knight paired-VS source-value correction (2026-07-18)

The follow-up exact C0 capture disproved a DXBC-to-AIR translation fault for the
canonical draw. The paired DXBC ends in `mov o1.xyzw, v1.xyzw`; AIR pulls the same
Float4 from slot 16, stride 88, offset 24 and returns it as `user(reg1_0)`.
GPU-visible source data for all four indexed vertices is
`(0,0,0,0x3ca0a0a1)` = `(0,0,0,5/255)`, stable across 14 bindings. Therefore the
zero RGB exists before AIR and the translator correctly preserves it.

No semantic patch was made and the 1800-second production run/snapshot gate remains
closed. Whole-backbuffer cause is `UNKNOWN`; next evidence must address the source
black quad or the earlier content expected behind it, without substituting another
same-PSO draw. Full verdict:
`reports/phase4-hollow-knight/VS-TRANSLATION-FIX-VERIFY.md`.

## 2026-07-18 - Vertex-data boundary closed; scene/bootstrap is next

The exact `ps_bef43b20` quad is source-black by application intent at the D3D
boundary. A fresh 1800-second default-off vertex provenance run matched exactly
one semantic occurrence and proved:

- Unity changes v768 from zero to `(0,0,0,5/255)` before Unmap;
- Map and Unmap use the same returned pointer/allocation;
- DXMT encodes the same GPU allocation and the exact draw reads stride 88;
- IA `reg1` is correctly mapped to slot 0 byte offset 24;
- 601 other encoded records contain nonzero RGB.

The display remained 1024x768 black across 253 window captures. Unity reported
missing Game Manager, GameCameras, and UIManager, with no scene-load marker. The
next evidence target is the managed scene activation/bootstrap path that should
instantiate those objects and provide visible content. Do not patch or re-probe
DXBC-to-AIR VS translation, Map/Unmap, IA binding, or a repeated same-PSO draw.

Current verdict and raw evidence:
`reports/phase4-hollow-knight/VERTEX-DATA-BLACK-CAUSE.md`.

## 2026-07-19 - Return delivery stops before macdrv; verdict UNKNOWN

UnityPlayer statically registers the keyboard through USER32 Raw Input
(`RegisterRawInputDevices` keyboard usage `{1,6}`), with `GetRawInputData` and
`GetRawInputBuffer` as the primary consumption route. DirectInput is not imported;
GetAsyncKeyState/GetKeyState are auxiliary queries.

Fresh zero-input A/A runs reproduced observer readiness and BLACK with zero Return
events. In sealed B, exactly one Return down/up pair was posted to the proven Hollow
Knight PID after `HighlightDefault`, but the target was not frontmost and macdrv
received no NSEvent. Wine enqueue, Unity input, and managed SetLanguage were therefore
all zero. The first observed miss is host/Quartz/focus, but full delivery coverage is
incomplete because the host record lacks target Win32 HWND/key-window/native-tid;
the strict verdict is UNKNOWN and StartManager/EventSystem are not implicated.

Do not investigate ConfirmLanguage, allowSceneActivation, managers, scene loading, or
graphics from this result. A separately authorized follow-up must first seal target
HWND/key-window/focus/native-tid telemetry, then repeat a fresh one-Return A/A/B and
stop at SetLanguage. Evidence:
`reports/phase4-hollow-knight/RETURN-TO-SETLANGUAGE-SEALED-AB.md`.

## 2026-07-19 — Focus-activation A/A+B stopped before admission

The host/macdrv Return observer now seals target PID/CG window ID, Cocoa window
ID to Win32 HWND mapping, foreground/active/focus HWND, `NSApp` active, exact
key/main NSWindow state, and host/macdrv native tids. A default-off diagnostic
activation request uses `NSRunningApplication` for the exact HK PID; Return is
suppressed unless B proves frontmost + exact key window + exact focus HWND.

Focused tests and deterministic build/dist gates PASS. The primary A1/1350 and
authorized fallback A1/1800 both failed before `HighlightDefault`: no on-screen
HK CG window, no `macdrv-window-map`, no mapping/native-tid coverage, and zero
Return posts. The fallback reached Win32 realization for `hwnd=0x2002e` and
Cocoa pointer `0x808acc000` at +189.786s, then timed out. Cleanup is complete,
headroom is 60 GiB, and cache artifacts were preserved.

Strict verdict is UNKNOWN. A2/B were not run because A1 was invalid. Do not
attribute this result to NSRunningApplication activation, window ownership,
CGEvent delivery, Unity mapping, SetLanguage, ConfirmLanguage, or scene
activation. Evidence:
`reports/phase4-hollow-knight/FOCUS-ACTIVATION-SEALED-AB.md`.

## 2026-07-19 — Observer-equivalence artifact A/A+B blocked in preflight

The known-good Return-route A/A manifests require PE `winemac.drv` SHA-256
`7f1281d7ababa0e3fe2305cddb2efedbf5dde9f894012925bb59b058480830c6`.
Those bytes are no longer present in the canonical/sibling/external artifact
trees, Trash, Spotlight candidates, available snapshots, or retained ccache
results. Exact Return Unix and observer bytes were recovered; the current Focus
PE/Unix/observer set was copied and checksum-sealed without rebuilding.

Observer coverage is also not equivalent: both Unix artifacts contain passive
`TRACE_WINSHOW` async-show and order-front markers, but only Focus contains
`macdrv-window-map`. A1/A2/B were not run because substituting/rebuilding the PE
or modifying the old observer would add an unsealed variable. Strict verdict is
artifact causality UNKNOWN. Focus/input/activation remain untouched. Evidence:
`reports/phase4-hollow-knight/OBSERVER-EQUIVALENCE-ARTIFACT-PREFLIGHT.md`.

## 2026-07-19 — Fresh focus-observer baseline build admitted

A new one-binary baseline no longer depends on the missing historical Return
PE. Cached default-off `MACRUNNER_HB_RETURN_ROUTE_FOCUS_MILESTONES` gates exactly
the one window-map and two focus-state observer calls before their state-query
path. Common Return stages and `TRACE_WINSHOW` are unchanged; source audit has
zero uncovered focus-stage calls.

Three focused/regression tests PASS. Two isolated builds from the same sealed
snapshot are byte-identical at PE `3b506d3…` and Unix `65bf6bf…`; the unchanged
managed observer is `ca5a6e…`. Standalone bundle verification PASS after scratch
cleanup. Working dist/cache were not changed. Admission is build-only:
`runtime_authorized=false`, `runtime_executed=false`, NOT_GOLDEN. The prepared
future A1/A2/B table is `FOCUS_MILESTONES=0/0/1`, but no runtime was launched.
Evidence:
`reports/phase4-hollow-knight/NEW-FOCUS-OBSERVER-BASELINE-BUILD-CONTRACT.md`.

## 2026-07-21 — HK Mono array accessor continuation

W03 is preserved as a proven getenv/suspend milestone. Its profiler entrypoint
was not opaque: the W03 log proves init failed only because shipped Unity Mono
does not export macro-only `mono_array_get`.

A fresh scratch-only profiler now resolves exported
`mono_array_addr_with_size` with the exact ABI and typed element-zero access.
Focused source/export gates pass (`25/25` required exports), and two final fresh
x86-64 PE builds are byte-identical at `17082ce1...a43b`.

The sole direct run armed the no-allocation profiler and reached
`HighlightDefault` plus exact UI-thread identity at +957.233s. It then asserted
before array access because the pre-existing source passes `MonoClass *` to
`mono_type_get_object`, whose ABI requires `MonoType *`. No lookup,
SetLanguage/ConfirmLanguage, Present, or capture boundary followed; child exit
was 5. Strict verdict is UNKNOWN/NOT_GOLDEN. No second fix, retry, input,
focus/activation action, production change, or commit was made. Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-MONO-ARRAY-ACCESSOR-RESULT.md`.

## 2026-07-21 — HK Mono class-to-type ABI continuation

A scratch-only source repair now converts the proven `StartManager` class with
exported `mono_class_get_type` and passes the resulting `MonoType *` to
`mono_type_get_object`. A bounded audit of all 13 one-shot Mono signatures found
no remaining ABI mismatch; 26/26 resolved exports are present. Fresh A/B PE
builds are byte-identical at `254619d0...34d90`.

The sole direct run armed at +54.340s, reached `HighlightDefault` and exact UI
thread at +1007.376s, then proved the repaired chain with `exception=0 count=1
exact_one=1`. The prior `mono_class_from_mono_type_internal` assertion is gone.
The first downstream miss is the existing combined parameterless method lookup:
`status=missing-method`; SetLanguage/ConfirmLanguage were not invoked.

Component verdict is class-to-type ABI PASS; overall verdict is
UNKNOWN/NOT_GOLDEN. No retry, second fix, observer, capture, input,
focus/activation action, production change, commit, or GOLDEN label occurred.
Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-MONO-CLASS-TYPE-ABI-RESULT.md`.

## 2026-07-21 - HK StartManager SetLanguage/ConfirmLanguage Sequence

Bounded continuation:
`/Users/timurtoby/Documents/MacRunner/agent-research/HK-CODEX-STARTMANAGER-STRING-SEQUENCE-CONTINUATION-20260721.md`
verified at SHA-256
`d6bdfb853dc61bfbae217919876ce87214abebf5149a09b207f51f47481be454`.

Applied the exact metadata-proven sequence in the existing one-shot observer path:
`StartManager.SetLanguage("EN")` followed by `StartManager.ConfirmLanguage()`.
The source gate, 14-API ABI audit, export gate, and dual deterministic x86-64 PE
build all passed. Product PE SHA-256:
`85d0f1e7d31bf065341d102a2fe456cccea9d18c374e49383773e1691cc30e5a`.

The sole direct run reached the full managed sequence in order:
exact-one StartManager lookup, managed EN string creation, `SetLanguage` invoke
and return, then `ConfirmLanguage` invoke and return. There were zero managed
exceptions and zero runtime rejects before that boundary.

Immediately after `ConfirmLanguage` returned, the first downstream boundary was
a runtime `MEMORY_FAULT` in `mono-2.0-bdwgc.dll` at `rva=0x1c1290`
(`block_pc=0x87ef25c1290`, `next_pc=0x87ef2666259`). Required product captures
were not produced (`png_count=0`), with no Present/readback evidence.

Component verdict is StartManager sequence PASS; overall verdict is
UNKNOWN/NOT_GOLDEN. No retry, second fix, new apparatus, input,
focus/activation action, production change, commit, or GOLDEN label occurred.
Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-STARTMANAGER-LANGUAGE-SEQUENCE-RESULT.md`.

## 2026-07-21 - HK GChandle Lifetime Continuation

The bounded gchandle-lifetime continuation retained the single pinned
StartManager handle for process lifetime and ran once. Build gates passed:
source SHA `cd13b0f55ccc72ffca59b950c30fd1abb0ea3ea8028d020195def2cfa1ab8aa5`,
product PE SHA `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.

Runtime proved the former immediate post-confirm `mono_gchandle_free`
boundary is gone: SetLanguage and ConfirmLanguage returned ok once, retained
handle marker appeared with `retained_handle=412614704`, and there were zero
`mono_gchandle_free`/`free_v2`/`rva=0x1c1290` records.

Product pixels remain unproven. Existing passive captures at pre-sequence and
post-confirm +10/+30/+60s were BLACK (`non_black_px=0`, `colorful_px=0`) against
visible window id 7413. The first post-confirm blocker is now
`UnityPlayer.dll rva=0xe10256 out=UNSUPPORTED_OPCODE`, bytes prefix
`66 0f 6f 05 62 40 ee 00 66 0f 72 d1 08 0f 57 ca`.

Overall verdict is UNKNOWN/NOT_GOLDEN: gchandle-lifetime component PASS,
product visible-pixel PASS not achieved. No retry, input, focus/activation,
scene forcing, working-dist install, production prefix mutation, commit, or
GOLDEN label occurred. Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-GCHANDLE-LIFETIME-RESULT.md`.

## 2026-07-21 - HK Unity MOVNT Corridor Managed Runtime Deployment

After the Unity `MOVNTDQ` corridor family fix, a bounded runtime deployment used
the sealed native ntdll pair and the accepted no-allocation managed actuator
without source changes or rebuilds. The isolated dist clone staged:
`ntdll.so=9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7`,
`ntdll.dll=3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`,
and managed actuator
`7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.

The actual run proved prefix sync copied the accepted managed DLL into
`system32`, but stopped before the HK Wine child. `run-contract.json` was
`BLOCKED` on `application.save_snapshot_manifest_sha256:path_absent`; there was
no `final-child.json`, no `Mono path`, no `observer-init`, no managed call, no
Present/readback, and no pixel capture. Scoped cleanup removed the prefix and no
HK/Wine process remained.

Verdict is BLOCKED/UNKNOWN/NOT_GOLDEN. This does not refute the Unity MOVNT
family fix and does not identify a next JIT wall. Minimal next-run correction is
to pass the existing immutable save authority:
`MACRUNNER_RUN_CONTRACT_SAVE_PATH=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/phase4-hollow-knight/mono-4ee14b-inline-force-20260715-1049/immutable/contract-save`.
Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-UNITY-JIT-CORRIDOR-MANAGED-RUNTIME-RESULT.md`.

## 2026-07-22 - HK Native NTDLL Binary Bisect

The requested pixel-first native ntdll binary bisect is now complete as
`NTDLL_PAIR_EXONERATED/NOT_GOLDEN`. ARM A reproduced the old C0 baseline with
the immutable OLD ntdll pair: `CreateSwapChainForHwnd rc=0`, `GetBuffer>0`,
`Present/Present1>0`, `UNSUPPORTED=0`, `MEMORY_FAULT=0`, `reject=0`, bounded
timeout rc 124, and first-Present pixel capture remained black.

ARM B staging was correct before launch: the disposable dist clone differed from
ARM A in exactly two `lib/wine` entries, `aarch64-unix/ntdll.so`
`9a3f20b426adeeb86d70c06d0b7cfc9e687838e6eace5e43c282a19eb0363fc7` and
`aarch64-windows/ntdll.dll`
`3863660ac0b5f57c106d610d6a88d897a89a6c0ab626f890e66b4d572f02de4d`.

The first B launch attempt remains preserved as `INVALID_PRE_WINE`: the env
import jq expression failed, `mr-run` wrote a `BLOCKED` run-contract and exited
before Wine.

The explicitly authorized corrected B execution fixed only the environment
import expression and reused the same staged B root. It produced
`run-contract READY`, blockers `0`, final-child present with the same 113-name
environment set as A, `CreateSwapChainForHwnd rc=0`, `GetBuffer=1`,
`Present=745`, `Present1=745`, `UNSUPPORTED=0`, `MEMORY_FAULT=0`, `reject=0`,
and first-Present pixel capture remained black. The native ntdll pair is
therefore exonerated for the GetBuffer/Present disappearance. Next isolation
target is `winemac.so` `65bf...` -> `02b4...`. Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-NATIVE-NTDLL-BINARY-BISECT.md`.

## 2026-07-21 - HK Unity MOVNT Runtime Contract Correction

The separately authorized runtime contract-correction handoff was verified at
SHA-256 `78d950dfe92c08be50a2dba22a53484d03720ca0e485561febf37fa60a04199f`.
The only launch correction was
`MACRUNNER_RUN_CONTRACT_SAVE_PATH=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/phase4-hollow-knight/mono-4ee14b-inline-force-20260715-1049/immutable/contract-save`.

The corrected run used the same sealed ntdll pair and no-allocation managed
actuator. `run-contract.json` was READY with zero blockers, prefix sync copied
the accepted managed DLL into `system32`, and final-child capture had zero
secret-bearing environment names.

Runtime crossed the former Unity corridor and product milestones: profiler
armed with `allocations=excluded`, `CreateSwapChainForHwnd rc=0`,
`SetLanguage("EN")` and `ConfirmLanguage()` both returned ok, managers
`GameManager`/`UIManager`/`GameCameras` were created, and automatic level start
was reached. Old `MOVNTDQ`, `UnityPlayer+0xe1028d`, `UNSUPPORTED`, and
`MEMORY_FAULT` counts were zero through bounded timeout rc 124.

Component verdict is `CORRIDOR_FIX_PASS`; overall product verdict remains
`UNKNOWN/NOT_GOLDEN` because pre-sequence capture was `NO_WINDOW`, post-confirm
+10/+30/+60 captures were black (`non_black_px=0`), and Present/readback markers
were absent. No retry, source change, rebuild, commit, production deployment,
PIXEL_PASS snapshot, or GOLDEN label occurred. Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-UNITY-JIT-CORRIDOR-MANAGED-RUNTIME-RESULT.md`.

## 2026-07-22 - HK Winemac One-File Binary Bisect

The accepted corrected-ntdll baseline was not rerun. A fresh disposable arm
changed exactly one typed dist entry, `aarch64-unix/winemac.so`, from
`65bf6bf9b9d3e1030e1331a3b59b5c56282885fa7ac5469a617b26ff8546d0bc`
to `02b4d94ecb5827ea331decccb846f1bf6ff97df632ea28f6b40951794f8c9604`.
The preserved pre-B cache matched `22606624` bytes, file SHA `f5faa1...`, and
typed inventory `26aced...` before the one authorized run.

Runtime absolute facts are positive: run-contract READY/blockers 0,
`CreateSwapChainForHwnd rc=0`, `GetBuffer=1`, `Present=448`, `Present1=448`,
faults/rejects/input/focus/activation all zero, and timeout rc 124. First-Present
and bounded-late captures were both BLACK. Prefix cleanup completed and no
HK/Wine process remains.

Strict causal verdict is `UNKNOWN / ENVIRONMENT_DRIFT / NOT_GOLDEN`, not
`WINEMAC_02B4_EXONERATED`. Effective final-child `WINEDLLPATH` had one extra
duplicate DXMT-root entry because the imported corrected-B final-child value
already contained the root and `mr-run.sh` prepended it again. Wall time also
stretched to 4762s despite the inner 1200s timeout rc, with the discontinuity
unclassified. The run proves `02b4` can submit Presents, but not as a zero-drift
one-file A/B causal result. No retry is authorized or performed. Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-WINEMAC-BINARY-BISECT.md`.

## 2026-07-22 HK post-scene guest-loop mapping: UNKNOWN capture miss

Mapping runtime for
`docs/CODEX-TASK-hk-post-scene-guest-loop-pair-and-fix.md` executed once from
the accepted post-scene JIT-spin checkpoint. Live admission passed with
116/116 child environment entries, child env stream
`5b38b392b178a90bb350426f0a2aa9ef8a0527ecd3dd4a606c7df2161f02f6df`,
WINEDLLPATH hash `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`,
and live actuator `7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`.

The run reached `GameManager`, `UIManager`, `GameCameras`, and one exact
`Performing automatic level start` marker at the timeout tail. Post-marker
`GetBuffer`, `Present`, `OMSetRenderTargets`, `Draw`, and `Dispatch` deltas
were all zero; faults/rejects/UNSUPPORTED stayed zero. The single macOS
`sample` found `Thread_5192760` with both `hb_jit_helper_exec_two_block_loop`
and `mem_read`, but the single LLDB attempt stopped on unrelated
`EXC_BAD_ACCESS` in `wine_xinput_hid_update`, not the restricted one-shot helper
hit. Per task contract this is `UNKNOWN_CAPTURE_MISSED`; no IR pair, fixture,
source fix, validation runtime, snapshot, or commit followed.

Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-RESULT.md`.

## 2026-07-22 HK corrected guest-loop capture: UNKNOWN identity drift

Corrected LLDB signal policy was proven build-only before HK: LLDB passed
`SIGSEGV/SIGBUS/SIGILL/SIGUSR1/SIGUSR2` to the inferior, the throwaway fixture's
`SIGSEGV` handler returned, the exact thread-restricted helper breakpoint hit,
capture wrote valid headers/IR, and timeout-detach proof passed.

The single authorized corrected HK mapping runtime then failed live admission:
child env count was `129`, not the required `116`. `WINEDLLPATH` remained exact
at `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`, product
bytes matched, blockers were zero, but 13 Codex/terminal parent env names had
leaked into `final-child.json`. The run was stopped fail-closed with scoped
`wineserver -k`; marker count was `0`. No retry, capture, IR pair, fixture,
source fix, validation runtime, snapshot, commit, or GOLDEN followed.

Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-CAPTURE-CORRECTED-RESULT.md`.

## 2026-07-26 - Sidechannel grid ready

The Present-side frame-dump path remains `FRAME_DUMP_REQUESTED_NO_COMPLETION`:
requests occurred, but its separate blit/completion path produced neither RGB
statistics nor files. The independently working C0 causal sidechannel remains
the trusted pixel witness. Its existing shared-buffer completion route now has
a default-off full-coverage RGB-grid summary gate; no new Metal completion or
blit route was added. Published Unix bridge SHA-256:
`1b2ba54a9f3580097dc15931de24bb12575b192d0707d6e92ba98a21ad807203`.
See `reports/phase4-hollow-knight/HK-SIDECHANNEL-GRID-IMPL.md`.

## 2026-07-26 - Sidechannel Grid Runtime Result

The one cold 4,200-second sidechannel-grid run reached automatic level start,
recorded 41,807 indexed draws and 857 Present/Present1 pairs, and had no
MEMORY_FAULT, UNSUPPORTED, reject, or HUP record. Its C0 sidechannel completion
classified all 655,360 written values BLACK with zero nonzero and nonfinite
samples. It did not cover the claimed full grid: 131,072 of 786,432 entries
remained sentinel. Thus the result is `PARTIAL_BLACK`, not a verified
full-frame `WALL_B_PROVEN` conclusion. The next step must repair or account for
the coverage contract before using this witness for an offscreen/matrix root
cause claim. See
`reports/phase4-hollow-knight/HK-SIDECHANNEL-GRID-RUN-RESULT.md`.

## 2026-07-26 - C1 Transport Control Invalid

The C1 magenta artifact was correctly published and actual-child/prefix-sync
proof passed, but the copied evidence invoker executed `mr-run.sh` with `4200`
instead of the authorized `2400` seconds. No Present or C1 completion occurred
before the over-budget owned child was scoped-terminated. This is not a
magenta/black transport result and no retry was performed. The grid tool also
still has the independently proven `coverage=full` versus sentinel mismatch.
See `reports/phase4-hollow-knight/HK-GRID-MAGENTA-CONTROL-RUN-RESULT.md`.

## HK Frame-Dump Early Gate (2026-07-25)

The direct DXMT frame-dump artifact set was loaded and verified in the actual
prefix, but the one authorized run was stopped at +120 s because it had not
reached `Present`. No dump was scheduled or written, so there is no valid
backbuffer color verdict. The observed boundary was swapchain setup plus one
Clear and a Unity Gfx-command storm, before any Draw or Present. See
`reports/phase4-hollow-knight/HK-FRAME-DUMP-RUN-RESULT.md`.

## HK Frame-Dump Full Budget (2026-07-26)

The corrected full-budget run reached four real Present-side dump requests but
never scheduled or completed Metal readback, emitted no RGB statistics, and
wrote no dump file. This is now an instrument boundary after request, not a
warm-up timing failure. The direct-backbuffer color remains unmeasured; the
separate existing C0 sidechannel reported only its pinned target as black. See
`reports/phase4-hollow-knight/HK-FRAME-DUMP-RUN-RESULT.md`.

## 2026-07-25 - HK direct backbuffer evidence is build-ready

DXMT now has a bounded, opt-in backbuffer readback for the next authorized
measurement: `MACRUNNER_DXMT_FRAME_DUMP=1` captures Present frames 1-5 and
every 200th as raw RGBA, with `min_rgb`, `max_rgb`, `mean_rgb`, and
`nonzero_fraction` in `dxmt-frame-dump:` records.  It is passive: the GPU copy
and file write complete asynchronously after the command buffer, with no
shader/state/input/focus modification or synchronous wait.  No runtime was
performed in this implementation step; see
`reports/phase4-hollow-knight/HK-DXMT-FRAME-DUMP-IMPL.md`.

## 2026-07-23 — verifier de-strictification implemented; runtime not retried

The post-level milestone correction reached `final-child.json` with the correct
117-name set (`baseline 116 + MACRUNNER_HB_POST_LEVEL_MILESTONE_TRACE=1`) but
the old admission verifier stopped on four launch-context value differences:
`DYLD_FALLBACK_LIBRARY_PATH`, `DYLD_LIBRARY_PATH`, `SHLVL`, and `_`. This was
`TRACE_INVALID_ENV_DELTA`, not a product observation.

The bounded policy in
`docs/CODEX-TASK-verifier-destrictification-declared-env-delta-content-mode.md`
is now implemented:

- declared observer deltas are regex-bounded, unique, and capped at four;
- undeclared names and all non-agnostic value changes remain fail-closed;
- the fixed seven-name context set is name-present/value-agnostic;
- staged-tree integrity remains strict, while integrity-identical mode drift is
  normalized only on the disposable clone.

All 11 new acceptance fixtures and all 6 existing capture/runner tests pass.
Replay of the stopped 117-name evidence passes with exactly the four expected
agnostic mismatches and zero strict mismatches. Historical evidence, staging,
product bytes, prefixes, caches, and engine/game code were untouched. No
milestone-trace runtime retry has occurred after this verifier-policy change.

## 2026-07-23 HK Mono main-thread capture correction: not Mono hash-loop

`docs/CODEX-TASK-hk-mono-jit-main-thread-capture.md` executed once with the
mode-normalized staged clone restored by only the two confirmed mode corrections
and then verified `4732/4732`.

The new topology selector retracted the prior `Thread_6099551` import-thunk
candidate. Offline fixtures passed, and both live post-marker samples selected
the same unique full-runtime thread, `6164462`.

The one allowed LLDB attach stopped on that selected thread in
`hb_jit_helper_exec_two_block_loop`, but the unchanged `hk-mono-cycle` validator
failed before any mutation: expected Mono-loop RVA byte windows did not match,
table invariants failed, and the captured guest PC was `0x87efd2441c0`.

Current boundary: `MAIN_RUNTIME_NOT_MONO_HASH_LOOP`. This run does not prove a
`MonoDomain::jit_code_hash` cycle and did not perform a memory cut, source edit,
retry, commit, or pixel snapshot.

Evidence:
`reports/phase4-hollow-knight/laneA-mono-jit-main-thread-capture-20260723-114833`.
Report:
`reports/phase4-hollow-knight/PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`.

## 2026-07-23 HK main-loop generic capture: unmapped guest PC

`docs/CODEX-TASK-hk-main-loop-generic-block-capture.md` executed once with the
same mode-normalized clone contract and immutable 2104 generic LLDB capture.

The run reached automatic level start and selected one unique full-runtime
thread in both passive samples: `6220697`. Generic LLDB capture passed with no
read errors and no writes. Captured `ctx->pc` was `0x87efde0f460`, with `x1 ==
x2`, so the helper was executing the same block as both entries.

The exact decoded block is a bounded scalar accumulation loop:
`lea eax,[r10+rdx]; inc edx; addss xmm0,[r9+rax*4]; cmp edx,r8d; jl back`.
Captured values were `rdx=1`, `r8=2`, so the recorded `jl` would not be taken
after this iteration. IR matched the decoded instructions; no exact
HyperBridge execution mismatch was proven.

Module resolution did not find a PE owner for `0x87efde0f460` in captured
relocation records. Current verdict: `UNMAPPED_GUEST_PC`, not a source fix
target yet.

Added D3D stream observation: on the same run the existing stdout D3D stream
recorded `Present=48`, `Present1=48`, and `Draw*=0`. This does not support the
"many black scene draws" branch and does not prove a single source-black quad.

Evidence:
`reports/phase4-hollow-knight/laneA-main-loop-generic-block-capture-20260723-124039`.
Report:
`reports/phase4-hollow-knight/PIXEL-FIRST-MAIN-LOOP-GENERIC-CAPTURE-RESULT.md`.

## 2026-07-23 HK Mono jit_code_hash runtime correction: pre-Wine identity stop

The accepted 2104 launcher was copied and only `RUN_DIR`/`PREFIX` were changed,
per `docs/CODEX-TASK-hk-mono-jit-hash-cycle-runtime-correction.md`. The exact
immutable `STAGED_DIST` literal preserved by that launcher is absent:

`reports/phase4-hollow-knight/laneA-post-scene-passive-stack-20260722-143030/staged-dist`

The foreground captured launch exited before Wine/HK with `rc=1` and stderr
`staged dist missing`. No `final-child.json`, `run-contract.json`, LLDB capture,
memory write, shader C0/C2/C3 rerun, black-frame fork, or product measurement
occurred. Current boundary is launch identity/evidence availability, not
Mono/hash-chain causality or shader value production.

Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`.

## 2026-07-23 HK mode-normalized staged clone: wrong helper state

The mode-normalized disposable clone matched historical `STAGED-TREE.json` after
changing exactly two owned-clone entries from `0755` to `0555`; strict verifier
passed `4732/4732` with `0` mismatches. The single foreground runtime then
passed live identity (`READY`, env `116/116`, raw `WINEDLLPATH` `cce9...`,
profiler SHA `7c620...`) and reached `Performing automatic level start.` with
`CreateSwapChainForHwnd rc=0`, `GetBuffer=1`, `Present=48`, `Present1=48`,
`UNSUPPORTED/MEMORY_FAULT/reject=0`.

The approved Mono hash-cycle LLDB attach used the accepted first post-marker
sample candidate, `Thread_6099551`, but validation failed: captured guest PC
`0x87ef2945bcb` was not the Mono `mono_internal_hash_table_lookup` loop state,
RVA byte/caller/callback/bucket checks failed, and no direct chain traversal was
valid. No memory write or post-cut measurement occurred. Current boundary is
wrong live helper-state selection/capture, not a proved `jit_code_hash` cycle
and not shader value production.

Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`.

## 2026-07-23 HK staged-dist restoration: pre-Wine mode mismatch

The staged-dist restoration handoff allowed one additional launcher constant:
`STAGED_DIST` now points to
`reports/phase4-hollow-knight/laneA-shader-value-production-c1-20260723-try1-064000/staged-dist`.
Before Wine, the replacement was compared against the historical
`STAGED-TREE.json` for all 4,732 entries.

Entry count matched, but typed-entry verification failed on two mode fields:
`lib/wine/x86_64-windows` and
`lib/wine/x86_64-windows/mono-profiler-hk_language.dll` were `493` instead of
historical `365`. File bytes for the managed actuator matched
`7c62033f7315aaf14f31783c32b0ddb46a14757a3fb4e0e248c291c47b4a570b`, but mode
equality was part of the required proof, so the task stopped before launcher,
Wine, HK, LLDB, mutation, or pixel measurement. Boundary is evidence/restoration
mode drift, not Mono hash-chain or shader causality.

Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-MONO-JIT-HASH-CYCLE-CAUSAL-RESULT.md`.

## 2026-07-22 HK hash-retraction guest-loop capture: stable Unity loop

The previous `UNKNOWN_IDENTITY_DRIFT` was retracted: the `6ae1...` digest was a
noncanonical joined-newline hash of `WINEDLLPATH`, while the raw child-env value
matched the accepted template at 900 bytes with SHA-256 `cce9...`.

One fresh 2400s runtime then passed live admission and reached
`Performing automatic level start`. Passive sample selected `Thread_5395781`
with both `hb_jit_helper_exec_two_block_loop` and `mem_read`; the accepted LLDB
supervisor hit the exact thread-restricted one-shot breakpoint and captured:

- `ctx->pc`: `0x87ef2470d79`
- first block: `cmp rax,rsi; je 0x87ef2470d9e`
- second block: `mov rcx,rbx; call qword ptr [rdi+0x10]`
- following window: `mov rbx,[rax]; test rbx,rbx; jne 0x87ef2470d73`
- post-marker `Present/GetBuffer/Draw/encoder`: `0/0/0/0`
- faults/rejects/UNSUPPORTED: `0/0/0`

Captured guest bytes and IR agree, so no decoder/execution-family defect is
proven and no source patch or validation runtime followed. Current boundary is
the Unity bootstrap state transition needed to exit that list/callback loop, not
a new opcode fix.

Evidence:
`reports/phase4-hollow-knight/laneA-post-scene-guest-loop-capture-hash-retraction-20260722-2104/GUEST-LOOP-CAPTURE-ADJUDICATION.json`.

## 2026-07-22 HK env-clean guest-loop capture: UNKNOWN identity drift

Env-clean continuation applied only the run-local launcher correction
`env = load_child_environment()` before `os.execve`. Phase 1 static proof passed
the 116-name clean set and removed the 13 inherited Codex/terminal names, but
the single authorized runtime failed live admission before marker/LLDB.

Live child evidence:

- child env names: `116/116`
- forbidden 13 names: `0`
- child name hash:
  `9793680dfb7541b522811065ad2f1a6693cb0b6d64147366288bfbe6014ef532`
- live child `WINEDLLPATH` hash:
  `6ae1e2285c346316c822b24c8c45afb2cdc1e76ad2a519a0b5b53dd0bc0449d1`
- Phase 1 sealed expected `WINEDLLPATH` hash:
  `cce9f264e671dcf4d6cf22894a40879802eb6ba1b8b941d1b9a8a78f6cc74432`
- run-contract save-path variable in child env: absent
- marker count: `0`

The live `WINEDLLPATH` hash matched the imported 116-name source template, not
the Phase 1 expected hash, so the admission proof itself is inconsistent with
actual `mr-run.sh` child semantics. The run was stopped fail-closed with scoped
`wineserver -k rc=0`; owned prefix is absent, disposable staged clone removed,
cache preserved, and HK/Wine residue is zero. No retry, capture, source fix,
validation runtime, snapshot, commit, or GOLDEN followed.

Evidence:
`reports/phase4-hollow-knight/PIXEL-FIRST-POST-SCENE-GUEST-LOOP-CAPTURE-CORRECTED-RESULT.md`.
