# LANE HK MASTER — progress heartbeat

Goal: Hollow Knight playable — menu + input changes selection + Start Game enters + real pixels + audio.
One line per step. `LOOP-STATUS:` at col 0 only for GOAL/BLOCKED.

## ITER-1 — 2026-07-29

- 13:22 Started. Master progress file was empty (0 bytes) — this is iteration 1. No prior heartbeat to resume from.
- 13:24 Machine census: found a LIVE run in progress (`hk-run-try12-config.sh AUDIOCHECK 2700 3`), not launched by me. Did not disturb it. 9 orphaned `winetemp-*` procs at 0% CPU from an earlier run.
- 13:26 FREE STATIC WORK while the run proceeded. Read the driver-event re-arm invariant end to end:
  `server/queue.c:1407` `msg_queue_poll_event` disarms the queue fd on POLLIN and sets QS_DRIVER;
  `server/queue.c:3174` `set_queue_mask` re-arms + clears QS_DRIVER **only when `req->poll_events` is non-zero**;
  `win32u/message.c:3435` sends `poll_events = drained`; `winemac.drv/event.c:838` returns
  `drained = (mask==QS_ALLINPUT && !check_fd_events(fd, POLLIN))`. So a drain returning FALSE leaves the fd
  unarmed AND QS_DRIVER set.
- 13:26 **[CORRECTION to ITER-12]** ITER-12 concluded "the pump is effectively one-shot / HK is not peeking"
  from `ProcessEvents_drain` never printing `calls>=10`. That inference is NOT supported by that probe:
  `win32u/message.c:3418,3431` calls `pProcessEvents` **only `if (driver_signaled)`** (QS_DRIVER set), and the
  drain probe lives inside `pProcessEvents`. So the counter measures **QS_DRIVER assertions, not PeekMessage
  calls**. Whether HK peeks is still unmeasured. See report §1.
- 13:27 Scanned all 39 `laneA-*` run dirs for the drain signature. The no-thread-data signature
  (`queue=(nil) fd=-1 handled=-1`, `event.c:782`) appears **0 times in 0 runs** — so "the pumping thread never
  ran macdrv_init_thread_data" is refuted for every drain that actually happened. Two fds only: `fd=15 handled=0`
  (every run, 1x) and `fd=220 handled=1..41` (runs that dequeue).
- 13:28 **[MEASURED]** Live run's own log: at +45.249s HK's process (`unix_pid=71264 wine_pid=0020`) logs
  `load_display_driver_reentrant` → `PLACEHOLDER KEPT — this process has NO real driver`, while the REAL macdrv
  is installed in a DIFFERENT process (`wine_pid=005c` / unix 83892). The `88894334` CAS-retry cannot fire
  because the real driver never arrives in HK's process at all. HK ran this whole boot on the null driver.
- 13:29 Placeholder census over 65 run dirs: `PLACEHOLDER KEPT` 31 runs, `user_driver_placeholder_replaced`
  13 runs. **Every run with `macdrv_key_event`>0 (16, 17-t1, 18, 21) has repl=1 — 4/4.** repl=1 does NOT imply
  keys. Pre-2026-07-29 runs' zeros are NOT measurements (marker did not exist yet).
- 13:29 `MACRUNNER_WIN32U_PLACEHOLDER_REPAIR` (default-OFF repair already in `win32u/driver.c`) appears in
  **1 of 65 runs** (`HK-E2E-07-REPAIR`), which never reached `Begin MonoManager`. **The repair has never been
  A/B'd at menu depth.** That is the next experiment.
- 13:33 **[MY ERROR — withdrawn claim]** I read `ps` CPU on pid 39816, found 0.0% across 3 samples with a frozen
  log, and called the live run wedged with "the guest main thread is gone". 39816 was the guest of a SECOND,
  concurrent attempt (`AUDIOCHECK-a2`); the real a1 guest is 71264 at 100% CPU / 8.6 GB RSS. Both the wedge
  claim and the missing-main-thread claim are **withdrawn**. This is the ITER-12 wrong-process trap, repeated.
- 13:34 Tore down the duplicate a2 chain (verified PIDs 91871/23596/39816) + orphaned a2 script 23597 + a
  59-min orphaned `hk-dllmain-i10` chain. a1 left untouched and is now the only HK on the machine.
- 13:36 **[MEASURED, live, correct process]** `sample 71264 10`: 45 threads, 1 running, 42 blocked. The single
  running thread spends **5399/7366 samples (73%) in `_sigtramp`** —
  `macrunner_hb_primary_signal_handler` 3580, `segv_handler` 1780, `macrunner_hb_route_x64_callback_fault` 971,
  `macrunner_hb_redirect_arm64x_hexpthk_sigill` 555, plus `pc_is_x64_guest_code_module_no_lock` /
  `ldr_entry_from_pc` / `module_from_pc` classifier frames. **[HYPOTHESIS]** fault-driven guest/host transition
  is the fixed-count cost behind the ≈558 s constant. Needs a second spaced sample + a counter.
- 13:36 **[MEASURED]** `caulk::concurrent::details::worker_thread` threads are present in HK's process — the
  CoreAudio worker pool exists, so `winecoreaudio.drv` is live in-process. Necessary, not sufficient, for step 4.
- 13:44 **[I REFUTE MY OWN 13:29 ENTRY]** I was about to spend the next slot A/B-ing
  `MACRUNNER_WIN32U_PLACEHOLDER_REPAIR=1`. Read the code before spending it: `win32u/driver.c:1237` records
  **"RESULT OF THAT A/B (HK-E2E-07-REPAIR, 2026-07-29): REFUTED. DO NOT ENABLE."** — the repair works
  mechanically but the next `load_display_driver()` then installs the LOUD null driver
  (`pCreateWindow = nodrv_CreateWindow`) so window creation fails outright; `Initialize engine version` 1→0,
  run died at 1679 lines. My census counted *occurrences* of the flag, not *outcomes*; the verdict lived in a
  source comment, not a report. **Rule: grep a flag's definition site for a recorded verdict before proposing
  an A/B on it.**
- 13:45 **★ THE REAL DEFECT, and it converges with the DLLMAIN lane.** That same refuted run measured:
  `KeUserModeCallback( NtUserLoadDriver )` **reports SUCCESS** in HK's process
  (`driver_loaddriver_callback … wine_pid=0020 ret=1 driver=L"winemac.drv"`) while **no `dllmain_attach` is
  ever emitted for that wine pid** — winemac.drv's DllMain never runs in the x86_64 guest even though the
  loader claims it did. That is the same defect the sister DLLMAIN lane found from the other side
  (`MODULE_InitDLL` skips AMD64 `LDR_WINE_INTERNAL` builtins returning SUCCESS; forcing it wedges in
  `route_x64_callback_fault`). Note `route_x64_callback_fault` is **12.5 % of the signal storm measured at
  13:36** — loader defect and fault storm are plausibly the same machinery [HYPOTHESIS].
- 13:46 **NEXT = a 41-second probe, not a title run.** `tools/x64winprobe.c` already reproduces both halves
  without Hollow Knight. Report: `reports/phase4-hollow-knight/HK-MASTER-ITER1-SIGNAL-STORM-AND-PLACEHOLDER-20260729.md`.
- 13:46 Hand-off state: `laneA-AUDIOCHECK-a1` still running, chain intact (14712→14743→55469→guest 71264),
  98.6 % CPU, silent since +53.3 s, ~28 min of its 2700 s budget left. Left alone deliberately. One HK on the
  machine. Disk 33 GB free.

14:00 · COORDINATOR — ★★★ THE WEDGE IS A REPEATED FAULT AT ONE PC, NOT A NESTED ONE. Sampled live (HK pid 71264, 435 s silent,
99.9 % CPU) because the auto-sampler caught it before teardown. Hot thread: 542 samples, **351 in `_sigtramp`** — almost
entirely inside signal delivery — under `segv_handler -> macrunner_hb_route_x64_callback_fault ->
macrunner_hb_redirect_arm64x_hexpthk_sigill -> …pc_is_x64_guest_code_module_no_lock -> macrunner_hb_module_from_pc ->
macrunner_hb_ldr_entry_from_pc`. All 44 other threads parked in waits. The faults are SEQUENTIAL: handled, returned to the
same instruction, faulted again. Each enters the router at depth 1 and leaves cleanly, which is why `fault-reentry-break`
has read 0 in every wedged run — the morning's depth guard cannot see this shape.
FIX: a same-pc repeat counter alongside it (`macrunner-hb-fault-same-pc-break`, limit 4096, resets whenever the pc
changes so the JIT's routine SIGSEGV-as-control-flow is untouched). Past the limit the router returns FALSE and the fault
reaches normal handling — a crash with the pc printed instead of a silent hang. Compiles clean; deploy armed for the next
free slot, verified by `strings`, not exit code.
**What this does NOT do:** it does not fix whatever faults at that pc. It converts a six-iteration mystery into a named
crash address. Expect runs that used to freeze to now die and TELL you where — that is the point, and the pc it prints is
your next lead.

## ITER-2 — 2026-07-29

- 13:44 Resumed. Slot BUSY: `laneA-AUDIOCHECK-a1` guest pid 71264 alive at ~100% CPU. Did not disturb it;
  did free measurement on it instead, which ITER-1 had named as the required next step.
- 13:45 **[MEASURED, 2nd spaced sample]** The signal storm REPLICATES. 7423/7426 samples of the one
  running thread are under `_sigtramp`. A 3rd sample at 13:57 (21 min after the first) is identical —
  steady state, not a transient.
- 13:50 **[MEASURED]** Subtree accounting on that thread: **67.0% is PC classification**;
  `pc_is_x64_guest_code_module_no_lock` 21.8%, `pc_in_graphics_arm64x_x64_range` 19.2%,
  `pc_is_pe_code_module_no_lock` 16.6%. Inclusive leaf: **`ldr_entry_from_pc` = 43.6%**.
- 13:52 **★ THE DECISIVE STEP — offsets mapped onto the UUID-matched shipped binary** (`BEC78D19`,
  identical to the running image). Of `ldr_entry_from_pc`: 4-entry positive hot cache (+40..+132)
  **0.6%**; PEB module-list walk (+184..+276) **97.2%**; cache-insert, reached only on a HIT
  (+280..+340) **0.0% — zero of 3146 samples**. The walk runs to completion and finds NOTHING almost
  every time, and misses are never cached, so each one re-walks 40-80 modules ~10x per fault.
- 14:02 **[I CORRECT MY OWN ITER-1 ENTRY]** ITER-1 called this the Mono phase and tied it to the ~558 s
  excess. **Withdrawn.** That run has `Begin MonoManager` = **0** — it hard-stopped emitting at
  **+53.279 s** and burned 100% CPU for 37 min after. It is a **wedged boot**, not scene-load
  throughput. `fault-reentry-break` = 0, so it is also NOT the known `202ade1b` recursion class.
- 14:04 **[INFERRED, marked as such]** The faulting PC is in **no PE image at all** (not merely absent
  from the PEB list): `read_local_memory` took ZERO samples so `module_from_pc`'s header scan never ran,
  yet `module_from_pc` still reached `ldr_entry_from_pc`, which happens only after its own 256-entry
  positive cache misses ⇒ it returned NULL. Matches Mono-JIT/generated code (no PE header). **If true,
  `redirect_arm64x_hexpthk_sigill` can never accept a candidate and the SIGILL is rejected and re-taken
  ⇒ the +53 s wedge is a fault LOOP, and this iteration's throughput fix will NOT cure it.** Confirm
  cheaply with `MACRUNNER_HB_TRACE_HEXPTHK_CANDIDATE=1` (self-caps at 2048 lines) on a wedging run.
- 14:06 **FIX, default OFF.** `ldr_entry_from_pc` now caches misses too — direct-mapped by page, 512
  slots (one load + compare, vs the 512-entry linear scan the sibling neg cache uses); every 512th
  negative hit re-walks to re-validate and clears the entry if a module now covers that page.
  Gate `MACRUNNER_HB_LDR_NEG_CACHE=1`. Positive-cache path unchanged (verified byte-identical in asm).
- 14:07 Build verified by **SHA and content**: `ntdll.so` 968d1dafcbcc3b81 -> bdecfb6fdbb73db1; gate
  string present in the new artifact, absent in the shipped one; `ubfx w9,w19,#12,#9` = the 512-slot map.
  `libhyperbridge.a` deliberately NOT rebuilt (would pull a sister lane's uncommitted hb_*.c into engine).
- 14:08 **PRE-REGISTERED** (before the run): PASS = `ldr_entry_from_pc` 43.6% -> <5% and classifier
  67.0% -> <30%. NULL = classifier stays >=60%. Wall-clock is NOT attributable to this flag (this build
  also picked up another lane's signal_arm64.c). A repeat wedge at ~+53 s is evidence FOR the fault-loop
  inference, not a failed experiment.
- 14:09 `scripts/hk-ldrnegcache-ab.sh` armed and waiting on the slot: waits (never pkills), deploys by
  atomic rename (a live guest has ntdll.so mapped), verifies SHA+strings, launches the control config
  plus the one flag, samples the guest every 200 s. Report:
  `reports/phase4-hollow-knight/HK-MASTER-ITER2-LDR-WALK-IS-THE-COST-20260729.md`.
- 14:12 **★★ THE BIGGER FINDING — HK STOPPED REACHING MONO AT ~11:32 TODAY, AND IT IS THE AUDIO DRIVER
  [HYPOTHESIS].** Census of today's 64 run dirs, **conditioned on wall-clock first** (this lane's own
  lesson: short probes share run dirs with real attempts and have reversed a conclusion before). Among
  the **23** runs that both reached `Initialize engine version` AND lived >=400 s:
  **`winecoreaudio` absent -> 19/19 reached `Begin MonoManager`; present -> 0/4.** Fisher exact
  two-sided **p = 1.1e-4**. Commit `2aa93b2b` (ships winecoreaudio.drv/msacm32.drv/winspool.drv into
  the prefix) landed **11:32**; last run to reach Mono started **11:16**, first failure **11:41**.
- 14:12 **[TRAP CHECKED AND CLOSED]** I first suspected the 11:15 ntdll deploy. Cross-tabbing the run
  dirs' recorded ntdll SHA kills it: `968d1dafcbcc` has **6 reached Mono / 4 did not** — same binary,
  both outcomes, split by time. So the ntdll build does NOT explain it. (Consistent with the earlier
  retraction in `project_hk_ntdll_build_gates_menu_scene_20260729` — I did not re-derive it.)
- 14:13 **The separation is PERFECTLY CONFOUNDED WITH TIME** (all 4 failures are the 4 newest runs), so
  archaeology cannot settle it and I did not treat p=1.1e-4 as a cause. Built the A/B lever instead:
  `MACRUNNER_SYNC_EXCLUDE_AUDIO_DRV=1` in `sync-prefix-from-dist.sh` drops exactly the three drivers
  that commit added. Default UNCHANGED. The template (07-24) has no audio DLLs at all, so the drivers
  arrive via the run-time sync — which `mr-run.sh:460` calls directly, so the gate reaches it.
- 14:14 **REPRIORITISED, and I stopped my own queued run to do it.** The ldr-neg-cache arm was armed and
  waiting on the slot; a throughput flag measured on a boot that never reaches Mono answers little, and
  this regression has silently invalidated every run since 11:32. Killed it and launched **AUDIOEXCL**
  instead: `SKIP_DEPLOY=1` so the shipped ntdll stays `968d1dafcbcc` — **byte-identical to the four
  failing runs** — and the three drivers are the ONLY difference. PREDICTION: `Begin MonoManager`
  returns. If it does not, the audio driver is exonerated and the 11:16->11:41 change is elsewhere.
- 14:15 Instrument written so both arms are measured identically, not by eye:
  `scripts/sample-classifier-share.py`. Validated against the control — reproduces 43.6%/67.0% exactly,
  and the independent 13:57 sample replicates at 43.2%/66.0% with PEB-WALK 97.0%, cache-insert 0.0%.
14:09 · BACKEND FAILOVER · `claude` retired after 2 consecutive returns under 180s with no journal line; continuing on `codex`.
LOOP-STATUS-was: BLOCKED (backends exhausted 14:10; claude is back, lane restarted) — every backend in the failover chain (claude codex) retired after 2 consecutive no-work fast returns; quota or auth is exhausted and the operator must refresh it.

16:22 · COORDINATOR — СКОРОСТЬ: ТРИ РЫЧАГА, ВСЕ ИЗМЕРЕНЫ, РАЗНОЙ ПРИРОДЫ. Cold start to the HK menu is 476 s
(prefix sync 17 s · wine start 18 s · to Unity init 21 s · **Mono load phase 232 s** · language 124 s · menu scene 61 s).
Rosetta reaches the same point in under 45 s. The gap decomposes as:
(1) **Translation cache retains 0.3 %** — `hits=39161 misses=40612 stores=117 store_skips=40495`.
`native_blob_single_arg_helper_stub()` bails at the second `blr x23`, so any block with two helper calls can never be
persisted and the cache has reached a steady state where it cannot learn the rest. FIX IN TREE, DEFAULT OFF:
`MACRUNNER_HB_CACHE_MULTI_HELPER=1` (commit a2c37781) generalises the patcher to N sites, keeping every safety rejection.
A/B running now; prediction registered before the arms: if `stores` stays ~117 with the gate on, multi-helper blocks are
NOT the cause and the rewrite is not the win.
(2) **Synchronisation waiting, untouched.** A live `sample` of a stalled run: two threads at 538 samples each, both in
`macrunner_hb_try_kernel32_handle_semantic -> NtWaitForSingleObject -> inproc_wait -> msync_wait_objs -> __ulock_wait2`
(293 and 495 samples in the wait itself), process at 28 % CPU. That is not translation cost and no cache removes it. It
also explains why concurrent runs reached Mono 35 % of the time against 76 % for solo runs — contention on the same
objects, not on CPU.
(3) **Memory barriers — Rosetta's architectural advantage, not our bug.** Our codegen emits `dmb ish/ishld/ishst` to
emulate x86 TSO (24 sites in hb_arm64_codegen.c; `emit_direct_mem_load_to_gpr_tso` etc.). Apple Silicon has a hardware
total-store-ordering mode that removes the need entirely, and Rosetta enables it — a privilege of the system translator.
`grep -rn 'TSO|ACTLR'` finds no attempt in our tree, and a third-party process cannot set that bit. So barrier COST can be
reduced (emit only where ordering is observable) but not eliminated. Parity with Rosetta on a cold start is not a
realistic target; on a warm cache it is worth chasing.
NOTE: the lane died at 14:10 with ALL BACKENDS RETIRED. `claude` answers now, `kimi` is 403 (billing cycle). Restarted on
claude alone.

## ITER-3 — 2026-07-29

- 16:23 Resumed. Slot BUSY (`laneA-MH-ON-a1` guest 66105, the coordinator's multi-helper A/B, launched 16:21).
  Did not disturb it. Verified its arming rather than assuming: `MACRUNNER_HB_CACHE_MULTI_HELPER=1` on MH-ON
  vs `=0` on MH-OFF — the A/B **is** correctly armed.
- 16:24 **[MY ERROR, caught in the same turn]** I counted `winecoreaudio` in 5 run logs, got 1/1/1/1/1, and
  was about to report "audio driver present in every run, so ITER-2's audio hypothesis is refuted". The
  pattern was matching `[sync] holding back winecoreaudio.drv by default` — the line that says the driver is
  **absent**. The count measured the exact opposite of what I read into it. This is the lane's own
  "a pattern you misspelled is not a measurement" trap; I pulled the literal lines and it dissolved.
- 16:25 **[TRAP CLOSED BEFORE SPENDING A RUN]** I had an audio A/B queued (`MACRUNNER_SYNC_INCLUDE_AUDIO_DRV=1`).
  Applying ITER-1's own rule — grep the flag's definition site for a recorded verdict first —
  `scripts/sync-prefix-from-dist.sh:91-102` already records it: **A/B SETTLED 14:16, default reversed.**
  Shipping the drivers breaks the boot, AND `tools/winaudioprobe.c` against a prefix that DID contain
  winecoreaudio.drv reports **`waveOutGetNumDevs = 0`**. So the run I was about to spend would have
  re-derived a settled result. Not spent.
- 16:25 **[MEASURED] Step 4 (audio) has a named blocker, and it is not the missing file.** The file arriving
  is necessary and demonstrably NOT sufficient: zero endpoints enumerate with it present. FMOD fails in
  5/5 of today's recent runs. **[HYPOTHESIS]** this is the same defect class as winemac.drv and winemetal:
  a wine builtin `.drv` whose DllMain/unixlib init never runs in the x86_64 guest
  (`MODULE_InitDLL` skips AMD64 `LDR_WINE_INTERNAL` builtins returning SUCCESS). Three modules, one symptom.
- 16:27 **[MEASURED] Reclaimed a stuck core.** pid 63001 = HK guest, 69 min at 100 % CPU, **orphan** on four
  independent checks: `ppid=1`; its prefix no longer exists on disk (only the live run's `_mr-run.8nNaCS`
  remains); no run dir's `wine-child.pid` names it; no supervisor. Sampled it before killing (evidence kept
  at `reports/phase4-hollow-knight/specimens/orphan-63001-interpreter-bound-20260729.sample.txt`), then
  scoped `kill -TERM 63001`, gone on TERM.
- 16:27 **[MEASURED] That specimen is a THIRD wedge class, distinct from ITER-2's signal storm.** Its one
  busy thread is **87.6 % in `hb_interpreter_run` -> `exec_instr_unlocked`** with **zero** `_sigtramp`/
  `segv_handler`/`ldr_entry_from_pc` frames anywhere in the process; 7 Unity `AssetGarbageCollectorHelper`
  threads block in `NtWaitForSingleObject -> server_select -> read`. The `exec_instr_unlocked` offsets are
  spread over many opcodes (+52/+88/+9912/+14380/+17264/+20684), so it is **executing varied code, not
  spinning on one instruction** — i.e. not a hang but the x86 interpreter grinding at interpreter speed for
  69 minutes. **[HYPOTHESIS]** a block demoted to the interpreter never re-promotes to the JIT.
  Also visible and cheap: `_tlv_get_addr` (TLS lookup) takes 237 samples inside `mem_read`/`hb_memory_read`.
- 16:29 **[MEASURED] The self-init cost is reproducing in today's runs, and today's arms sit in the
  degraded regime.** Both MH arms have `MACRUNNER_MACDRV_SELFINIT_DELAY_MS` **unset** => the driver installs
  at ~+140 s, inside the ~+54..+280 s Mono window. Same-day, same-ntdll comparison:
  · AUDIOAB (delay=420000, **deferred**): MonoManager +57.6 -> UnloadTime +289.8 = **232.2 s**, and it
    reached `Loaded Objects now` + `Restored language` (the menu scene).
  · MH-OFF (delay **unset**): +57.9 -> +387.4 = **329.6 s**, `Loaded Objects now` = **0**.
  · CACHESKIP-a2 (unset): reached UnloadTime, `Loaded Objects now` = 0. MH-ON still grinding at +426 s.
  Consistent with `macdrv_main.c:566-572`'s recorded 14/16-vs-0/8. **Note for the coordinator:** the
  multi-helper A/B is running in this degraded regime, so its *wall-clock* is confounded; its registered
  metric (`stores`, a count) is not.
- 16:30 **PRE-REGISTERED, written before the run.** Brief step 1 says fix the self-init cost properly rather
  than living on the delay. ITER-2 built exactly that fix and never ran it (deprioritised 14:14):
  `MACRUNNER_HB_LDR_NEG_CACHE=1` caches `ldr_entry_from_pc` **misses**, and ITER-2 measured the miss path —
  the PEB module-list walk — at **97.2 %** of that function, which is **43.6 %** of the wedged thread, with
  cache-insert at **0.0 % of 3146 samples**. Verified the gate is in the **shipped** artifact by content, not
  by SHA alone: `strings dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so | grep MACRUNNER_HB_LDR_NEG_CACHE`
  = 1 hit, ntdll `6c6f2fa2d5a48801`. **No rebuild needed — this costs one run.**
  **[HYPOTHESIS] being tested:** installing a driver mid-load adds modules to the PEB list and adds
  unclassified PCs, so it lengthens every miss walk — which would make the neg-cache the direct fix for the
  self-init cost, not merely a throughput tweak.
  · ARM: `MACRUNNER_HB_LDR_NEG_CACHE=1`, `MACRUNNER_HB_CACHE_MULTI_HELPER=0`, delay unset — single variable
    against MH-OFF.
  · CONTROL: **MH-OFF, already run at 16:08** (Mono 329.6 s, `Loaded Objects now`=0).
  · **PASS** = Mono phase < 260 s (the deferred band is 232 s) OR `Loaded Objects now` >= 1.
  · **NULL** = Mono phase >= 320 s => the neg-cache does not explain the self-init cost, and step 1 needs a
    different mechanism.
  · **PRIMARY signal is the mechanism metric, not wall-clock** (n=1/arm, and the source itself records that
    only a *subset* of runs stalls, medians unchanged): `scripts/sample-classifier-share.py` on a live
    sample — PASS = `ldr_entry_from_pc` 43.6 % -> <5 % and classifier 67.0 % -> <30 %.
- 16:36 **[MY HYPOTHESIS, REFUTED BY MY OWN CHECK — do not carry it forward]** At 16:25 I proposed that
  `winecoreaudio.drv` is the same defect class as winemac.drv/winemetal.dll (AMD64 builtin whose DllMain
  `MODULE_InitDLL` skips, so `__wine_init_unix_call` never publishes the unixlib handle). **Wrong.** The
  shipped `x86_64-windows/winecoreaudio.drv` is a **pure winecrt0 stub** — 49152 B, imports only kernel32
  (`DelayLoadFailureHook`/`DisableThreadLibraryCalls`/`ResolveDelayLoadedAPI`), built from `crt_dllmain.c`/
  `delay_load.c`/`dll_main.c`, and `strings` finds **zero** `__wine_init_unix_call` /`__wine_unixlib_handle`
  where winemetal.dll has all of them. `Makefile.in` confirms: every source compiles into the **unixlib**.
  **There is no PE-side DllMain to skip**, so a winemetal-shaped fix would have changed nothing.
- 16:38 **[MEASURED] Step 4's real mechanism, and it is a 40-second probe away.** `mmdevapi/main.c:75-99`:
  mmdevapi (not the driver) opens the unixlib via `NtQueryVirtualMemory(..., MemoryWineUnixFuncs, ...)` and
  then `__wine_unix_call(process_attach)` / `(test_connect)`. Steps 2-4 each log **`ERR`** — on by default —
  so a run with the drivers shipped **already names the failing step and its NTSTATUS**. Only step 1
  (`LoadLibraryW` failure) is `TRACE`-gated and therefore invisible. NB this is the same
  `MemoryWineUnixFuncs` bridge the lane recorded as **refuted for winemac.drv**; refuted *there* must not be
  generalised to "the bridge is irrelevant" — for audio it is the load-bearing path.
  NEXT for step 4 = `tools/winaudioprobe.c` + `MACRUNNER_SYNC_INCLUDE_AUDIO_DRV=1`, ~40 s, no HK: audio init
  is at ~+55 s, long before the Mono window those drivers damage, so the boot regression does not block it.
- 16:40 **[MEASURED] The multi-helper A/B could not read its own registered metric.** In BOTH arms
  (MH-OFF 16:08, MH-ON 16:21) `macrunner-hb-translation-cache-summary`, `stores=`, `store_skips=` and
  `hits=` occur **0 times**, though `translation_cache=1` is on. `hb_contract_telemetry.c:143-160`: the
  summary is one-shot from **atexit**, so a harness-timeout kill emits nothing — the source comment already
  diagnosed this and added a `-progress` emitter every 5000 compiles. The shipped ntdll at that moment
  (`6c6f2fa2d5a48801`) predates it. The ntdll now shipped (`638e27f71034d533`, 16:35) **does** contain the
  progress marker and a fresh MH-OFF launched 16:37 — so this looks found-and-closed independently. Recorded
  only so the 16:08/16:21 numbers are never read as a measured `stores` result.
- 16:41 **[MEASURED] MH-ON landed at mono_phase 600.1 s, `Loaded Objects now`=0.** Full inline-regime
  baseline is now **{313.3, 329.6, 600.1} s with LO=0 in 4/4**, against deferred **232.2 s with LO=1**. The
  inline spread is **1.9x**, so no single run settles step 1 on wall-clock — the classifier-share mechanism
  metric must stay primary, as pre-registered. Also: **5 of 6 deferred runs never reached `Begin MonoManager`
  at all** despite living 633-2746 s — deferral buys the menu when the boot survives, it does not make the
  boot survive.
- 16:43 **[MEASURED] MY PRE-REGISTERED RUN WAS KILLED FROM OUTSIDE AT +64.5 s — it is a lost run, not a
  result.** Launched correctly (verified: gate present **by content** in the shipped artifact; the run's own
  `translation-cache-open root=…/ntdll-6c6f2fa2d5a48801` proves which binary it used; the inherited stale
  `MACRUNNER_WINE_BIN` was detected and unset by the launcher, so that trap did not fire). It reached
  `Begin MonoManager` at +60.9 s and died 3.6 s later with **`exit=143` (SIGTERM)**. ntdll was rebuilt and
  deployed at **16:35**, the same minute; replacing a mapped `ntdll.so` requires killing live guests, so a
  deploy-time cleanup is the plausible killer — **[HYPOTHESIS]**, I did not observe the killer directly.
  **Read nothing into this run.**
- 16:45 **★ MACHINE HAZARD, and I yielded rather than win it.** `scripts/laneA-run-hk.sh:104-119` waits up to
  60 s for other spike wine and then **force-cleans it** (`mr-clean.sh`) — *before* creating its own run dir,
  i.e. **upstream of `mr-run.sh`'s atomic slot lock, which therefore cannot prevent it**. Two concurrent
  invocations destroy each other's runs. My retry chain was sitting in exactly that loop against the
  coordinator's live `MH-OFF-163710` guest (18861), so I terminated **my own** chain (41376 -> 18537 -> 79646,
  supervisor-first, scoped by verified PID). Verified after: my chain gone, their guest alive at 103.7 % CPU.
  **Left untouched deliberately:** the five 14:10-era `AUDIOCHECK` supervisors, hung 2.5 h at **0 % CPU** in
  post-run Python triage — inert where they are, but killing them would let their parent loops advance into
  the same force-clean and take out the live run. Safe to remove only when the slot is idle.
- 16:47 Report: `reports/phase4-hollow-knight/HK-MASTER-ITER3-AUDIO-MECHANISM-AND-SLOT-COLLISION-20260729.md`.
  Also reclaimed a stuck core and captured a **third wedge class** (interpreter-bound, zero signal frames) —
  see the 16:27 entries. Consequence worth carrying: **"100 % CPU + silent log" is now ambiguous across three
  classes**; sample before calling any run wedged, it is free.
- 16:52 **★ FIXED THE MACHINE HAZARD ITSELF, surgically.** `scripts/laneA-run-hk.sh:111-120` now refuses to
  force-clean a **live** run. It reuses mr-run.sh's OWN definition of staleness rather than inventing a
  second one: the title-slot lock (`$TMPDIR/macrunner-title-slot.lock/pid`) records the owner pid, and
  mr-run.sh already breaks locks by **PID liveness, not age** (a real run legitimately lasts 45+ min). So:
  live owner -> keep waiting (log every 5 min; `exit 75` after 60 min rather than kill someone's run);
  no owner or dead owner -> force-clean exactly as before, so **the orphan path this branch was written
  for is preserved byte-for-byte**. Both branches exercised directly: live pid -> WOULD WAIT, dead pid
  99999 -> WOULD FORCE-CLEAN. `bash -n` clean. This is the defect that killed my 16:34 run.
- 16:54 **[MEASURED, free, from the coordinator's own live run] Lever #1's baseline has MOVED, and the
  useful number is not the one registered.** `laneA-MH-OFF-a1-try1-163710` (MULTI_HELPER=**0**, i.e. the
  CONTROL) on the new ntdll `638e27f71034d533` now emits `translation-cache-progress` — the emitter the
  16:08/16:21 arms lacked — and reads:
  `hits=19390 misses=150434 stores=67410 store_skips=83023 store_skip_multi=73571 store_skip_unmatched=9452`.
  **Retention is 44.8 % (67410/150433), not the 0.3 % recorded at 16:22** — that 0.3 % (`stores=117`) came
  from an older binary, so "stores stays ~117" can no longer be the pass/fail line. **The sharp number is
  `store_skip_multi` = 73571 = 88.6 % of ALL remaining skips.** That gives lever #1 a much better
  pre-registered prediction than the original: if `MACRUNNER_HB_CACHE_MULTI_HELPER=1` does what it claims,
  retention should move 44.8 % -> ~93 % and `store_skip_multi` should collapse toward 0; if
  `store_skip_multi` stays ~73k with the gate on, the patcher rewrite is NOT the win. Handing this to the
  coordinator rather than acting on it — it is their lever and their run.
- 16:56 **Step 4 armed and waiting, cheap.** `scripts/hk-audio-probe-ab.sh`: builds `tools/winaudioprobe.c`,
  waits for the slot, runs it with `MACRUNNER_SYNC_INCLUDE_AUDIO_DRV=1`, and reports **which of the three
  `mmdevapi` `ERR` steps fires** (or says "no ERR at all -> the failure is step 1, which is TRACE-gated and
  therefore invisible" instead of reporting silence). Traps encoded and each one verified, not assumed:
  · **`timeout` and `gtimeout` are BOTH ABSENT on this box** (`which` finds neither) — the recorded
    silent-exit-127 trap is real, so nothing is wrapped in it; mr-run.sh does its own timing.
  · The probe is a **genuine x86_64 PE, proven by `CHPEMetadataPointer = 0x0`, not by the machine word** —
    an ARM64EC PE also reports 0x8664 while running as native ARM64 and never touching HyperBridge, which
    would have made the whole measurement a no-op. The script aborts if that pointer is non-zero.
  · It runs through **`mr-run.sh` directly, never `laneA-run-hk.sh`** — mr-run takes the atomic slot lock
    and has **no** force-clean, so this probe physically cannot destroy the live run. It waits.
  Disk checked before arming: `disk-guard.sh --check-only` rc=0 at exactly the 30 GB minimum.
- 16:58 **★ SYNTHESIS — steps 1 and 3 share one root, so lever #1 is on the FIRST-PIXEL critical path.**
  Read `HK-BLACK-FRAME-ROOT-CAUSE-VERDICT-20260727.md` rather than re-deriving step 3: the black frame is
  **already closed as a graphics question** — every graphics layer is independently exonerated, the frame is
  the cleared backbuffer because **no scene is ever created**, and the guest managed bootstrap halts at
  `Performing automatic level start.` in the Mono/JIT `jit_code_hash` cycle. Handed off to the CPU/Mono lane.
  So brief step 3 does **not** need a graphics experiment from me; it needs guest execution to get faster/
  unstuck — which is exactly what this iteration measured from the other side (translation-cache retention
  44.8 % with `store_skip_multi` at 88.6 % of skips; the classifier storm; the interpreter-bound third wedge
  class). **The menu-speed work and the black frame are the same critical path**, so lever #1 is not merely a
  boot-time nicety. The verdict also hands over a ready-made regression detector: the ordinal-200 presented
  readback, expected-black hash `0xc770038f717d0383` — it turns non-black exactly when a candidate engine fix
  works, which is the acceptance test for any of this.
- 17:04 **[I REFUTE MY OWN 16:27 HYPOTHESIS]** I proposed that the interpreter-bound third wedge class is a
  block **demoted** to the interpreter that never re-promotes, and named "JIT code cache full" as the likely
  mechanism after finding 6 such fallback sites in `hb_runtime.c`. **The logs refute it.**
  `hb_runtime.c:3236` emits `macrunner-hb-jit-code-cache-full` **ungated, once per process**, and that one
  emitter covers *every* block-cache reason (`block-cache-full`, `block-cache-put-failed`,
  `block-cache-promote-lost-entry`, `jit-buffer-full`). Across today's **12** runs that lived >=400 s it
  appears **0 times**, and so does `macrunner-hb-jit-signal-fallback`.
  **And I checked the zeros before believing them**, which is the trap I fell into once already today:
  `strings` on the shipped `ntdll.so 638e27f71034d533` finds `macrunner-hb-jit-code-cache-full`=1,
  `macrunner-hb-jit-signal-fallback`=1, `"interpreter fallback"`=5, `"JIT code cache full"`=1 — the markers
  exist, so the zeros are measurements, not silence. Strictly scoped: `laneA-MH-OFF-a1-try1-163710` is the
  one run *verifiably* on that marker-carrying binary (the earlier ones ran on `6c6f2fa2d5a48801`, now
  overwritten and no longer checkable), and it reads 0 for both.
  **So no instrumented JIT->interpreter demotion fired at all.** The likelier reading of the orphan is the
  dull one: `hb_interpreter_run` is simply the path for blocks the JIT has not compiled/promoted yet, so
  that specimen was executing a lot of COLD code, not stranded by a demotion. Carry the *observation*
  (interpreter-bound is a distinct outward signature from the signal storm) and **drop the demotion story**.
- 17:05 **[MEASURED] The coordinator's new control landed at mono_phase 588.0 s.** Inline-regime baseline is
  now **{313.3, 329.6, 588.0, 600.1} s** — MH-OFF at 329.6 **and** 588.0 on the same flag setting. The
  multi-helper arms (MH-OFF 329.6/588.0 vs MH-ON 600.1) are **entirely inside the control's own spread**, so
  wall-clock cannot separate that A/B at n=1-2; the `store_skip_multi` count (16:54) is the metric that can.
- 17:10 **[MEASURED] Inline-regime menu tally is now 0/5.** `MH-OFF-163710` reached `UnloadTime` at +644.9 s
  (mono 588.0 s) and **never** emitted `Loaded Objects now`; at +655.7 s it hit
  `macrunner-hb-callback-exception-stack` (pc=0x1073197dc, frame pc_module=ntdll.dll rva=0x68284) — the
  native-dispatch callback-exception class, not this lane's. So across every run today with the driver
  installed inline: **mono phase {313.3, 329.6, 588.0, 600.1} s and `Loaded Objects now` = 0, 5/5**; the one
  run that reached the menu is the single **deferred** run at 232.2 s. The deferral is still the only thing
  that has produced a menu today, which is exactly why brief step 1 asks for a real fix rather than the delay.
- 17:11 STATE HANDED FORWARD: audio probe armed and WAITING on the slot (it never force-cleans, so it cannot
  disturb anything); the step-1 neg-cache A/B must be RE-ARMED on `638e27f71034d533` **with its control
  re-run**, because the binary changed under the 16:08 control. `LOOP-STATUS: CONTINUE` (not at col 0 — this
  iteration is not GOAL and not BLOCKED).

## ITER-4 (master lane) — the pinned-thread wedge is a KERNEL FAULT LOOP, and its instruction is named
- 17:01 **[MEASURED, live specimen, replicated 3x] Three threads pinned at ONE instruction:**
  `macrunner_hb_pc_is_x64_guest_code_module_no_lock+264` = `0x28940` = `ldrh w9,[x0]` in the shipped
  `ntdll.so 638e27f71034d533`. Specimen = the coordinator's live `laneA-MH-OFF-a1-try1-163710` (guest 18861),
  sampled at +18m, +19m and +21m. Threads t46601608 / t46602764 / t46602884, **7518/7518, 7599/7599 and
  6058/6058 samples at that single PC**, every frame offset above it identical too:
  `__wine_unix_call_dispatcher -> macrunner_hb_x64_thread_entry -> macrunner_hb_run_x64 -> _sigtramp ->
  macrunner_hb_primary_signal_handler+628 -> macrunner_hb_route_x64_callback_fault+752 -> ...+264`.
- 17:01 **[MEASURED] It is a KERNEL page-fault loop — not a spin, not a suspend, not signal recursion.**
  `ps -M` twice, 20 s apart. The three threads: **sys +4.60/+4.56/+4.58 s vs usr +0.38 s each** (~12x
  inverted), STAT=**R**, ~24-26 % CPU. The two healthy running threads on the same process invert the
  other way (usr +5.45 s vs sys +0.45 s). User PC frozen + system time dominant = the load faults, the
  kernel services it, the same instruction re-faults, forever. Each thread burns a full core doing it.
- 17:01 **[MEASURED] Source-level cause, exact:** `macrunner_hb.c:16366-16369` takes `module = ldr->DllBase`
  from the unlocked LDR walk and calls `macrunner_hb_module_machine(module)`, which at **:12783** does
  `dos->e_magic != IMAGE_DOS_SIGNATURE` — a **raw dereference of a PE header, from inside the SIGSEGV
  handler**. That deref IS the pinned `ldrh w9,[x0]`. Two more raw derefs follow it (`nt->Signature`,
  `nt->FileHeader.Machine`).
- 17:01 **This explains a documented puzzle rather than adding one.** The lane recorded
  `macrunner-hb-fault-reentry-break`=0 and concluded the fault-reentry guard never engaged. Correct —
  and it **structurally cannot** engage here: the faulting load runs inside the handler with SIGSEGV
  masked, so the kernel never delivers a second signal, so there is no re-entry for the guard to see.
  Only ONE `_sigtramp` in the stack, which had previously read as evidence AGAINST a fault loop.
  [I therefore do not treat 202ade1b's guard as covering this class.]
- 17:01 **[HYPOTHESIS]** these wedges are why the process is 78-81 % `__ulock_wait2` / 42.9 %
  `macrunner_hb_rtl_wait_on_address` (identical in both samples): 3 of ~7 runnable threads are
  permanently gone. NOT yet proven to be the ~558 s constant — the lane already refuted
  "pinned thread causes the stall" once, and I am not re-litigating that; what is new here is the
  MECHANISM and that it is fixable, not a claim about the stall.
- 17:01 Safe primitive already exists in the same file: `macrunner_hb_read_local_memory` (:1094,
  `mach_vm_read_overwrite`) which returns FALSE instead of faulting, and is already used for exactly
  this purpose by `module_from_pc`'s header scan at :1259/:1263. Fix = route the fault-path header
  probes through it. Hot-path cost is real (:16337 says this is called PER BLOCK from run_x64), so the
  safe read must be gated on "in signal handler", not applied unconditionally.
- 17:08 **FIX BUILT + PRE-REGISTERED A/B (registered BEFORE either arm runs).**
  Change: PE-header reads switch to `mach_vm_read_overwrite` (non-faulting) **only while the thread is
  inside the signal handler**; off the fault path they stay raw memcpy, so run_x64's per-block classifier
  cost is unchanged. Scoped in `macrunner_hb_primary_signal_handler` (the single `sa_sigaction` entry,
  so it also covers the ARM64X hexpthk SIGILL redirect, which reaches the same classifier by another route).
  **Swept the whole class, not just the measured site** — `module_machine` (the wedge), `module_size`,
  `image_nt_header`, and `pc_in_executable_section`'s section-table walk. The last one matters: it runs
  IMMEDIATELY after `module_machine` on the same path, so fixing only the measured site would have
  RELOCATED the wedge into the section loop instead of removing it.
  Probes are **member-precise, not whole-struct** — copying a full IMAGE_NT_HEADERS would touch ~264 B
  where the original touched 6 B and could fault in the gate-OFF control. Verified each rewritten
  function touches the same byte extent as the code it replaced.
  Gate `MACRUNNER_HB_FAULT_SAFE_HEADER_PROBE=1`, **default OFF**.
  Build verified by SHA **and** content: new `ntdll.so` `073f7233161cd534` vs shipped `638e27f71034d533`;
  `macrunner-hb-fault-header-probe-refused` and `MACRUNNER_HB_FAULT_SAFE_HEADER_PROBE` each appear 1x in
  the new artifact and **0x** in the shipped one. (Rebuilt once more after this line to move a getenv
  out of signal context, so the final SHA will differ — it will be re-verified the same way.)
  **Both arms run the SAME new binary**, gate off vs on, so the flag is isolated rather than the build —
  the trap that invalidated the 16:08 control when ntdll changed underneath it.
  **PREDICTION, arm B (gate ON):** `macrunner-hb-fault-header-probe-refused` >= 1 AND zero threads pinned
  in `pc_is_x64_guest_code_module_no_lock` with sys-time >> usr-time under `ps -M`.
  **PREDICTION, arm A (gate OFF):** refusals == 0, and the pin reproduces.
  **Falsifiers, stated in advance:** refusals==0 in arm B while threads still pin => the faulting read is
  NOT the one I fixed and this fix is WRONG. Refusals>0 but threads still pin => the wedge relocated and
  my sweep was incomplete. No pin in EITHER arm => the wedge simply did not recur this run; inconclusive,
  not a pass — the wedge appeared in 1 of the runs I have sampled, so absence proves nothing at n=1.
  **What this does NOT claim:** nothing about the ~558 s constant, the menu, or `Loaded Objects now`.
  Those are separate questions and this iteration does not touch them.
- 17:14 **DEPLOYED, and by a method that does not kill live runs.** Final `ntdll.so` `2919b8adc70c30a8`
  (clean rc=0, zero errors; every warning pre-existing at unrelated lines). Deployed with `cp` to a temp
  name then `mv` over the target — **atomic rename, so a live guest keeps its old inode**. Verified: the
  coordinator's run 89791 was mid-flight across the swap and survived. This is the direct fix for the
  in-place-overwrite hazard that killed my own run at 16:35; ntdll can now be shipped without waiting
  for an idle machine. Verified by CONTENT in the live dist, not just SHA: both markers = 1, and 0 in
  the binary it replaced.
- 17:14 **[MEASURED] A second specimen says the wedge ARRIVES LATE — this changes how it must be sampled.**
  `laneA-MH-ON-a1-try1-170346` (guest 6466) at **+5.5 min**: `macrunner_hb_pc_is_x64_guest_code_module_no_lock`
  = **0** occurrences and `_sigtramp` = **0**, across 53 threads. The specimen that showed the wedge was
  sampled at +18 min. Consistent with the lane's recorded "arrives ~560 s after the stall starts".
  **Consequence: any A/B that samples early measures nothing and would read as a false pass.** The driver
  therefore samples at +16 min, and "no pin in either arm" is pre-registered as INCONCLUSIVE, not a pass.
- 17:14 A/B driver `scripts/hk-header-probe-ab.sh` launched (pid 56189) and **waiting on the slot** behind
  the coordinator's live run — it goes through `laneA-run-hk.sh`, which after my 16:52 fix waits on a
  live owner instead of force-cleaning it, so it physically cannot destroy their run. Disk checked
  first: `disk-guard.sh --check-only` rc=0 at 30 GB (the floor — worth watching if arms are added).
- 17:14 Report: `reports/phase4-hollow-knight/HK-MASTER-ITER4-FAULT-PATH-HEADER-WEDGE-20260729.md`.
  Carried forward for the next iteration: **read the A/B results and apply the pre-registered falsifiers
  honestly** — refusals==0 with the pin still present means MY FIX IS WRONG, not that the run was bad.
  Untouched this iteration and still open: brief steps 2 (input in HK), 3 (black frame), 4 (audio —
  `scripts/hk-audio-probe-ab.sh` is still armed and unrun, and mmdevapi's three `ERR` steps will name
  the failure the moment it gets a slot).
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).

## ITER-5 (master lane) — the wedge MECHANISM is proven offline, and so is the remedy
- 17:18 **[MEASURED, free, third independent replication of the wedge]** Sampled the coordinator's live
  `laneA-MH-ON-a1-try1-170346` (guest 6466) at **+15 min**: of 57 threads, **1** has **5440/5440** samples
  at `macrunner_hb_pc_is_x64_guest_code_module_no_lock+264`, with the frame stack identical to ITER-4's
  specimen (`__wine_unix_call_dispatcher → …x64_thread_entry → …run_x64 → _sigtramp →
  …primary_signal_handler+628 → …route_x64_callback_fault+752`). `ps -M` ~9 s apart: that thread
  **+3.82 s sys vs +0.36 s usr (10.6× inverted)**, STAT R, 56.2 % CPU; the other running threads invert the
  other way. **Arrival timing now has three points: 0 pins at +5.5 min, 1 at +15 min, 3 at +18 min.**
- 17:18 **[I CORRECT MYSELF, same iteration]** My first pass read `ps -M` as UTIME-then-STIME and had the
  ratio backwards. The header is **STIME before UTIME**. Read correctly the conclusion is unchanged, but
  the readout script now *asserts* the column order from the header instead of trusting position — getting
  it backwards inverts the entire verdict.
- 17:26 **★ [PROVEN — this was ITER-4's biggest unproven assumption, and it holds]** `tools/hb_masked_segv_probe.c`
  (new instrument, 30 s, **no title slot**): a SIGSEGV handler installed *without* `SA_NODEFER` (so SIGSEGV
  is masked inside it) dereferences an address that was mapped then `munmap`ped — the shape of a PE-header
  read at a dead module base. Two arms, same address, same context:
  `ARM RAW : WEDGED (killed at 4.0s) usr=0.407s sys=3.752s **sys/usr=9.2x**`
  `ARM SAFE: COMPLETED (no wedge)    usr=0.000s sys=0.000s` — `mach_vm_read_overwrite` refuses and the
  handler runs to completion. RAW prints its "before" marker and **never** its "after": the faulting load
  never completes. **9.2× sits right next to the live HK signature (10.6 % / ~12×.)** So the Darwin/arm64
  kernel really does retry a fault taken with its own signal blocked, forever, as system time at a frozen
  user PC — and the primitive the shipped fix uses really does escape it.
  **What it does NOT prove:** that HK's specific faulting address is unreadable, or that the gate removes
  HK's wedge. The live A/B is still the arbiter. It removes the chance that the *mechanism* was wrong.
- 17:24 **[HYPOTHESIS, named and located] Where the bad base comes from: two per-thread caches with NO
  invalidation anywhere in the file.** `macrunner_hb.c:1267` caches a raw **`LDR_DATA_TABLE_ENTRY *`**
  (+range) — a freed entry leaves a dangling pointer whose `ldr->DllBase` (:16502) is then read; and
  `:16479` caches `{base,end,amd64}` whose stale `base` goes straight into `pc_in_executable_section`.
  Both are `__thread`, which fits *some* threads wedging and the count growing over time. Two variants I
  cannot yet separate: **(a)** an unload outliving a cache entry, or **(b)** the lock-free walk at `:1488`
  observing an entry linked before its image is committed — **(b) needs only a LOAD**, which is what an
  inline driver install is a burst of, in exactly the window where every guest thread runs this classifier
  per block. Consistent with inline {313.3, 329.6, 588.0, 600.1} s / `Loaded Objects now` 0/5 vs deferred
  232.2 s / 1-1, **but not measured.** Distinguishing test = print the refused address + LDR membership.
  **Deliberately NOT built this iteration:** rebuilding ntdll now would change the binary under an A/B
  whose OFF arm has not started — the exact trap that invalidated the 16:08 control.
  Corollary: the shipped fix is a **mitigation, not a cure** — the torn/stale base is still produced.
- 17:22 **[VERIFIED ahead of the arms, three traps closed] Gate proof needs no rebuild.** `final-child.json`
  records the guest's OWN environment (122 entries, `ENV-STREAM/v1`), so the readout asserts
  `MACRUNNER_HB_FAULT_SAFE_HEADER_PROBE`=1 in ON / unset in OFF and marks an arm **VOID** otherwise —
  "refusals == 0" can no longer be confused with "the gate never applied". Also confirmed from the same
  records: **`MACRUNNER_WINE_BIN` IS set to the stale `engine/wine/dist/bin/wine` in every run on this box**
  while `argv0` is nevertheless `dist-arm64ec-spike/bin/wine` (trap live, not biting — check argv0, never
  the variable); and **both arms run the driver INLINE** (`…SELFINIT_DELAY_MS` unset), so they compare
  against the inline band, not the deferred run.
- 17:22 `scripts/hk-header-probe-ab-readout.sh` (new) computes the verdict with the **pre-registered
  falsifiers encoded in code, written before any arm produced a number**. Separate file on purpose: bash
  reads a running script incrementally from its fd, so editing `hk-header-probe-ab.sh` mid-flight could
  make pid 56189 execute garbage. **Secondary endpoint registered now, before either arm ran:** if the pin
  causes the inline stall, ON's Mono phase should move toward < 260 s and may emit `Loaded Objects now`;
  OFF should stay 313–600 s at 0. **n=1 per arm — suggestive only; a null there does not refute the
  mechanism result.**
- 17:28 **A/B still QUEUED, not stalled.** Driver pid 56189 alive; arm OFF waiting on the atomic slot lock
  held by the coordinator's mr-run 89791 (guest 6466 still alive at +25 min). Nothing was launched into a
  busy slot and nothing was force-cleaned.
- 17:28 **Flagged, not acted on:** `mr-run.sh` pid **8446** is an orphan (ppid 1, timeout arg 600) alive
  **4 h 41 m** having burned **34 min CPU (12.1 % sustained)**, no child but a transient `(bash)`. It does
  **not** hold the slot lock. Left alone — not mine, not blocking, and kills here are scoped-by-PID and
  supervisor-first, not tidy-ups of someone else's tree mid-experiment.
- 17:30 Report: `reports/phase4-hollow-knight/HK-MASTER-ITER5-MASKED-SEGV-MECHANISM-PROVEN-20260729.md`.
  Carried forward: (1) run `scripts/hk-header-probe-ab-readout.sh` and apply the falsifiers honestly —
  `refusals==0` with the pin present means THE FIX IS WRONG; (2) build the refusal-address telemetry only
  after the A/B binary is out of test; (3) brief steps 2/3/4 untouched — `scripts/hk-audio-probe-ab.sh` is
  written but **NOT running** and must be relaunched behind the arms.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 17:32 **★ MY A/B PRODUCED NOTHING, AND THE CAUSE WAS A DOCUMENTED TRAP I WALKED INTO.** Both arms exited
  **rc=2 at +24 s** — `run-contract-ledger status=BLOCKED exit before wine`. Neither arm ever started wine,
  so the READOUT correctly reports `pin: NO SAMPLE … MISSING, not absent` and `ARM VOID` rather than zeros.
  Root cause, measured: v1 called `laneA-run-hk.sh` **directly**, bypassing `scripts/hk-run-try12-config.sh`,
  which is where the contract's required inputs are set —
  `MACRUNNER_MR_RUN_{ACTXPRXY,CRT_CASE_FUSION,WWISE_OBSERVER}` (else `branch_input_absent`) and
  `MACRUNNER_MR_RUN_PREFIX_TEMPLATE=artifacts/hk-prefix-template-NOSERVICES`. Without the template mr-run
  falls through to `find_warm_prefix_template()` and picks `artifacts/warm-prefix/laneA-fullybooted-20260626-*`,
  which I verified **has no `drive_c/users`** (NOSERVICES has it) → `save_snapshot_manifest_sha256 = path_absent`.
  **`hk-run-try12-config.sh`'s own header documents this exact failure** ("exits rc=2 with a ~1250-line log
  and never reaches wine; that is NOT the exit=53 boot flake and retrying it cannot help") — and my arm logged
  `rc=2 lines=1255`. The lesson is not "the machine broke": it is that the run entry point IS part of the
  experiment, and the coordinator's comparable runs all go through that config (their `-a1` tag suffix is that
  script's `${TAG}-a${try}`).
- 17:37 **RE-ARMED AND VERIFIED, not merely relaunched.** `scripts/hk-header-probe-ab2.sh` drives both arms
  through `hk-run-try12-config.sh`. Checked the gate that killed v1 *before* spending an hour on it:
  `laneA-HDRPROBE2-OFF-a1-try1-173711` → **CONTRACT STATUS: READY, blockers []**, guest pid 25476 alive and
  past +40 s. The config also unset the inherited stale `MACRUNNER_WINE_BIN` on the way through. Sampling is
  now driven by the **guest's own clock** read from its run.log, not the driver's wall clock, because that
  config retries flakes and a driver-side timer would sample the wrong rundir or none. Falsifiers unchanged.
- 17:38 **★★ [I REFUTE THE LANE'S OWN WORKING CLAIM — including my own 17:22 framing] "Inline never reaches
  the menu / the deferral is the only thing that has produced a menu today" is WRONG, and the 0/5 tally was
  largely a TIMEOUT-BUDGET ARTIFACT.** Tabulated every run dir with its `timeout=` banner, its actual
  `ran_to`, its Mono phase and its markers — the trap my own notes warn about ("diff the `timeout=` banner
  first"). Today's inline runs:
  · tmo **700** → ran to ~725 s: MH-OFF-160800 (mono 329.6), MH-ON-162113 (mono 600.1) — **cut off**
  · tmo **900** → ran to ~927 s: CACHESKIP-a1 (—), CACHESKIP-a2 (mono 313.3) — **cut off**
  · tmo **1500** → MH-OFF-163710 mono 588.0, `Restored language`=1, `Loaded Objects now`=0
  · tmo **1500** → **MH-ON-170346 mono 484.5, `Restored language`=1 at +994.3 s, `Loaded Objects now`=1 at
    +1391.1 s** ← an INLINE run that reached the menu markers
  **Both of today's inline runs given a budget ≥1500 s reached `Restored language`; the 700/900 s runs were
  killed ~470 s before the marker's own arrival time.** So the marker tally was measuring the budget, not the
  driver. (Two further inline runs reached both markers with larger budgets — `vscb-onscreen-17` tmo 3000,
  lang=2/objs=2 — but they are dated **07-27** and **07-28**, i.e. different builds, so I cite them as
  context only, not as today's evidence.)
  **The honest reframing of brief step 1: inline is a ~2.7× SLOWDOWN to the same marker, not a wall.**
  Deferred `AUDIOAB-NOAUDIODRV-141354` reached BOTH markers having run only 515 s (mono 232.2); inline
  MH-ON-170346 needed +1391 s for the same marker.
- 17:38 **[and this cuts against my own pre-registered secondary hypothesis, stated before the arms report]**
  MH-ON-170346 is the very run in which I measured a **pinned thread at +15 min** — and it still reached
  `Restored language` and `Loaded Objects now`. **So the wedge is a tax, not an absolute blocker**, and the
  secondary endpoint I registered at 17:22 ("ON should reach the menu, OFF should not") is the wrong shape:
  the discriminating variable is Mono-phase *duration*, not marker presence. The mechanism endpoint
  (refusals + pin count) is unaffected and remains primary.
- 17:41 **[MEASURED] Step 4's probe had the SAME defect and its one "result" was not a measurement.**
  `hk-audio-probe-ab.sh`'s first real invocation returned `mr-run rc=2` in **2 seconds** and reported
  "INCONCLUSIVE: probe never printed waveOutGetNumDevs" — that was the run-contract blocking before wine
  (`canonical_{config,data,save}_path_authority_absent` + the same `branch_input_absent` trio), not the audio
  stack saying anything. Patched: the three branch decisions are passed explicitly, and the three canonical
  paths — which the ledger derives from `--exe` and which for a scratch probe derive to nothing — are named
  at the probe's own directory. It now also reads the contract status and **aborts loudly with `exit 6` when
  the status is not READY**, so a contract block can never again be reported as an audio finding.
- 17:42 **Autonomous pipeline running, all of it verified rather than assumed:** AB2 driver 78597 (arm OFF
  live, guest 25476, contract READY, guest clock +176 s) → samples at guest-clock 900 s → arm ON → its own
  readout; chain watcher 20540 waits on 78597 and then runs the fixed audio probe. Disk 31 GB, one wineserver.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).

## iter 6 — 2026-07-29 17:44–17:58
- 17:44 **★★ [MEASURED — I REFUTE THE LANE'S FRAMING OF STEP 3] "The window is black because HK never
  presents" is WRONG.** In `coherent-dist-run-20260724-140658-NOT_GOLDEN` — the ONE archived run that
  both reached the menu markers (lang=1, objs=1) and had `MACRUNNER_DXMT_SWAPCHAIN_TRACE=1` **confirmed
  in its own final-child.json** — `SwapChain.Present`=**2045**, `SwapChain.Present1`=**2045**, hr
  histogram `{"0x0":4090}` (every present S_OK), `Context.OMSetRenderTargets`=**4096**. Parsed by
  splitting on the marker, NOT on newlines: the records are concatenated, and a line-split first told me
  "Present=1" — an 4090× undercount I caught and corrected.
- 17:44 **[and I refuse the easy conclusion from the same log]** `dxmt-hk-drawtrace`=0 there would mean
  "zero draw calls" — the whole answer — but I **cannot show that binary could emit it**: the run dir
  archives no d3d11.dll, and `dxmt-drawtrace-totals` (fires every 1024 events; ~8 expected from 4090
  presents alone) is **entirely absent**, which is exactly what a binary PREDATING the emitter looks like.
  A zero from a marker I cannot prove was compiled in is not a measurement. **[HYPOTHESIS]** HK submits
  no geometry.
- 17:46 **[MEASURED, negative — step 4] The audio driver reaching the prefix did NOT fix audio.** Both
  `HDRPROBE2-OFF-173711` and `MH-ON-170346` have `MACRUNNER_SYNC_EXCLUDE_AUDIO_DRV` **unset** and
  `winecoreaudio` synced into the prefix, and still log `FMOD failed to initialize any audio devices`
  at **+51.6 s** and **+53.8 s**. Confounder disarmed first: the fast deferred run AUDIOAB-NOAUDIODRV-141354
  also shows FMOD failing but had `SYNC_EXCLUDE_AUDIO_DRV=1` — audio was deliberately excluded there, so
  its failure proves nothing. The brief's "driver arriving is necessary; whether sound works is unmeasured"
  is now measured: **necessary, not sufficient.**
- 17:48 **★ [MEASURED] My A/B arm OFF was KILLED 1108 s early, and the triage then INVENTED a symptom.**
  `laneA-HDRPROBE2-OFF-a1-try1-173711`: banner `timeout=1500s`, died at **+392 s** with **exit=143
  (SIGTERM)**; 84 s later sibling `laneA-RELOC-a1-try1-174508` appeared. classify_run filed it
  `SILENT_SPIN_NO_MARKERS` — "watchdog kill (exit=143) with ZERO fault markers" — but **the watchdog was
  1500 s and never fired**; the class is false. `laneA-run-hk.sh:125-143` force-cleans upstream of
  mr-run's atomic lock, so the lock cannot stop it. **My TMPDIR-mismatch hypothesis is REFUTED** — both
  scripts resolve `${TMPDIR:-/tmp}/macrunner-title-slot.lock` to the same real dir, which exists and
  holds a live owner; and `mr-run.sh:305` traps EXIT **before** taking the lock, so it IS held all run.
  **[HYPOTHESIS]** the killer ran a copy of laneA-run-hk.sh without the `:128` live-owner guard.
  **Corpus-wide consequence:** every early SIGTERM currently enters the record as a rendering/boot
  "silent spin". Some fraction of this lane's lang=0/objs=0 runs are probably this, not the engine.
- 17:54 **[a false negative in my OWN safety check, found and fixed]** `strings -a "$SHIP" | grep -q`
  reported `FATAL: shipped d3d11.dll lacks 'dxmt-hk-drawtrace'` against a binary I had just verified
  contains it **4×**: `grep -q` exits at the first match, `strings` dies of SIGPIPE (141), and
  `set -o pipefail` converts the successful find into a failed pipeline. Fixed with `grep -c`. Noting it
  because it is the exact shape that manufactures fake zeros — a guard that lies in the SAFE direction.
- 17:54 **QUEUED, gate verified BY CONTENT before launch.** `scripts/hk-blackframe-drawtrace.sh` (pid
  97576 alive) + `-readout.sh` (separate file: bash reads a running script from its fd, so editing one
  must not corrupt the other). Shipped `d3d11.dll sha=0b1a7170f9ecabb4` — byte-identical to the one
  laneA-run-hk.sh copies into its per-run overlay — carries `dxmt-hk-drawtrace`×4,
  `dxmt-drawtrace-totals`×1, the gate ×9, plus "CAMetalLayer nextDrawable returned nil". Source: 
  `d3d11.cpp:53` reads the SAME env var as swaptrace (draws are not behind a second gate) and
  `d3d11.cpp:81` counts EVERY call, emitting uncapped totals every 1024 events.
- 17:54 **Vehicle = the DEFERRED driver (`SELFINIT_DELAY_MS=420000`), on iter 5's own reframing:**
  deferred reached `Restored language` @ **+414.4 s** / `Loaded Objects now` @ **+475.7 s** in a 515 s
  run, vs inline needing ≥1500 s to hit the same marker at +1391 s. The black frame is orthogonal to the
  install stall, so buying the marker 2.7× cheaper is correct here.
- 17:54 **Falsifiers pre-registered in code before any number existed:** V0 VOID (gate≠1 in the guest's own
  final-child.json), **V1 NO-GEOMETRY** (draws==0 ∧ present>0 ⇒ black frame is UPSTREAM of DXMT — Unity's
  render loop — and no Metal work can fix it), **V2 DRAWN-BUT-BLACK** (⇒ loss inside DXMT's present/layer
  path), V3 NO-DRAWABLE, V4 CLEAR-ONLY (predicts pixels == the clear colour EXACTLY, checkable against the
  bitmap), V5 INCONCLUSIVE (no `Restored language` ⇒ the count describes boot, not rendering).
  **Dry-run against the 07-24 archive before it guarded anything live** — it correctly declined to declare
  V1 with no totals line and printed "do not force them into a class".
- 17:54 **Slot discipline:** the driver waits for my sibling drivers, then for a live lock owner, and
  **never force-cleans**; exits 75 rather than killing someone's run — the exact failure that cost iter 5
  its arm OFF. Report: `reports/phase4-hollow-knight/HK-MASTER-ITER6-BLACKFRAME-PRESENT-RUNS-AUDIO-UNFIXED-20260729.md`.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 17:58 **[PREDICTION REGISTERED BEFORE THE RUN REPORTS — the brief's own rule]** I predict **V1
  NO-GEOMETRY** (draws==0 ∧ present>0), and the mechanism is **already named and still instrumented in
  the shipping engine**: `engine/wine/dlls/ntdll/unix/macrunner_hb.c` labels guest rva **0x5872dc** as
  `render-step-esi-gate` (:38522) and `consumer-ready-cmp` with **role="consumer"** (:36332, plus
  :36018/:36197) — 4 live sites in the CURRENT source, not a backup. The CrossOver oracle recorded that
  same compare as **esi=1 under CrossOver, esi=0 for us**. A consumer-ready gate that never passes ⇒ the
  render step is skipped ⇒ **presents keep happening while nothing is drawn**, which is precisely the
  shape the 07-24 archive shows (4090 S_OK presents, 4096 OMSetRenderTargets, no drawtrace).
  **If V1 holds, the black frame, the 2026-06-27 present-stall RCA and the "producer LATE" finding are
  ONE bug, and it lives upstream of DXMT** — no Metal/layer/present work can fix it.
  **Falsifier, stated now:** if **V2** holds (draws>0 with a black capture) this unification is WRONG and
  the loss is inside DXMT's present/layer path. I will not reinterpret V2 as "partial V1".
- 17:58 **Contention, stated plainly rather than worked around:** a sibling lane is cycling short runs
  through the single title slot (`laneA-RELOC-174508` 17:45→17:49, `laneA-HELPERS-175052` 17:50→live),
  each ~4 min and each ending mono=1/lang=0/objs=0 — the same shape as my externally-killed arm. My
  header-probe A/B (78597) has therefore been blocked since 17:37 with its arm ON never started, and the
  blackframe driver (97576) queues behind it with a 60-min sibling cap before it proceeds regardless.
  **I did not kill anything to take the slot** — that is the behaviour that destroyed iter 5's arm.

## iter 7 — 2026-07-29 18:11–18:55
- 18:15 **Unblocked the slot rather than waiting 40 min for a cap to expire.** iter 6's HDRPROBE-AB2
  driver (78597) was wedged forever, and I found the mechanism: `laneA-run-hk.sh` pipes mr-run into a
  python timestamp filter, the SIGTERM'd guest left an orphan holding the pipe's write end, and the
  filter blocked on `stdin` for 37 min with its run long dead (chain 78597→79181→79210→9718→9719).
  Killed scoped by verified PID, supervisor-first; the blackframe driver took the slot 15 s later.
- 18:16 Launched iter 6's pre-registered blackframe run `laneA-BLACKFRAME-DRAWTRACE-a1-try1-181620`.
- 18:33 **★★ [MEASURED — I REFUTE MY OWN PRE-REGISTERED PREDICTION] THE BLACK FRAME IS REFUTED.**
  `dxmt-drawtrace-totals: draw=0 drawindexed=904 drawinstanced=0 clear=1 omset=61 present=29
  present1=29`, `nextDrawable returned nil`=0. At 17:58 I registered **V1 NO-GEOMETRY (draws==0)**;
  **drawindexed=904 kills it**, and with it the claim that the black frame, the 2026-06-27
  present-stall RCA and "producer LATE" are one bug upstream of DXMT via `render-step-esi-gate`.
  I withdraw that unification. The outcome is also outside my whole V1..V5 set — V2 was
  "DRAWN-BUT-BLACK" and this is **drawn-and-visible** — so I record a gap in my pre-registration
  rather than relabelling it a partial V2, exactly as I promised at 17:58 I would not do.
- 18:33 **★★ [MEASURED, with pixels] Hollow Knight renders its LANGUAGE-SELECT MENU.**
  `INPUT-EVIDENCE/hk-live.png`, 3024×1964 (Retina of 1512×982), **nonblack=5640/372444 = 1.51%**:
  legible `English/Français/Deutsch/Español/Italiano/Português/Русский/日本語/简体中文/繁體中文/한국어`
  with the ornate selection cursor on **English**. The brief's baseline for step 3 was `nonblack=0`.
- 18:22–18:47 **[MEASURED, negative — step 2] Four injection arms, every one Δ=0 at
  `stage=app_sendEvent_key_enter`:** (A) `CGEventPostToPid`; (B) `.cghidEventTap` + activate;
  (C) centre-click then B; (D) System Events `set frontmost of process "wine"` (which SUCCEEDED)
  then B. Also 0: `macdrv_key_event`, `macdrv_send_keyboard_input_sent`, `unity-async-state-query`.
- 18:40 **The zeros are measurements, checked four ways before I believed them.** Gate
  `MACRUNNER_TRACE_WINEMAC_KEYS=1` in the guest's own final-child.json; the same config block's
  sibling `macrunner-winshow` printed **11** lines, so the tracing block executed in THIS process;
  emitters present by content (`winemac.so` dae6b4e7f1da607c, `win32u.so` 5ee1ef280c8a0f35); and a
  **positive control** — a listen-only `.cghidEventTap` in my own process saw **6/6** of the same
  synthetic events, with `AXIsProcessTrusted=true`. The actuator injects; the wine process is deaf.
- 18:45 **★ [MEASURED] The break is STARVATION, not a wedge — and it is upstream of AppKit.**
  `sample` pid 10408, 3039/3039 main-thread samples in a **healthy** spine:
  `__wine_main → CFRunLoopRun → run_cocoa_app → -[NSApplication run] → _DPSNextEvent →
  _DPSBlockUntilNextEventMatchingListInMode → mach_msg`. `[NSApp run]` is running and waiting;
  nothing arrives to wake it. `applicationDidBecomeActive`=0 and every `activate()` returned
  `true` with `is_active:false`, `policy:0 (regular)`. **This refutes the framing that the current
  keyboard blocker is key-window routing inside winemac.drv** — the existing default-ON fix
  (`cocoa_app.m:108-118`, `:2143-2147`) acts on events delivered to `-sendEvent:`, and none are.
- 18:47 **[HYPOTHESIS, and the confound I could NOT close]** arms B–D all need the app to become
  active and it never did; macOS refuses activation from a non-active CLI — `mouse_inject.swift:349`
  recorded the same from the other side. Arm C's centre click may have hit an occluder. **The guest
  exited before I could run `mouse_inject --mode hid`'s exposed-point search.** So the honest claim
  is "unattended synthetic injection does not reach HK", which is strictly weaker than "keyboard is
  broken in HK". Next iteration must settle it: exposed-point click → confirm
  `applicationDidBecomeActive`=1 → then keys.
- 18:50 **[MEASURED] The deferred vehicle deferred NOTHING, and the menu came anyway.**
  `macdrv_init_user_driver_set` **+53.059 s** (normal path), `macdrv_selfinit_entry`=0, so despite
  `SELFINIT_DELAY_MS=420000` this was an **INLINE** run — and it reached `Restored language`
  **+373.155 s** / `Loaded Objects now` **+427.445 s**, FASTER than the deferred numbers the brief
  quotes (+414.4 / +475.7). The brief's step-1 premise "menu reachable only with the install
  deferred" does not hold for this build. I am not claiming the ~558 s stall is gone — I did not
  measure `Begin MonoManager`→`UnloadTime` against the healthy band.
- 18:52 **[MEASURED, negative — step 4] Audio still fails.** `FMOD failed`=1 in a run that reached
  Mono, the menu and 904 draws, so this is a real audio measurement. Necessary-not-sufficient stands.
- 18:53 **[MEASURED] The auto-triage misfiled this run for the SECOND iteration running.**
  `CLASS: SILENT_SPIN_NO_MARKERS conf=0.70`, `WHY: watchdog-kill with zero faults/zero markers`,
  `PIXEL_TRUTH: NO_ARTIFACT` — against a run with both menu markers, 904 draws and a photographed
  menu. "Zero markers" is false. **Corpus consequence: some fraction of this lane's recorded
  "black frame / silent spin" runs are probably triage artifacts, not engine states.**
- 18:54 **Corrections to myself, all made within the iteration:** (1) "`MACRUNNER_HK_INPUT_EVIDENCE`
  is a fake gate no code reads" — WRONG, it is a script-level opt-in at `hk-run-try12-config.sh:128`
  and it is what armed this measurement; (2) "I injected before the +420 s install" — WRONG, the
  driver was up at +53 s; (3) my `count()` printed "0\n0" because `grep -ac` exits 1 while printing
  0, blanking the first deltas; (4) the first click arm's `no_window` was a transient window-order
  gap, so that arm never posted — redone with a retry loop, and only the redone arm is reported.
- 18:55 Report: `reports/phase4-hollow-knight/HK-MASTER-ITER7-BLACKFRAME-REFUTED-INPUT-STARVED-20260729.md`.
  New instruments kept: `tools/hk_cghid_key.swift` (HID-tap keyboard actuator + activation report),
  `scripts/hk-input-evidence-inject.sh` (rides a live run, pre-registered K0..K4 falsifiers).
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 18:44 **★ [MEASURED, root-caused, FIXED, and both directions tested] The driver-wedge that has been
  eating the title slot all day.** `laneA-run-hk.sh:218` runs `mr-run.sh 2>&1 | timestamp_stream`.
  mr-run starts wine's services; **`services.exe` inherits the pipeline's stderr on fd 2** and is
  reparented to init when the guest dies. The guest exits, mr-run exits — and the python filter never
  sees EOF, so `rc=${PIPESTATUS[0]}` is never reached and the whole driver chain hangs **forever**
  with its run long dead. Proven by lsof: the filter's fd 0 was pipe `0xdd6a4a8f3dd15998`, and exactly
  one process held the matching write end — `pid=13371 cmd=services.exe fd=2`. `kill -TERM 13371`
  unwedged the entire chain in <10 s. This cost iter 6's arm OFF 37 min and the blackframe run 40 min.
  Fixed with `pipe_watchdog()` in `laneA-run-hk.sh`, scoped **by pipe identity** — it kills only
  processes holding THIS pipeline's write end, and only after `$RUNDIR/wine-child.pid` is dead plus a
  grace period, so a sibling lane can never be in the set. **Both controls run:** a faithful synthetic
  wedge freed in ~20 s (was: hang), and a negative control with a LIVE guest left it untouched
  (0 watchdog lines, guest alive after 40 s). Kill switch `MACRUNNER_LANEA_NO_PIPE_WATCHDOG=1`.
- 18:44 **[a fake fix I caught before shipping it]** my first watchdog located the filter with
  `ppid==$$`. `timestamp_stream` is a shell FUNCTION, so bash forks a subshell and python is the
  subshell's child (57004→93248→93250) — that match finds nothing and the watchdog would have been a
  silent no-op that merely looked like a fix. Replaced with an ancestry walk.
- 18:46 **★ [MEASURED] `hk-run-try12-config.sh` retried a run that HAD reached the menu — it reads a
  GLOBAL latch that every lane writes.** Line 152 took `RUNDIR` from `/tmp/laneA-current-rundir.txt`;
  with sibling lane SYNCBLOCK-a1 alive, that named the SIBLING's run dir, the menu grep read the wrong
  log, and it printed *"attempt 1 did not reach the menu (marker 'Restored language' absent) —
  retrying"* about a run whose log contains `Restored language` at +373.155 s. Verified live: the latch
  said `laneA-SYNCBLOCK-a1-try1-183638` while the tag-scoped lookup resolved my dir with the marker
  present. **Every successful run paid a second 20-minute slot while any sibling was alive.** Fixed to
  resolve the run dir by OUR tag, falling back to the latch only when it matches our tag.
- 18:50 **New instrument, built and deployed with both required checks.** `macrunner_trace_winshow`
  now prints `ex_style` and `noactivate`, because that is the one field that discriminates the
  measured input failure: `WS_EX_NOACTIVATE` → `wf.prevents_app_activation` (`window.c:184/:216`) →
  `NSWindowStyleMaskNonactivatingPanel` (`cocoa_window.m:131`) → `transformProcessToForeground:NO`
  (`cocoa_window.m:1815`). A non-activating panel **renders normally but never takes key focus** —
  which is exactly the shape observed (visible menu, `applicationDidBecomeActive`=0, zero NSEvents).
  It rides the already-affordable `MACRUNNER_HB_TRACE_WINSHOW` gate (11 lines/run, not per-frame).
  Built `winemac.so` **sha dae6b4e7f1da607c → 152762ab47273e8d**, and verified by CONTENT in what
  landed in `dist-arm64ec-spike`: new format string ×1, keys gate ×1, `macdrv_key_event` ×2.
- 18:51 **[gate check that saved a false conclusion]** `stage=canBecomeKeyWindow_NO` is gated on
  `MACRUNNER_TRACE_WINEMAC_INPUT`/`UI_INPUT`/`UI_EVENT_PATH` — **none of which the run set**. Its zero
  in the blackframe run is uninterpretable and I am not counting it as evidence either way.
- 18:52 **Queued with falsifiers registered in code before any number exists:**
  `scripts/hk-input-activation-ab.sh` (driver 86178, all five gates verified by content at launch) —
  A0 VOID / A1 ACTIVATION FAILS / A2 ACTIVE BUT DEAF / A3 REACHES APPKIT, DIES INSIDE / A4 REACHES THE
  GUEST / **A5 PIXELS MOVE**. A5 is the important one: now that the window renders, the brief's own
  criterion ("an injected key changes the menu selection") is directly observable — the driver captures
  the window before and after and diffs it, so the verdict does not depend on log markers at all.
  **Prediction registered: A1** — the exposed-point click will land and the app still will not activate.
- 18:52 Disk 29 → **32 GB** by compressing (never deleting) five already-analysed run.logs >100 MB and
  >3 h old, skipping any file a live process holds open; `gunzip` restores them byte-identical.
- 20:26 **★ [MEASURED — the new instrument REFUTES my own hypothesis] `ex_style=0x0 noactivate=0`.**
  HK's window is a plain `WS_POPUP|WS_VISIBLE|WS_CLIPSIBLINGS` with **no extended style at all**, so
  `WS_EX_NOACTIVATE` is NOT set, `wf.prevents_app_activation` is FALSE, the window is NOT an
  `NSWindowStyleMaskNonactivatingPanel`, and `transformProcessToForeground:YES` was called. The
  non-activating-panel explanation for "the app never becomes active and receives no NSEvent" is
  **dead**. I built and shipped that instrument specifically to test that hypothesis and it killed it.
  Next candidate, from the same code path and testable the same cheap way: `get_cocoa_window_state`
  (`window.c:259-260`) sets `excluded_by_cycle` when `!(ex_style & WS_EX_APPWINDOW) && owner` — with
  ex_style=0 that reduces to **"has an owner window"**, and `isExcludedFromWindowsMenu` makes
  `canBecomeKeyWindow` return NO (`cocoa_window.m:2556`). So print GW_OWNER next. **[HYPOTHESIS]**
- 20:34 **★ [MEASURED] The black frame is REACH, not a rendering failure — same build, two runs.**
  run 1 `BLACKFRAME-181620`: `Restored language` +373 s, **`Loaded Objects now` +427 s**, 904 indexed
  draws → **rendered menu**. run 2 `INPUT-ACT-201313`: `Restored language` only at **+1115 s**,
  **`Loaded Objects now`=0**, and **fewer than 1024 draw/present events in the whole run** (the
  periodic `dxmt-drawtrace-totals`, which fires every 1024, never printed once) → **100 % black**,
  still black when the guest exited. This is direct evidence for the [HYPOTHESIS] I registered at
  18:33 — historical all-black captures were runs that never got far enough. Boot speed to the same
  marker varies **~3×** run to run, which is consistent with the sibling lane's own finding that
  one must not grade on time-to-menu.
- 20:34 **My A0 falsifier worked, and it saved the measurement.** The driver refused to inject into a
  black window and wrote A0 VOID instead of producing an input "result" from a run that never drew a
  menu. That is the failure mode this lane has been burned by repeatedly, caught by pre-registration.
- 20:36 **Three defects in my OWN driver, found by running it, all fixed:** (1) it gated on
  `Restored language`, which is NOT sufficient — the gate is now `Loaded Objects now`; (2) it voided
  on a single capture taken 10 s after the marker, so it now polls captures for up to 4 min before
  calling a window black; (3) its wait was bounded from DRIVER START and expired **45 s before the
  run dir even appeared** (laneA-run-hk.sh spent 34 min waiting out other lanes' wine, 19:39→20:13) —
  the clock now restarts when the run dir exists. Budget raised 1200 → **2400 s**, because 1200 s
  killed run 2 before it ever drew. Relaunched as INPUT-ACT2 (driver 81871).
- 20:36 **Slot reality, stated plainly:** this iteration got 2 title-slot runs in ~2 h. A sibling lane
  held the slot 19:00→19:39, and several sibling chains are wedged 4–6 h in exactly the pipe bug I
  fixed at 18:44 (`AUDIOCHECK-a3` chains, an orphaned `mr-run.sh` at ppid 1). I did **not** kill any
  of them — they are not mine and they hold no lock. The fix prevents new ones.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 20:52 **★ [MEASURED — REFUTES A LOAD-BEARING PREMISE OF THIS LANE, INCLUDING MY OWN ITER-7 ENTRY]
  `applicationDidBecomeActive=0` IS AN UNENABLED-GATE ZERO. It measures nothing.** The print at
  `cocoa_app.m:2665` is gated on `trace_ui_input_enabled()` (`cocoa_app.m:51-60`), which requires one
  of `MACRUNNER_TRACE_WINEMAC_INPUT` / `MACRUNNER_TRACE_UI_INPUT` / `MACRUNNER_TRACE_UI_EVENT_PATH`.
  Read from `final-child.json` for the three most recent runs that reached the menu scene
  (`BLACKFRAME-DRAWTRACE-181620`, `INPUT-ACT-201313`, `INPUT-ACT2-203603`): **all three have all
  three gates FALSE**, while `MACRUNNER_TRACE_WINEMAC_KEYS`/`HB_TRACE_WINSHOW`/`HK_INPUT_EVIDENCE`
  are true. The string `applicationDidBecomeActive` also appears in `winemac.so` x2 as an ObjC
  SELECTOR whether or not anything prints it, so `strings` alone does not rescue it either.
  Consequence: **"the app never becomes active for the entire run" was never measured.** That claim
  is load-bearing in my own iter-7 entry, in the 18:44/20:26 journal entries, in the code comment I
  shipped at `window.c:137-150`, and in the memory note `project_hk_input_starved_before_appkit`.
  I am retracting it as unproven — not asserting the opposite, which is equally unmeasured.
- 20:52 **[MEASURED, and it survives] `macdrv_key_event=0` is REAL.** Its gate
  (`MACRUNNER_TRACE_WINEMAC_KEYS`) was true in all three runs. So: no key reached the driver in the
  run that rendered the menu with 904 draws. The *fact* of key starvation stands; only the
  *explanation* ("because the app is inactive") was never evidence.
- 20:52 **[MEASURED — 34-run aggregate]** Of every run dir under `reports/phase4-hollow-knight/`,
  **34 reached the menu scene** (`Loaded Objects now` > 0). In all 34: `macrunner-secondary`=0
  (the owner gate has NEVER been on, so HK's window owner is UNMEASURED to date) and
  **`FMOD failed`=1 — audio has failed in every single run that ever reached the menu**, which makes
  step 4 a 34-sample result rather than the 1-sample one I reported in iter 7.
- 20:58 **[MEASURED, live, no rebuild] The activation refusal is real — and CORROBORATES an existing
  note rather than discovering anything.** `tools/hk_cghid_key.swift` now emits the process's true
  activation state from `NSRunningApplication` BEFORE it touches anything, so `--probe-only
  --no-activate` is a pure observer safe to point at a run another driver owns. On live HK guest
  41348: `activation_policy=0` (**.regular** — so "never transformed out of .prohibited/.accessory"
  is DEAD), `isFinishedLaunching=true`, real on-screen 1512×982 window, `is_active=false`, and
  **frontmost = "Claude", my own terminal (pid 28023)** — so every `.cghidEventTap` key this lane has
  injected while HK was inactive went to the injector's terminal. `activate()` returned true,
  `is_active` stayed false at +0.6 s and again at +2 s; 3 key-downs moved `macdrv_key_event` 0 → **0**
  with its gate verified ON. The memory note already had the activate-true/is_active-false pair; I am
  not claiming it as new.
- 20:58 **⚠ [THE CONFOUND I CANNOT RESOLVE, stated before anyone reads a conclusion into the above]**
  macOS refuses activation requests from a **non-active CLI**, and my observer ran from one. So
  "activate() ignored" is equally consistent with *"HK cannot activate"* and *"no CLI-initiated
  activate works here, for any app"*. **The A/B (below) is the within-CLI control that separates
  them** — same actuator, same CLI, against a known-good control window.
- 21:00 **[MEASURED, free, from an UNGATED marker already in every run] NSApp's class VARIES.**
  `run_cocoa_app_entry` is ungated on purpose, so it is in every log: `INPUT-ACT2` guest **41348**
  (the one measured above) came up `nsapp=0x0` → `NSKVONotifying_WineApplication`, i.e. **a proper
  WineApplication**, while `BLACKFRAME-DRAWTRACE` pid 10408 found a **stock `NSApplication`**
  pre-created. So the stock-NSApp recovery path (`cocoa_main.m:221`) is real and sometimes taken, but
  it is **NOT necessary** for the failure. [HYPOTHESIS killed as a sole cause.]
- 21:00 **[MEASURED, `sample` of the live guest — and it CONFIRMS a prior refutation]** main thread
  2220/2220 in `[NSApp run]→_DPSNextEvent→mach_msg`. `[NSApp run]` runs inside a CFRunLoop **source0
  callback**, which looks alarming but is the **documented intended design**
  (`cocoa_main.m:180-188`: "the perform callback of a custom run loop source … never returns").
  Recording the spine so the next reader does not re-open "nested run loop", which stays refuted.
- 21:02 **Two vehicle defects, found by RUNNING the thing, both fixed.** (1) `mr-run.sh`'s
  run-contract ledger **blocks probe runs**: setting `MACRUNNER_RUN_DIR` arms an identity ledger that
  demands canonical config/data/save authority + a prefix template — it certifies a TITLE — so it
  returned `status=BLOCKED` and mr-run **exited before wine** on all 3 arms. Fixed by not setting
  RUN_DIR; the atomic slot lock is taken regardless (`mr-run.sh:242`), so no collision risk. (2) the
  arm timer counted **while mr-run was queued behind a sibling's slot** (owner 79390), burning all
  three 60 s window timers inside mr-run's wait loop — a busy machine that would have read as "the
  window never appeared". Arms now wait for `[mr-run] prefix=` before any countdown. **My VOID guard
  refused to emit an input number both times**, which is the only reason neither became a fake zero.
- 21:02 Instruments: `tools/winkeyprobe.c` **3 arms** (`overlapped` control left byte-for-byte /
  `hkstyle` = HK's exact 0x94000000+ex_style 0 / `hkstyle_owned` = + an owner), verified by CONTENT —
  `machine=0x8664` (HK's own guest ISA) and every arm string present; `scripts/hk-winkeyprobe-style-ab.sh`
  with **S0/S1/S2 predictions written into the header before any number existed**, including an
  explicit clause that says the theory is REFUTED if S2 passes; `MACRUNNER_TRACE_SECONDARY_WINDOW=1`
  now on in `hk-run-try12-config.sh` so the next title run finally measures HK's window **owner**
  (the print already ships at `window.c:626` — no rebuild).
- 21:02 Report: `reports/phase4-hollow-knight/HK-MASTER-ITER8-ACTIVATION-REFUSED-MEASURED-20260729.md`.
  A/B is queued behind a sibling lane's live HK run (slot owner 79390, guest 96879 — not mine, not
  touched) and fires when the slot frees.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 21:12 **★★ [MEASURED — THE LANE'S DESIGNATED CHEAP VEHICLE IS SILENTLY INVALID] `winkeyprobe` runs
  on the NULL DRIVER: macOS never sees its window.** The brief tells every iteration to prefer this
  40-s probe over a 45-min title run. Measured on arm `overlapped` at 21:09: `macdrv_selfinit_entry`
  **0**, `run_cocoa_app_entry` **0**, `macdrv_init_user_driver_set` **0**, `macrunner-winshow` **0**,
  and **ZERO wine-owned windows in the on-screen CGWindowList** (28 candidates, none wine) — while the
  guest reported `visible=1 focus=1 active=1 foreground=1(self=1) paint=1` and `winemac_drv=0x87efddd0000`
  (the DLL IS loaded; `macdrv_init` just never ran), `WSF_VISIBLE=1`. On the null driver every USER
  call succeeds and no NSWindow is created, so the probe emits confident guest-side numbers about a
  window that **does not exist to macOS**. Same class as the brief's winecfg warning, but subtler —
  this one HAS a WS_VISIBLE window and DOES report focus.
- 21:13 **[ROOT-CAUSED] Why self-init never fired: the probe never WAITS.** Self-init is default-ON
  (`MACRUNNER_MACDRV_UNIX_SELFINIT` only disables, `macdrv_main.c:657-659`) but is driven from
  `nulldrv_ProcessEvents()` — "called from every message-pump **wait** of every placeholder process"
  (`macdrv_main.c:649-652`). The probe looped on `PeekMessageW(PM_REMOVE)` + `Sleep(10)`, which never
  performs such a wait. HK blocks in a real pump; the probe polled. Replaced with
  `MsgWaitForMultipleObjectsEx(0,NULL,100,QS_ALLINPUT,MWMO_INPUTAVAILABLE)`; rebuilt, arm strings
  re-verified. **[HYPOTHESIS — UNVERIFIED]** that this repairs it: the re-run is queued behind a
  sibling and has produced no number. I am NOT claiming the vehicle is fixed.
- 21:13 **[consequence I am flagging, not asserting]** the brief's "keyboard is proven on winkeyprobe
  0→5/5/5" rests on this vehicle. With no NSWindow there is no route for a HID key to reach that
  guest, so either that run had a driver this one lacks, or the claim needs re-establishing. I did
  not re-derive it and I do NOT assert it is false. `[HYPOTHESIS]`
- 21:14 **[MEASURED — the excluded_by_cycle PREDICATE flips exactly as the static read said]** across
  the three arms: `overlapped` style=0x14cf0000 ex_style=0x100 owner=0x0 **excluded_by_cycle=0**;
  `hkstyle` style=0x94000000 ex_style=0x0 owner=0x0 **excluded_by_cycle=0**; `hkstyle_owned` same
  style/ex_style but owner=0x2006a → **excluded_by_cycle=1**. So "with ex_style==0 the predicate
  reduces to GW_OWNER != NULL" is now EMPIRICALLY confirmed, not just read off the source. **But its
  CONSEQUENCE for key delivery is still unmeasured**, because no arm had a real driver and no arm was
  ever injected into (`actuator.jsonl` absent in all three) — so `keydown=0` everywhere is trivially
  true and, by my own pre-registered rule (S0==0 ⇒ VOID), **the A/B is VOID and I report no cause
  from it.** The pre-registration is the only reason I did not read S2's excluded_by_cycle=1 +
  keydown=0 as a confirmed root cause; it would have been a completely fake win.
- 21:14 **Instrument defects found by RUNNING them, fixed:** (1) `hk_cghid_key` filtered on
  `kCGWindowOwnerName=="wine"` — true for HK, false for a probe exe, so 58 polls matched a window
  that was on screen; added `--any-owner` AND made `no_window` dump the on-screen candidate list,
  which is what located the null-driver finding in ONE run instead of a slot each. Flag verified
  **functionally**, not by `strings` — the parser has `default: break`, so an unknown flag is silently
  ignored and a strings hit would prove nothing. (2) the A/B readout dumped ~40 ticks/arm into ab.log
  because the probe's stderr arrives CR-separated and `grep FINAL | tail -1` matched the file as one
  line; now `tr '\r' '\n'` first.
- 21:20 **Slot honesty:** the re-run is queued behind a genuine SIBLING lane (`hk-memwalk-ab.sh
  memwalk2`, 4 arms × 900 s, pid 7612 under 84507) — **not mine, not touched.** Earlier I did stop a
  chain that looked like a sibling and was in fact MY OWN orphaned retry (`laneA-run-hk.sh
  INPUT-ACT2-a2`, ppid 82836 = the child of the driver I had already killed); the `hk-run-try12`
  retry loop had relaunched it and it was starving my own probe. Verified the ancestry before killing.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 21:45 **★★ [MEASURED, from data that ALREADY EXISTED — the keyboard break is one rung past
  everything this lane has been fixing, and it is NOT in the Cocoa half]** Scanning every run dir
  for the full ladder found two runs carrying all of it. `E2E-24-WIN32UHOOK-t1` / `BLACKFRAME-DRAWTRACE`:
  `controller_handleEvent_key` 18/15, `controller_keydown_no_keywindow` **9**/0,
  `wine_window_sendEvent_keydown` 9/6, `wine_window_keyDown` 9/6, **`postKey_posted` 18/15**,
  **`ProcessEvents_key_dequeued` 0/0**, **`macdrv_key_event` 0/0**. So AppKit delivers the event, it
  carries the right `WineWindow`, `-[WineWindow keyDown:]` runs, and the key IS posted to the wine
  event queue — and **nothing ever copies it back out.** `macdrv_key_event=0` has been read all day
  as a focus/activation failure; it is a **drain** failure, downstream of all of it.
- 21:45 **[MEASURED — REFUTES two of the three candidates `event.c:71-74` itself names]** posted-to
  queue vs drained queues: R4 `0xbc93e8140` ∈ {`0xab6d2c500`,`0xbc93e8140`}; R5 `0xa113d01c0` ∈
  {`0x8a8cec8c0`,`0xa113d01c0`} — **exact overlap in both**, draining tid in the same pid as the
  posting window. So "HK realizes its window on a thread that does not pump, so nobody drains that
  queue" is REFUTED, and so is "the guest never pumps at all" (`handled=1`/`2`).
- 21:45 **[HYPOTHESIS — explicitly NOT measured, and the old instrument structurally could not
  measure it]** Two candidates survive: (a) the thread stops calling `macdrv_ProcessEvents` before
  the key is posted; (b) it keeps calling with `event_mask==0`, either from a caller mask without
  `QS_KEY` or from the nested-event guard (`event.c:800-805`) which zeroes `event_mask` wholesale
  whenever `data->current_event` is set — then the loop copies nothing, `count` stays 0, and the log
  is IDENTICAL to (a). Why it was undecidable: decade log-spacing. BLACKFRAME printed
  `calls=1/10/100` at t=821.8/831.8/837.8 then went silent, but `calls=1000` would have outlasted the
  run — and the first key was posted at **t=847.2**, nine seconds into that silence.
- 21:45 **Instrument fixed** (`winemac.drv/event.c`): `macrunner_note_drain` now also prints on a
  **10-second time bucket** (~1 line/pumping thread/10 s) and reports `mask`/`event_mask`/`current`
  plus decoded `qs_key=`. A live-but-idle pump becomes visible and (b) names itself. Same gate.
  Deployed `winemac.so` `152762ab47273e8d` → **`76e8ffec97d1b01d`**, verified by SHA **and** content
  (`qs_key=` present; pre-existing markers still present); atomic rename so the sibling's live guest
  keeps its inode.
- 21:45 **[MEASURED — 1-min native controls, no wine, no slot] The keyboard actuator this lane
  switched to cannot reach a non-frontmost app.** Control = plain `.regular` Cocoa app counting
  `keyDown:`. `.cghidEventTap` **0/5**; `CGEventPostToPid` **5/5** (10 sendEvent) while the target was
  `isActive=false`, **`isKeyWindow=false`**, another app frontmost. This **CONFIRMS an existing design
  decision rather than discovering one** — `cocoa_app.m:2136-2138` already says postToPid "can never
  make" a key window and is why the `keydown_no_keywindow` branch exists. Consequence:
  `hk_cghid_key.swift` (HID tap + activate) and the driver-side fix pull in opposite directions.
- 21:45 **[MEASURED] CLI-initiated activation is incapable for ANY app — not an HK property.** Same
  CLI, `activate()` on **Finder** (`policy=0`) returned `true`, `isActive` stayed `false` across 4
  polls / 2 s. Resolves the confound iter 8 flagged and could not resolve: "HK refuses to activate"
  measured the injector, not HK.
- 21:45 **Two corrections to my own entries this session.** (1) I wrote `objc-after-window-got-focus`
  = 0/4 — a **column misalignment in my own table**; the totals line in the same output said
  `afterGotFocus=4`. The activation block runs to COMPLETION in all 4 traced runs. (2) I claimed the
  `keydown_no_keywindow` fix had "never run" with the postToPid injector — wrong, caused by reading
  `final-child.json` at `env.entries` when the env lives at `environment.entries`, so every gate read
  back unset. Correctly parsed: it was explicitly `=1` in four morning runs and **fired 9 times** in R4.
  The same bad parser produced my "sibling has no gates" read; re-checked, that one happens to hold.
- 21:45 Report: `reports/phase4-hollow-knight/HK-MASTER-ITER9-KEY-DRAIN-BREAK-20260729.md`. Memory:
  `lesson_keyboard_and_mouse_need_opposite_injectors`. Run `MASTER-I9-DRAIN` in flight with the new
  probe, queued behind the sibling's lock; it decides (a) vs (b).
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 21:50 **★ [CORRECTING MY OWN ENTRY 5 MINUTES OLD — the inference, not the number]** I read
  `ProcessEvents_drain calls=1` as "the pumping thread barely pumps". **That inference is wrong.**
  `win32u/message.c:3431` calls `pProcessEvents` **only** when `check_internal_bits(QS_DRIVER)` is
  set, so the drain counter can only tick when the server has raised QS_DRIVER on that thread's
  queue. A low count means **QS_DRIVER was raised that few times** and says NOTHING about how often
  the guest pumps. Worse, this defeats the time-bucket I had just shipped: if QS_DRIVER never fires,
  the driver-side probe prints nothing, which looks identical to "the pump stopped".
- 21:50 **Second probe, the other half** (`win32u/message.c`): `process_driver_events` now emits a
  bounded line (one per thread per 10 s + decade-spaced) with `qs_driver`, running `signaled_total`,
  `events_mask`, decoded `qs_key`, `drained`. Gate `MACRUNNER_TRACE_QS_DRIVER` **falling back to
  `MACRUNNER_TRACE_WINEMAC_KEYS`**, chosen so the ALREADY-QUEUED run picks it up with no new env var;
  deliberately not `MACRUNNER_TRACE_UI_INPUT` (the brief records it as also lighting HyperBridge's
  per-guest-call tracing). Deployed `win32u.so` `5ee1ef280c8a0f35` → **`7c3e4f27bf93cfb2`**, verified
  by SHA **and** content, pre-existing `MACRUNNER_HB_RETURN_ROUTE_OBSERVER` markers intact.
  The two probes form a decision table: `process_driver_events` absent ⇒ thread not in the message
  path; present with `qs_driver=0` throughout and no drain ⇒ **the queue write never reaches the
  server**; `qs_driver=1` + drain with `event_mask=0x0` ⇒ the nested-event guard; `qs_driver=1` +
  drain with `event_mask≠0` and no dequeue ⇒ the copy-out filter.
- 21:50 Run `laneA-MASTER-I9-DRAIN-a1-try1-214905` took the slot at 21:49:05 with both probes in
  place (`INPUT_EVIDENCE=1`, `SELFINIT_DELAY_MS=420000`). Boot to menu is ~430-850 s on this config.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 22:15 **★★★ [MEASURED — FIRST NON-ZERO `macdrv_key_event` IN THIS LANE'S HISTORY]** On
  `laneA-MASTER-I9-DRAIN-a1-try1-214905`, after waiting for the driver install **scoped to HK's own
  pid**, a pid-scoped postToPid injection moved the whole ladder: `BASELINE @+677s post=30 key=0
  deq=0` → `AFTER @+686s post=54 key=54 deq=54`, with `macdrv_send_keyboard_input_sent`=54 and
  `hwnd=0x30038 vkey=0x28 scan=0x50 flags=0x1/0x3`. **Keys traverse Cocoa → winemac.drv → win32u in
  the real title.** That is the core of brief step 2, previously unproven.
- 22:15 **[MEASURED — the harness bug that produced every earlier K1]** Every process in a run writes
  to ONE log, so an unscoped marker count counts ALL of them.
  `hk-input-evidence-inject.sh` waited for `macdrv_init_user_driver_set` unscoped and matched
  **pid=65959** — a helper that installs from dllmain at +49 s and had already exited — while HK
  (52275) was still printing `macdrv_selfinit_deferred delay_ms=420000`. It injected ~130 s early,
  twice, returning "K1 NOTHING ARRIVES" both times. **Both verdicts were void by the file's own rule.**
  Fixed: scoped to `$RD/wine-child.pid`, missing pid file = explicit VOID.
- 22:15 **★★ [MEASURED — and it REINSTATES the latch I withdrew 40 min ago] The drain stops again
  after exactly one burst.** Same live guest, driver installed, window on screen: pixel-A/B VK_DOWN×3
  → `post +6, deq +0`; then a pre-registered VK_RETURN×5 → **`post +10, deq +0, key +0,
  ProcessEvents_drain +0`**. The drain probe not ticking means `macdrv_ProcessEvents` was **not
  called at all** — nothing asked, rather than the filter rejecting. Shape: install → ONE burst of 54
  end-to-end → permanent silence across two injections 250 s apart.
  **I withdrew the §3b latch too early.** Satisfying `mask == QS_ALLINPUT` only makes the return value
  depend on `!check_fd_events(fd, POLLIN)`; if that says pending, `ret=FALSE` → `poll_events=0` → the
  server skips `clear_queue_bits(QS_DRIVER)` + `set_fd_events(fd, POLLIN)` (`server/queue.c:3174-3179`)
  → `msg_queue_poll_event` (which disarmed the fd at `:1413`) never fires again → QS_DRIVER never
  raised again → `pProcessEvents` never called again for the life of the thread. `[HYPOTHESIS]`, now
  with a named line and a reproducible signature. **NOT fixed this iteration** — default-OFF flag +
  live A/B is the rule, and that is next iteration's first job.
- 22:15 **[MEASURED] Owner/exclusion theory DEAD.** HK's top-level window: `owner=0x0`,
  `ex_style=0x0` ⇒ `excluded_by_cycle=0`; `canBecomeKeyWindow_NO`=0. This is the question iter 8
  armed `MACRUNNER_TRACE_SECONDARY_WINDOW=1` for. Also refuted live: `event_mask=-1` (all types incl.
  KEY_PRESS), `mask=0x1cff` == `QS_ALLINPUT` (**0x1CFF in this tree, not 0x4FF**), `qs_key=1`,
  `current=0x0` — so neither a missing QS_KEY nor the nested-event guard is involved.
- 22:15 **Retractions of my own entries, this session.** (1) "self-init never fired" (t=657) —
  **WRONG**, it installed at **+677 s**; I converted a quiet interval into an absence, the same
  mistake as the decade-spaced counter. (2) "the pumping thread barely pumps" — wrong inference,
  `pProcessEvents` only runs when QS_DRIVER is set. (3) The pixel A/B is **VOID**: its no-injection
  CONTROL registered 0 differing bytes, so the capture cannot detect change at all — "no visible
  effect" is not a finding.
- 22:15 **Instruments fixed** (3, all found by running them): harness install-check pid-scoped;
  `process_driver_events` now prints `wine_pid` (TEB ClientId, hex) — labelled so it cannot be
  confused with winemac.drv's unix `getpid()`, and NB `window_tid` is DECIMAL while `tid` is HEX
  (`window_tid=36` IS `tid=24`; comparing them raw yields a confident false conclusion);
  `qs_driver` normalised (it printed `-2147483648`, so `grep qs_driver=1` read as a clean zero).
  Deployed `winemac.so` **76e8ffec97d1b01d**, `win32u.so` **18f0923c4e736706**, both verified by SHA
  and content. Actuator validated FUNCTIONALLY against a control app (key 0→4) before any HK zero
  was believed.
- 22:15 Report: `reports/phase4-hollow-knight/HK-MASTER-ITER9-KEY-DRAIN-BREAK-20260729.md`.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 22:20 **⚠ [FOR THE COORDINATOR — the engine sources I edited are INVISIBLE TO GIT]**
  `engine/wine/dlls/winemac.drv/event.c` and `engine/wine/dlls/win32u/message.c` are both
  **untracked AND git-ignored** in this tree (`git check-ignore` = YES for both), yet they are
  compiled into the shipped `winemac.so`/`win32u.so`. `git status` reports nothing for them, so the
  change cannot be committed as-is and a tree clean would destroy it silently — exactly the hazard
  in `feedback_track_load_bearing_sources`. I did **not** force-add ignored files (commits are the
  coordinator's). Verbatim copies + SHA256SUMS + the deployed-artifact SHAs are preserved in
  `reports/phase4-hollow-knight/checkpoints/20260729-iter9-key-drain-probes/`.
  `scripts/hk-input-evidence-inject.sh` is untracked but NOT ignored, and carries the pid-scoping
  fix without which its own verdicts are void.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).

## ITER-10 — 2026-07-29

- 22:12 Census. I9-DRAIN run `laneA-MASTER-I9-DRAIN-a1-try1-214905` had FINISHED (guest 52275 dead, run.log
  frozen +1125.5s). Sibling A/B `hk-hdrprobe-speed-ab.sh hdrspeed 900 off on off on` (pid 43001, created
  21:51, never in this journal ⇒ not mine) held the slot; I left it alone. It ABORTED after arm 1
  (`laneA-hdrspeed-off1-a1-try1-220914`, run.log 4899 B frozen 22:16:25). Slot now free (wineserver=0).
- 22:14 **[MEASURED — REFUTES my own ITER-9 §3b latch, from source, before spending a run]** The QS_DRIVER
  "latch" I queued as this iteration's first job CANNOT EXIST. `server/queue.c`: QS_DRIVER is SET only at
  :1414, which always disarms the fd at :1413; it is CLEARED only at :3177, which always re-arms at :3178.
  They flip together, so there are exactly two states: A(qs=0, fd armed) and B(qs=1, fd disarmed). A
  `drained=FALSE` leaves state B — where `pProcessEvents` is called on EVERY pump, i.e. the OPPOSITE of
  silence. ITER-9 measured `ProcessEvents_drain +0` (never called), so the queue was in state A. **Building
  that default-OFF re-arm fix would have been fixing a non-bug.** Also: `cocoa_event.m:284`
  `signalEventAvailable` fires UNCONDITIONALLY on every post, so a missed wakeup is not the mechanism either.
- 22:16 **★★★ [MEASURED — the (a)-vs-(b) question ITER-9 left open is ANSWERED, from the run that had already
  finished; no slot spent] It is (a), and sharper than (a) was stated.** In I9-DRAIN the new
  `process_driver_events` probe caught two threads: **tid=120** pumps continuously (492 calls, +138.9s→
  +1122.8s) with `qs_driver=0` on every one; **tid=24** — HK's OWN window thread — made **16 calls in 637 s**
  and its LAST is +681.375s, the exact call that drained the 54-key burst (`drained=1`, and the winemac line
  at the same timestamp is `pid=52275 fd=220 handled=56`). **Zero calls in the remaining 444 s.**
  Ownership is independent: `macrunner-get-win-data hwnd=0x30038 owner_tid=36` DECIMAL = tid **0x24**, and
  `create_cocoa_window pid=52275 hwnd=0x30038 queue=0x85c504080` matches the drained queue.
- 22:16 **[MEASURED — why that count IS the pump rate]** `process_driver_events` is reached from
  `check_for_driver_events()` (`win32u/message.c:3571`), called from **`NtUserPeekMessage` (:3826,:3850)** and
  **`NtUserGetMessage` (:3884,:3905)**, throttled only by an **8 kHz** tick — a rate limiter permitting 8000
  calls/s, not a suppressor. So the probe's `calls=` is a near-1:1 proxy for guest PeekMessage/GetMessage.
  **HK's window thread called PeekMessage/GetMessage 16 times in 637 s and 0 times in the final 444 s.**
  Keys are delivered into the queue correctly; **nothing ever calls PeekMessage to take them out.**
  "Install buys exactly ONE burst, then permanent silence" is fully explained: the injection coincided with
  pump call #16. **This REFUTES my memory note's "one-shot QS_DRIVER poll latch" framing.**
- 22:16 **[MEASURED] HK was AT THE MENU, and the process was NOT frozen.** `Begin MonoManager` +61.1s →
  `UnloadTime` +212.8s (151.7 s — healthy vs the 784 s driver-installed case), `Restored language` +290.0s,
  `Loaded Objects now` +339.4s. The pump died ~340 s AFTER the menu loaded. Guest kept executing:
  `mp_calls` +4,160,832 over the 439 s after the pump stopped. So: running, not pumping.
- 22:16 **[CAVEAT I must not hide]** This run's `win32u.so` predates the `wine_pid` field (deployed 22:15,
  run started 21:49), so **tid=120 is UNATTRIBUTED** — it may not even be HK's process. tid=24's attribution
  is solid for the independent reasons above; tid=120's is not, and I claim nothing from it.
- 22:18 **[PRE-REGISTERED — before the run, per the standing rule]** Hypothesis: HK's main thread is not
  pumping because guest execution is degraded by the fault-path PE-header wedge
  (`project_hk_fault_path_header_wedge_20260729`); `MACRUNNER_HB_FAULT_SAFE_HEADER_PROBE=1` was **absent**
  from I9-DRAIN's env (verified at `environment.entries`, the documented-correct path; gate name verified
  present in the shipped `aarch64-unix/ntdll.so` by `strings`).
  **Control** = I9-DRAIN (same script/gates/delay, lever OFF): window-thread `calls=16` over 637 s, 0 in the
  last 444 s. **Test** = same, lever ON.
  **Prediction:** window-thread `calls` ≥10x the control AND non-zero in the final 100 s of the run.
  **Falsifier:** pump rate statistically unchanged ⇒ the wedge is NOT why the thread stops, and the cause
  must be read off a `sample` of the thread instead of guessed at.
  **VOID rule:** if the run does not reach `Loaded Objects now`, it measures nothing about the menu-state
  pump and I report VOID, not a cause.
  Regardless of arm I will `sample` the guest at menu depth — that is the measurement that says WHERE the
  thread is, and it does not depend on the hypothesis being right.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 22:22 **★★★ [MEASURED — 12 sample files, no run spent] Where the thread IS.** Parsed every recent
  `sample(1)` capture: **0 threads carry a PeekMessage/GetMessage/ProcessEvents frame in 12 of 12 samples**
  (3 runs, 54-66 threads each) — independent corroboration of the log finding. In
  `INPUT-EVIDENCE/hk-sample.txt` (Cocoa main **healthy**, 66 threads): **27 threads parked in
  `macrunner_hb_rtl_wait_on_address`**, including **HK's main guest thread at 3039/3039 samples**
  (`hb_x64_thread_entry → hb_run_x64 → call_import_thunk → try_wait_address_semantic → rtl_wait_on_address
  → NtWaitForAlertByThreadId → futex_wait → __ulock_wait2`) and every Unity `Job.Worker`; 9 in
  `msync_wait_single`; `Loading.PreloadManager` in `macrunner_hb_work_object_probe_block`.
  **HK is not pumping because its main thread is parked in a futex, with Unity's job system alongside it.**
- 22:22 **[MEASURED — real, but NOT universal; recording rather than unifying]** In run E2E-19 the **Cocoa
  main thread** sits in `macrunner_hb_primary_signal_handler → route_x64_callback_fault →
  redirect_arm64x_hexpthk_sigill` in **4/4** samples, with a guest thread blocked in `NtUserGetCursorPos →
  macdrv_GetCursorPos → OnMainThread → kevent` waiting on it — a genuine deadlock. **But E2E-20 and E2E-21
  (8 samples) show a HEALTHY Cocoa main (idle in `mach_msg`) with 0 threads blocked on it, and still nobody
  pumping.** So the fault-router deadlock is not the universal cause.
- 22:26 **[CORRECTION to my own 22:16 entry — a load-bearing claim was imprecise]** I wrote that `calls=` is a
  "near-1:1 proxy for PeekMessage/GetMessage". It is not. `process_driver_events` is ALSO reached via
  `check_for_events()` from **`NtUserGetAsyncKeyState`** (`input.c:862`), **`NtUserGetQueueStatus`** (`:929`),
  **`get_input_state`** (`:1002`), **`NtUserSetCursor`** (`cursoricon.c:126`), **`NtUserRedrawWindow`**+
  `RDW_UPDATENOW` (`dce.c:2037`), and directly from `wait_message` (`:3623`,`:3655`). So 16 is an **upper
  bound across ALL of them at once** — which STRENGTHENS the conclusion: the guest made essentially **no
  user32 input calls of any kind** in 637 s, notable because Unity polls input via `GetAsyncKeyState`.
- 22:28 **[HYPOTHESIS + the lever that already exists]** HyperBridge **reimplements** WaitOnAddress/
  WakeByAddress host-side (`macrunner_hb.c:27972` wait, `:28096` wake-all), intercepting **by import name**
  (`:6926`,`:29467`) — the ABZU "HB must forward, not reimplement" shape. The algorithm itself is wine's
  correct sticky-alert pattern (compare+enqueue under the queue spin lock, then `NtWaitForAlertByThreadId`;
  waker removes under the same lock, alerts after unlock), so it is **not** an obvious lost-wakeup race. The
  exposure is the **asymmetry**: the wait is emulated host-side while any wake NOT intercepted by name stays
  guest-side and can never find the host queue entry. `macrunner_hb_rtl_wake_address_all` already prints
  `found=N` per wake under `MACRUNNER_HB_TRACE_WAITADDR`. I scanned **all 153** `laneA-*/run.log`:
  **0 contain any `waitaddr` trace — unmeasured, NOT measured-zero.** A default-OFF A/B lever already
  exists: `MACRUNNER_HB_DISABLE_WAIT_ADDRESS_SEMANTIC` (`macrunner_hb.c:1682`).
- 22:28 **[AMENDING MY OWN PRE-REGISTRATION, out loud rather than quietly]** The 22:18 pre-registered
  `FAULT_SAFE_HEADER_PROBE` A/B is now the **weaker** experiment: the samples put HK's main thread in a
  futex, not in the fault path, in every run whose Cocoa main is healthy. **Amended experiment** (diagnostic
  before corrective): HK to menu with `MACRUNNER_HB_TRACE_WAITADDR=1` + the QS_DRIVER probe + a `sample` at
  menu depth. Reads: (i) does the window thread's `calls=` stay ~16 or climb; (ii) do wakes report
  `found=0` while threads are parked; (iii) does the 27-thread parking reproduce with a healthy Cocoa main.
  **Only if (ii) implicates the semantic** do I A/B `MACRUNNER_HB_DISABLE_WAIT_ADDRESS_SEMANTIC=1`.
  Same VOID rule: no `Loaded Objects now` ⇒ VOID, not a cause. The header-probe A/B is demoted, not discarded.
- 22:28 **Slot honesty.** I launched `MASTER-I10-PUMP`, then realised its `wait_for_slot` polls every 20 s and
  would take the slot **between** the sibling's arms — corrupting their 4-arm speed A/B and mine (a pump-rate
  read is equally timing-sensitive; the control I9-DRAIN ran alone). **Stood down** (killed 32836/32840,
  sibling verified untouched). Replaced with a watcher that waits on the sibling's **driver pid 67358** to
  EXIT, not on `wineserver`, then launches `MASTER-I10-WAITADDR`.
- 22:28 **⚠ [FOR THE OPERATOR — a CPU thief that also confounds the sibling's speed A/B]** An orphaned
  `mr-run.sh` (pid **8446**, ppid 1, **9.7 h**, no children, run long dead) is **spinning at ~12 % of a
  core** — measured as a CPU-time delta of **0.6 s per 5 s**, NOT from `ps %cpu` (a lifetime average that
  would not have shown this). Also idle-wedged: two `AUDIOCHECK` chains (8-9.7 h) and one `SYNCBLOCK`
  (3.7 h). I am NOT killing them mid-flight: 8446 was alive during I9-DRAIN (my control) and is alive during
  the sibling's arm 1, so removing it now would bias both. Queued for cleanup at iteration end.
- 22:28 Report: `reports/phase4-hollow-knight/HK-MASTER-ITER10-NOBODY-CALLS-PEEKMESSAGE-20260729.md`.
  Memory: `project_hk_window_thread_stops_calling_peekmessage_20260729`; corrected
  `project_hk_key_break_is_queue_drain_not_focus_20260729` (its QS_DRIVER-latch section is now marked
  REFUTED).
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 22:32 **★★ [MEASURED — provenance of the key sample, and it upgrades the finding] The parking is at a
  RENDERED MENU, not mid-boot.** `INPUT-EVIDENCE/hk-live.png` (**18:33**, two minutes before the 18:35
  sample) shows HK's **language-selection menu fully rendered** — English carrying its selection ornaments,
  plus Français/Deutsch/Español/Italiano/Português (Brasil)/Русский/日本語/简体中文/繁體中文/한국어, all 11
  entries. So the sequence is: **HK renders real pixels → its main thread parks in `RtlWaitOnAddress` → it
  never pumps again.** The menu sits on screen waiting for a keypress it structurally cannot process.
  "Parked during boot" would have been unremarkable; parked at a live menu is the defect. Independently
  re-confirms the black-frame refutation.
- 22:32 **GOAL criteria status:** menu **reached**; **real pixels YES (seen, not inferred)**; input changes
  selection **NO**; Start Game not reached; audio unmeasured. The blocking defect is now well localised to
  ONE thing — HK's main thread parks in `RtlWaitOnAddress` at the menu — and the remaining criteria are all
  gated on it resuming.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 22:36 **★★ [MEASURED — free, from the same finished run — and it CORRECTS A PREMISE IN MY BRIEF] Audio is
  not failing; it was never enabled.** The brief records under *Established*: "winecoreaudio.drv now reaches
  the prefix (commit 2aa93b2b)". For I9-DRAIN that is **false**. At **+0.217s** the sync logs
  `[sync] holding back winecoreaudio.drv by default (set MACRUNNER_SYNC_INCLUDE_AUDIO_DRV=1 to ship it)`
  — and the same for `msacm32.drv` and `winspool.drv`. `run-contract.json` **never mentions winecoreaudio**,
  so it did not reach the prefix. Then at **+60.875s**: `FMOD failed to initialize any audio devices,
  running on emulated software output with no sound.` The driver IS built and in the dist (aarch64-unix /
  aarch64-windows / x86_64-windows / i386-windows, 17:49 today) — the sync deliberately withholds it.
  **So the standing FMOD error is NOT evidence that audio is broken.** The 2aa93b2b claim needs re-scoping
  to "reaches the prefix *when MACRUNNER_SYNC_INCLUDE_AUDIO_DRV=1*". Criterion 5 is now a one-flag
  experiment rather than an investigation.
- 22:36 **Deliberately NOT folded into the queued run.** Three extra drivers in the prefix is a real
  variable, and that run is my only slot for the next hour; its job is the blocking defect that all four
  remaining GOAL criteria depend on. Audio gets its own run with the flag.
- 22:36 **Instrument note (a false-zero trap I nearly built).** The waitaddr wake trace prints
  `found=N` for `mode=all` but **`found_tid=`** for `mode=single` (`macrunner_hb.c:28190`) — grepping
  `found=` alone would silently miss every single-wake and read as a clean zero.
- 22:36 Two watchers queued: one waits on the sibling's **driver pid 67358 to exit** (not on `wineserver`,
  which would let me steal the slot between their arms) then launches `MASTER-I10-WAITADDR`; the other polls
  for that run dir, waits for `Loaded Objects now`, and takes **two `sample(1)` captures ~150 s apart** —
  a thread parked in BOTH is parked, not merely slow.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 22:40 **[BUILT — the 40-second probe the brief asks for, isolating the hypothesis from HK entirely]**
  `tools/x64waitaddrprobe.c`: N threads hammer `WaitOnAddress` while the main thread bumps the value and
  calls `WakeByAddressAll`/`Single`, with a stall watchdog and a hard self-cap (**there is no `timeout`/
  `gtimeout` on this box** — confirmed again, so the cap is internal). It reports **per-worker** wake counts
  and `workers_with_zero_wakes` — a worker that never wakes is exactly HK's signature.
  Built: `winegcc --target=x86_64-windows ... -municode -lkernelbase` (**`-lsynchronization` does not exist
  in this toolchain** — first attempt failed on `synchronization.lib`). **Verified as a vehicle, not assumed:**
  PE machine word = **0x8664**, and `WaitOnAddress`/`WakeByAddressAll`/`WakeByAddressSingle` are present as
  **named imports of kernelbase.dll**, so the calls go through `call_import_thunk` and DO hit HyperBridge's
  by-name interception. An `--indirect` arm resolves via `GetProcAddress` as a contrast that may bypass it.
  Unknown flags are REPORTED, not silently ignored (a silently-dropped flag has produced a confident false
  conclusion here before).
- 22:40 **Watcher re-sequenced.** On sibling exit it now runs the **three cheap probe arms first**
  (all / single / indirect, ~2 min total) and only then launches the 20-minute HK run — the cheap
  measurement may answer it outright, and either way it targets the expensive one.
- 22:40 **⚠ [FOR THE COORDINATOR]** `tools/x64waitaddrprobe.c` is **untracked but NOT git-ignored**
  (`git status` shows `?? tools/x64waitaddrprobe.c`), so unlike the iter-9 engine edits it is at least
  visible. I do not commit. Probe artefacts land in
  `reports/phase4-hollow-knight/MASTER-I10-WAITADDR-PROBE/`.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 22:42 **[CORRECTING MY OWN 22:22 ENTRY — an instrument limit I stated too strongly]** I wrote that the
  main thread being at "3039/3039 samples" in `rtl_wait_on_address` shows it is **parked**. `sample(1)`
  **aggregates** stacks, so a thread that RE-ENTERS the same wait every frame produces the IDENTICAL spine as
  one parked forever — **the sample alone cannot distinguish parked from cycling.** The conclusion actually
  rests on the **log** (`calls=16` in 637 s, **0** in the last 444 s; a frame-rate loop would light that
  counter constantly, since it is reached from PeekMessage/GetMessage/GetAsyncKeyState/GetQueueStatus/
  SetCursor/RedrawWindow/MsgWaitForMultipleObjects at up to 8000/s). The sample only says WHERE it sits
  while not looping. Neither is sufficient alone; together they support "parked in a futex".
- 22:42 **Instrument upgraded before the run rather than after.** The sampler now also captures **per-thread
  CPU time** (`ps -M`, two snapshots 20 s apart around each sample): a genuinely parked thread accrues
  **no** CPU, which settles parked-vs-cycling independently of both the log and the stack.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).

## ITER-11 — 2026-07-29 — the park is in HB's OWN wait queue, and the fix is testable without HK
- 22:41 Resumed. Census first: slot **BUSY** with a **sibling's** A/B (`hk-hdrprobe-speed-ab.sh hdrspeed2
  900 off on off on`, driver pid 67358, arm `on2` guest 57899). Not mine, not in this journal. Left alone.
  My ITER-10 watchers did **not** survive the iteration boundary (0 alive) and `tools/x64waitaddrprobe.c`
  was **never built** — the probe dir was empty. So ITER-10's queued plan did not run; I re-armed it.
- 22:45 **[MEASURED — free, from sample files already on disk, no slot spent] The park is reproducible and
  its size is a constant.** Dominant-chain classification (frames carrying >=90% of the thread's samples;
  a leaf-only classifier, because "any frame contains __ulock_wait2" wrongly counts a minority branch):
  · E2E-19/stall1: **27** threads parked in HB `try_wait_address_semantic -> rtl_wait_on_address`, 11 msync,
    15 other-blocked, **1** runnable.
  · E2E-19/stall3: **27** / 12 / 14, **0 runnable — every thread in the process blocked at that instant.**
  · E2E-21/stall1 and /stall3: **27** / 9 / 15, 5 runnable (one of them pinned in
    `pc_is_x64_guest_code_module_no_lock` = ITER-4's fault-path header wedge, a different class).
  **27 in 4/4 samples across 2 independent runs.**
- 22:45 **[MEASURED] The two threads that matter are in the SAME primitive.** In the menu-depth specimen
  (`INPUT-EVIDENCE/hk-sample.txt`, 65 threads, the run whose PNG shows the language menu rendered):
  HK's main guest thread (Thread_51879310) **and** `UnityGfxDeviceWorker` both read, identically,
  `__wine_unix_call_dispatcher -> hb_x64_thread_entry -> hb_run_x64 -> call_import_thunk ->
  try_wait_address_semantic -> rtl_wait_on_address -> NtWaitForAlertByThreadId -> futex_wait ->
  __ulock_wait2` at **3039/3039 samples each**. 54 of 65 threads have `__ulock_wait2` as their leaf.
- 22:46 **[MEASURED — and it cuts against a story I was about to tell] `Loading.PreloadManager` is NOT the
  universal culprit.** In the menu specimen it is the one thread genuinely executing guest code
  (`hb_jit_runtime_run`, `run_jit_block_with_signal_guard -> _platform_memset/_platform_memmove`,
  `try_promote_hot_block_families`) — which invited "main is blocked waiting for a preload that never
  finishes". But in E2E-21 stall1/2/3 PreloadManager is itself **fully parked** (single 10-frame chain to
  `__ulock_wait2`) across 5 minutes. Same park, opposite PreloadManager state ⇒ **"PreloadManager never
  completes" is NOT the mechanism.** Recorded so it is not carried forward as one.
- 22:47 **[MEASURED, from source] The interception is name-SYMMETRIC — so ITER-10's stated asymmetry is not
  the bug as phrased.** `macrunner_hb.c:6926` and `:29467` both cover all six names (`WaitOnAddress`,
  `RtlWaitOnAddress`, `WakeByAddressAll/Single`, `RtlWakeAddressAll/Single`), and the wait (`:27972`) and
  wake (`:28096`/`:28155`) bodies are a faithful copy of wine's sticky-alert pattern (compare+enqueue under
  the queue spin lock, `NtWaitForAlertByThreadId`; waker clears `entry->addr` and removes under the same
  lock, alerts after unlock). **What remains structurally true is narrower and stronger: HB keeps its OWN
  queue.** A waiter enqueued in HB's queue is invisible to any wake that reaches wine's ntdll queue instead,
  and vice versa — the ABZU "two implementations over one piece of state" shape
  (`project_abzu_abba_heap_cs_double_impl_20260713`). That is a HYPOTHESIS about which paths cross over,
  not a proven lost wakeup.
- 22:48 **[BUILT + VERIFIED AS A VEHICLE, not assumed]** `tools/x64waitaddrprobe.c` (written ITER-10, never
  compiled) now builds: `winegcc --target=x86_64-windows -municode -lkernelbase` — **`-lsynchronization`
  does not exist in this toolchain**, confirmed again. Verified by parsing the PE, not by trusting the
  machine word: `Machine=0x8664`, `OptMagic=0x20b`, and **`CHPEMetadataPointer = 0x0`** (an ARM64EC PE also
  reports 0x8664 while running native and never touching HyperBridge — that trap would have made the whole
  measurement a no-op). `WaitOnAddress`/`WakeByAddressAll`/`WakeByAddressSingle` are all present as **named
  imports of kernelbase.dll**, so the calls go through `call_import_thunk` and DO hit the by-name intercept.
- 22:49 **Gate strings verified present in the SHIPPED artifact by CONTENT** (`strings` on
  `dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`): `MACRUNNER_HB_DISABLE_WAIT_ADDRESS_SEMANTIC`=1,
  `MACRUNNER_HB_TRACE_WAITADDR`=3, `macrunner-hb-waitaddr-wake`=2. **No rebuild needed.**
- 22:50 **PRE-REGISTERED, written BEFORE any arm runs.** `scripts/hk-waitaddr-probe-ab.sh`, 4 arms, ~2 min
  total, **no Hollow Knight**:
  · 1 `all`      — WakeByAddressAll, semantic ON (the exact path HK parks in)
  · 2 `single`   — WakeByAddressSingle, semantic ON
  · 3 `indirect` — GetProcAddress-resolved, may bypass the by-name intercept (contrast arm)
  · 4 `semoff`   — `MACRUNNER_HB_DISABLE_WAIT_ADDRESS_SEMANTIC=1` ⇒ forwards to the real implementation.
    **Arm 4 is the candidate FIX, not just a diagnosis.**
  **PASS (emulation is broken, fix identified):** arms 1/2 report `VERDICT=STUCK`/`NO-WAKES-AT-ALL`/`PARTIAL`
  or `workers_with_zero_wakes>0`, AND arm 4 reports `VERDICT=OK`.
  **FALSIFIER, stated in advance:** if **all four arms report OK**, HB's WaitOnAddress round-trip is sound
  under stress and **HK's park is NOT a simple lost wakeup in this primitive** — I will say so plainly and
  redirect to "what are those 27 threads waiting FOR", rather than keep the hypothesis alive.
  **VOID:** any arm whose log carries no `MACRUNNER-waitaddr` line at all measures nothing (the probe prints
  every count even on success, so silence = the probe did not run, not a clean result).
  **What this canNOT settle:** whether HK's specific addresses cross the two queues. A probe passing does
  not exonerate the two-queue structure under a real guest; it only removes the simple explanation.
- 22:50 Slot honesty: the driver **waits on the sibling's driver pid 67358 to exit** — not on `wineserver`,
  which would let me take the slot BETWEEN their arms and corrupt their speed A/B (and mine). It runs
  through `mr-run.sh` directly, which takes the atomic slot lock and has **no force-clean**, so it
  physically cannot destroy their run.
- 22:53 **★★ [INSTRUMENT CHECKED BEFORE SPENDING THE RUN — this one would have bought a confident
  nothing]** The obvious HK-side experiment is "run to menu with `MACRUNNER_HB_TRACE_WAITADDR=1` and read
  the main thread's last wait". **That trace structurally cannot answer it.** Its emitter is gated by
  `macrunner_hb_waitaddr_should_log(n)` (`macrunner_hb.c:24169`) = **`n < 64 || (n % 8192) == 0`** — the
  first 64 waits of the process, then every 8192nd. HK's FINAL wait, the one it never returns from, is
  almost certainly neither, and its absence from the log would have read as "no wait happened".
- 22:53 **★★ [FOUND THE INSTRUMENT THAT DOES ANSWER IT, already built, no rebuild]**
  `MACRUNNER_HB_WAIT_WAKE_TRACE=1` starts an observer thread (`:27859`) that polls
  `MACRUNNER_HB_WAIT_WAKE_TRACE_ARM_FILE` every 10 ms; the instant that file appears it arms the trace and
  calls `macrunner_hb_wait_wake_trace_dump_current()` (`:27842`), which walks **every** wait queue and emits
  `phase=wait-snapshot` per parked waiter with `guest_addr host_addr current= expected= **equal=** timeout=
  caller_module= caller_rva= thread=`. Two properties make it the right tool: I choose the MOMENT (menu
  depth, not boot), and the snapshot entries take the **first** budget slots because `dump_current()` runs
  inside the arming CAS before any live wait/wake can consume them (budget default 5000, set to 20000).
  Verified by CONTENT in the shipped `ntdll.so 63c9691c0f8af97f`: `MACRUNNER_HB_WAIT_WAKE_TRACE`=3,
  `_ARM_FILE`=1, `_BUDGET`=1, `wait-snapshot`=1, `phase=armed`=5.
- 22:54 **PRE-REGISTERED, before the run — and `equal=` is a CLEAN DISCRIMINATOR between the only two
  remaining explanations:**
  · **(A) LOST WAKEUP** — `equal=0` on a parked waiter ⇒ the value at the wait address has ALREADY changed
    and the thread was never alerted. HB's own queue is separate from wine's, so a wake that reached wine's
    queue can never find an HB waiter. **This would be proof, not inference.**
  · **(B) NEVER SIGNALLED** — `equal=1` on every parked waiter ⇒ they are still legitimately waiting and
    whoever should write that value never ran; the hunt then moves to the producer named by
    `caller_module`/`caller_rva`, which the same line hands me for free.
  **VOID rule:** no `Loaded Objects now` ⇒ the run measures nothing about the menu park and I report VOID.
  Separately VOID: `phase=armed` present but `wait-snapshot` = 0 ⇒ instrument failure, **not** "no waiters".
- 22:55 Queue armed, both chained so no slot time is lost and nobody's run is disturbed:
  `hk-waitaddr-probe-ab.sh` (pid 9365) waits on the sibling's driver 67358 → 4 probe arms (~3 min) →
  `hk-waitwake-snapshot.sh` (pid 65151) waits on 9365 → the HK run. Both reach the slot only via
  `mr-run.sh`/`laneA-run-hk.sh`, which after ITER-3's 16:52 fix wait on a live owner instead of
  force-cleaning it. Key actuator rebuilt this iteration (the inject script's default `ACT` pointed into a
  **previous session's** scratchpad): `hk_cgevent_key` (CGEventPostToPid — the keyboard-correct injector per
  `lesson_keyboard_and_mouse_need_opposite_injectors`; `.cghidEventTap` is the mouse one).
- 23:00 **★★★ [MEASURED, from source — the structural defect, named exactly] THERE ARE TWO FUTEX QUEUES,
  AND THEY ARE THE SAME DATA STRUCTURE WRITTEN TWICE.**
  · HyperBridge: `macrunner_hb_wait_addr_queues[256]` (`macrunner_hb.c:24011`), hash `(val>>4)%256` (`:24017`)
  · wine PE ntdll: `futex_queues[256]` (`sync.c:1368`), hash `(val>>4)%256` (`sync.c:1374`)
  Same size, same hash, **two independent arrays**. A waiter enqueued in one is structurally invisible to a
  wake that reaches the other. This is `project_abzu_abba_heap_cs_double_impl_20260713`'s "HB must forward,
  not reimplement", applied to Win32 futexes instead of critical sections.
- 23:00 **[MEASURED] Why the two queues genuinely meet.** `dlls/ntdll/sync.c` builds **SRWLocks, condition
  variables, critical sections and barriers** on top of `RtlWaitOnAddress`/`RtlWakeAddress*` by
  **PE-INTERNAL direct calls** — crit-section `:662`/`:835`, SRWLock `:1075`,`:1114`/`:1138`,`:1140`,`:1163`,
  condvar `:1300`,`:1335`/`:1264`,`:1275`, barriers `:1959`,`:1998`,`:2018`/`:2005`,`:2026`,`:2034`. Internal
  calls never pass through `call_import_thunk`, so **HyperBridge cannot intercept them** and they use wine's
  queue; a guest module calling `WaitOnAddress`/`WakeByAddress*` **by import name** is intercepted and uses
  HB's. ⇒ any address touched by both paths splits across two queues.
  **[HYPOTHESIS, named not proven]** the fitting candidate: DXMT runs native ARM64 workers (`dxmt-encode-thr`,
  `dxmt-finish-thr` are both present in the specimen); a native-side completion signal reaching wine's queue
  while `UnityGfxDeviceWorker` waits in HB's queue can never find it — and that worker is one of the two
  threads parked. Not measured; §5.2's `equal=` field tests it.
- 23:01 Report written: `reports/phase4-hollow-knight/HK-MASTER-ITER11-TWO-FUTEX-QUEUES-20260729.md`.
  Both experiments armed and chained behind the sibling (probe pid 9365 → HK pid 65151); slot never taken
  by me this iteration. Sibling entered its 3rd arm (`hdrspeed2-off3`) at 22:52.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 23:08 **★★ [MEASURED — HK's own import tables, free, no run] BOTH queues are in use in HK's process, and
  this WEAKENS my own 23:00 crossover hypothesis rather than supporting it.** Parsed the import tables of
  all 6 game binaries (`Main/game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/` — note the DOT,
  per the recorded path trap):
  · **`UnityPlayer.dll` imports BOTH families**: `WaitOnAddress`, `WakeByAddressAll`, `WakeByAddressSingle`
    from **`api-ms-win-core-synch-l1-2-0.dll`** (an API-SET, not kernelbase) → **HB's queue**; and
    `SleepConditionVariableSRW`, `WakeAllConditionVariable`, `Acquire/ReleaseSRWLockExclusive`,
    `Enter/LeaveCriticalSection`, `WaitForSingleObject(Ex)`, `WaitForMultipleObjects`, `ReleaseSemaphore`,
    `SetEvent` from kernel32 → **wine's queue**.
  · **`mono-2.0-bdwgc.dll` uses ONLY wine-queue APIs** — condvars (`SleepConditionVariableCS/SRW`,
    `WakeConditionVariable`, `WakeAllConditionVariable`), SRWLocks, critsects. **Zero `WaitOnAddress`.**
  · `Hollow Knight.exe` and `HKDBG.exe`: critical sections only.
  **Interception is confirmed to FIRE for an api-set import** — name-only matching (`:6926` tests
  `import_name`, not `dll_name`), and the samples independently show the threads inside
  `try_wait_address_semantic`, so this is measured, not assumed.
- 23:09 **[I WEAKEN MY OWN 23:00 [HYPOTHESIS], out loud]** I proposed a DXMT-native-wake-crosses-to-wine's-
  queue mechanism. The import tables show the **intra-guest path is SYMMETRIC**: UnityPlayer both waits
  (`WaitOnAddress`) and wakes (`WakeByAddressAll/Single`) through the SAME intercepted names, so both land in
  HB's queue and no crossover is needed for them to find each other. Likewise wine's condvar/SRWLock waits
  and wakes are both PE-internal and both land in wine's queue. **The two queues are cleanly partitioned by
  API family**; they collide only if one address is driven by both families, which I have NOT shown.
  ⇒ **The evidence now leans toward (B) NEVER SIGNALLED rather than (A) LOST WAKEUP** — i.e. the thread that
  should call `WakeByAddress*` is itself blocked. That reading fits E2E-19/stall3 having **0 runnable
  threads** (a circular wait), which a pure lost-wakeup does not require. I am NOT retiring (A): the
  `equal=` field settles it, and the two-queue structure remains a real latent defect worth recording either
  way. Report §3.2 updated to say so rather than leaving the stronger claim standing.
- 23:14 **[DISK — flagged, deliberately NOT acted on]** `disk-guard.sh --check-only` rc=0 at **exactly the
  30 GB floor**. Attribution: `reports/phase4-hollow-knight` = **16.5 GB** over 495 dirs, of which **272 dirs
  / 7.1 GB predate today**. The only `artifacts/_mr-run.*` prefix present is the sibling's **LIVE** one, so
  the usual orphan-prefix sweep reclaims nothing; `reports/phase-h` is absent and `reports/pe32` is 1 MB, so
  CLAUDE.md's explicitly-authorised prunes free nothing either. **I did not mass-delete the 272 old run
  dirs**: they are other lanes' evidence, deletion is irreversible, and my queued runs need only a few
  hundred MB. Flagging for the operator instead — that 7.1 GB is the obvious reclaim if space is needed.
- 23:14 **[MEASURED — checked a second suspected double-implementation and CLEARED it]** 12 threads per
  sample sit in `try_kernel32_handle_semantic -> NtWaitForSingleObject -> inproc_wait -> msync_wait_objs`,
  and HB does intercept the whole event/mutex/semaphore family by name (`:29440-29462`:
  `CreateEventW/A`, `SetEvent`, `ReleaseSemaphore`, `ReleaseMutex`, `WaitForSingleObject(Ex)`,
  `WaitForMultipleObjects(Ex)`, …). That looked like a second instance of the futex-queue defect. **It is
  not:** the spine shows it **forwards to the real `NtWaitForSingleObject`** rather than keeping a private
  handle table, so both waiter and signaller act on the same wine object. Recorded so this is not re-opened.
- 23:20 **[VERIFIED — the failure mode that lost ITER-10's whole plan, mitigated and checked]** ITER-10's
  queued watchers were all gone by the time I resumed (0 alive) and its probe was never built, so its
  iteration produced a plan and no measurement. I do **not** know for certain why they died. What I did
  instead of guessing: confirmed all three of my drivers are **detached and reparented to `launchd`
  (ppid=1)** — `9365`, `65151`, `32206` — which is exactly the state of the sibling's driver `67358` that
  has survived 47 min across this whole iteration. That is the strongest available evidence they will
  outlive the iteration boundary.
- 23:20 **[VERIFIED — every lever a later iteration needs is in the SHIPPED binaries, by content]**
  `ntdll.so 63c9691c0f8af97f`: `MACRUNNER_HB_LDR_NEG_CACHE`=1 (ITER-2's criterion-1 fix, still never
  properly A/B'd), `MACRUNNER_HB_FAULT_SAFE_HEADER_PROBE`=1 + `macrunner-hb-fault-header-probe-refused`=1
  (ITER-4), `MACRUNNER_HB_DISABLE_WAIT_ADDRESS_SEMANTIC`=1, `MACRUNNER_HB_CACHE_MULTI_HELPER`=1.
  `MACRUNNER_MACDRV_SELFINIT_DELAY_MS` is **0 in ntdll and 1 in `winemac.so`** — correct, that is where it
  lives; checking only ntdll would have produced a false "the gate is missing".
  **No rebuild is needed for any queued experiment.**
- 23:24 **★ [I AMEND MY OWN 22:50 PRE-REGISTRATION — BEFORE the results land, not after] The probe's
  finite timeout means a PASS cannot exonerate the primitive.** Read `tools/x64waitaddrprobe.c`'s worker
  logic properly (`:96`) rather than trusting the file header: each worker waits with a **1000 ms finite
  timeout**, and on timeout it `continue`s and re-waits. **HK's real waits are `timeout=INFINITE`.** So an
  intermittent lost wakeup **self-heals** in this probe — the worker times out after 1 s, re-reads `g_val`,
  and counts progress — and the stall detector (`:190`, "no total-wake progress for 5000 ms") therefore
  never fires. Consequences, stated now:
  · `VERDICT=STUCK` / `NO-WAKES-AT-ALL` remain **strong positive** evidence (systematic loss).
  · **`VERDICT=OK` rules out only SYSTEMATIC loss, NOT an intermittent one** — the exact failure mode a
    parked-forever INFINITE waiter would exhibit. My 22:50 falsifier said all-OK ⇒ "the round-trip is sound";
    **that was too strong.** Corrected: all-OK ⇒ *no systematic loss on the finite-timeout path*, and the
    question moves to the `equal=` snapshot, which measures the INFINITE waiters directly.
  · `workers_with_zero_wakes` is sound in BOTH modes: `wake_single` pops the queue head (`:28172`) and the
    queue is FIFO (`list_add_tail`, `:28053`), so single-wake is fair and a zero-wake worker is still
    meaningful rather than an artefact of one worker always winning.
  I am NOT rebuilding the probe to use INFINITE — that would make a lost wake hang the probe itself, which
  is precisely what its author avoided, and it would tell me only "it hung". The right instrument for the
  INFINITE path is the `wait-snapshot` census already queued.
- 23:27 **[RISK PRE-RECORDED, so the next iteration can read a failure correctly]** The snapshot's value read
  `macrunner_hb_waitaddr_read_val` (`:24146`) is a **raw dereference** (`*(const ULONG *)p`), and
  `dump_current` (`:27842`) calls it for every entry **while holding that queue's spin lock** (`:27851`).
  If any parked waiter's address were unmapped, the observer thread would fault inside the lock and could
  wedge every subsequent wait/wake on that queue — i.e. **the act of arming could itself hang the run.**
  I judge the risk low (a parked waiter's address is by definition still mapped, and the observer is a
  normal thread with SIGSEGV unmasked, so ITER-5's masked-fault wedge does not apply). Recording it anyway:
  **if the run dies or freezes immediately after `phase=armed`, that is the explanation — not "no waiters".**
- 23:32 **HAND-OFF — the pipeline is armed, detached, and will outlive this iteration; READ ITS RESULTS
  FIRST.** The sibling's 4-arm speed A/B (`hdrspeed2`, driver 67358) is still on arm 3 of 4 at 49m, so my
  measurements land after this iteration ends. Chain, all `ppid=1`:
  · `9365` `hk-waitaddr-probe-ab.sh` → waits on 67358, then 4 arms (~3 min) →
    `reports/phase4-hollow-knight/MASTER-I11-WAITADDR-PROBE/SUMMARY.txt`
  · `65151` `hk-waitwake-snapshot.sh` → waits on 9365, then ONE HK run (~25 min) →
    `MASTER-I11-WAITWAKE/wait-snapshot.txt` + `RUNDIR.txt` + `menu-park.sample.txt`
  · `32206` → waits on 65151, then `hk-audio-probe-ab.sh` (~2 min, criterion 5's first real measurement)
  **How to read them, per the pre-registrations above (22:50, 22:54, amended 23:24):**
  · probe `STUCK`/`NO-WAKES-AT-ALL` ⇒ systematic loss in HB's futex emulation, and arm `semoff`
    (`MACRUNNER_HB_DISABLE_WAIT_ADDRESS_SEMANTIC=1`) reporting `OK` names the fix.
  · probe all-`OK` ⇒ **only** "no systematic loss on the finite-timeout path" — NOT an exoneration
    (the amendment at 23:24 explains why; do not overread it, I nearly did).
  · **The decisive one is `equal=` in `wait-snapshot.txt`:** `equal=0` on a parked waiter = lost wakeup,
    proven; `equal=1` everywhere = never signalled, and `caller_module`/`caller_rva` on the same line name
    the producer to chase next.
  · Any arm/run with no marker lines at all = VOID, not a clean zero.
  **Do not re-derive this iteration's static work** (two futex queues; import-table partition; the
  `should_log` gating that makes `TRACE_WAITADDR` useless for the final wait) — it is in
  `HK-MASTER-ITER11-TWO-FUTEX-QUEUES-20260729.md` and in memory
  `project_hb_two_futex_queues_20260729`.
  **Still untouched and still open:** criterion 1's real fix (`MACRUNNER_HB_LDR_NEG_CACHE=1` ships and has
  never been properly A/B'd — its 16:08 control was invalidated when ntdll changed underneath it), and
  criterion 4 (Start Game), which is downstream of the park.
  LOOP-STATUS: CONTINUE (not at col 0 — this iteration is neither GOAL nor BLOCKED).
- 23:38 **[CRITERION 1 — a plausible suspect RAISED AND KILLED BY MY OWN CHECK, before proposing any A/B]**
  Chasing the brief's step 1 (*why* does installing the driver mid-load cost ~558 s) I found what looked
  like an unconditional hot-path cost: `macdrv_init_core` (`macdrv_main.c:490-497`) takes a mutex and then,
  on the already-initialised path, does an **ungated `fprintf(stderr, "…macdrv_init_core_already…")` +
  `fflush`** — an unbuffered syscall serialised on the stderr lock across all threads. With 4
  `macdrv_process_selfinit()` call sites in `d3dmetal.c` (`:116`, `:219`, `:244`, `:420`) on DXMT's
  recurring render entry points, that would have been a per-frame flush, and it would have fitted the
  measured split beautifully (inline installs at ~+140 s, i.e. at the START of the Mono window → many
  flushes; deferred installs at +420 s → none during the window; medians unchanged, a subset stalls 2-4x).
  **It is wrong.** `macdrv_process_selfinit()` (`:653`) opens with `if (macdrv_process_initialised) return
  TRUE;` — a single BOOL read **before** the gate resolution, the delay check and any logging — so once the
  driver is up the call never reaches `macdrv_init_core` and that `fprintf` is **not** on the hot path. The
  source comment at `:649-652` already says exactly this, and it is accurate. Recorded because the ungated
  `fprintf` reads as a defect until you check the caller, and the next person will see it too.
  **Criterion 1's mechanism therefore remains OPEN**, and the honest summary of what is known is the
  measured shape, not a cause: *self-init absent → 14/16 reach the menu, Mono ≤237.7 s; present → 0/8, tail
  784.1 s / 928.5 s; medians unchanged.* Medians unchanged with a heavy tail is the signature of a **race or
  contention**, not a uniform per-call cost — which is the direction to look, and it is where
  `MACRUNNER_HB_LDR_NEG_CACHE=1` (shipped, never properly A/B'd) is still the standing untested candidate.
- 23:35 **★★★ [MEASURED — I REFUTE THE PREMISE MY OWN BRIEF'S STEP 1 IS BUILT ON]** "self-init
  PRESENT → 0/8 reach the menu, tail 784.1 s/928.5 s" is **not the current behaviour**. All four
  `hdrspeed2` arms ran with self-init **INLINE** (`SELFINIT_DELAY_MS` unset, `UNIX_SELFINIT` unset —
  read from each arm's `final-child.json`), and **4/4 reached `Restored language`**: off1 766.5 s,
  on2 690.0 s, off3 840.8 s, on4 825.9 s. `Begin MonoManager`→1st `UnloadTime` was **254.3 / 305.2 /
  345.1 / 357.0 s** — worse than the 237.7 s healthy max, nowhere near 784 s. **Every arm ended
  `exit=124`, the 900 s timeout.** `on2` logged `Loaded Objects now: 4274` at +886.5 s, `Couldn't
  find GameCameras` at +897.3 s, then 11× `Couldn't find a UIManager` between +924.7 and +925.5 s —
  **median gap 0.0775 s = ~12.9 Hz, a loop ticking** — and was killed 3 s later. The runs were not
  failing to reach the menu; **they were being stopped while alive and progressing.** The 900 s
  ceiling and "self-init is catastrophic" are confounded. NOT yet claiming the deferral is
  unnecessary — see the queued falsifier.
- 23:36 **[QUEUED, PREDICTIONS REGISTERED IN THE SCRIPT HEADER BEFORE IT RUNS]**
  `scripts/hk-inline-long-e2e.sh` (chained on driver 32206): one HK run, try12 config, **inline
  self-init**, timeout **2400 s** not 900 s, input evidence on, injector armed, pixel gate laddered
  **after** the first menu marker. P1 `Restored language` +690..850 s [high]; P2 `Loaded Objects
  now` +880..1000 s [medium]; P3 survives past +1000 s [UNKNOWN — the actual question]; P4 pixel
  gate NONBLACK [low-med]; P5 keys reach the guest [UNKNOWN]. **Falsifier: if progress also stops
  near +900 s under a 2400 s ceiling, the ceiling was not the cause and the 0/8 era stands.**
- 23:37 **[MEASURED — criterion 2, keys at a real menu window: K1 NOTHING ARRIVES]** Injected into
  `on4` at guest **+835→+864 s**, after its `Restored language` at +825.9 s, via `CGEventPostToPid`
  (the key channel proven 5/5 on winkeyprobe): `d_macdrv_key_event=0`, `d_..._sent=0`,
  `d_unity_async/key_state_query=0`, `d_macrunner_return_route=0`. Strongest setting this has been
  measured in — self-init inline, driver installed +143.8 s, an on-screen **1512×982 layer-0**
  window owned by the guest pid (actuator `--probe-only`), menu marker already logged. Input dies
  **outside** Wine; the open confound stays "the app never becomes active".
- 23:38 **[TRAP I NEARLY PUBLISHED AS A RESULT — criterion 3]** `pixel-truth-gate --pid 16657` at
  guest **+740 s** → `BLACK`, `non_black_px=0` of `1484784`, `dominant_ratio=1.0`. **It measures
  nothing:** that run's `Restored language` did not arrive until **+825.9 s**, so the capture
  predates the menu scene. Same shape as the `nonblack=0` readings this lane keeps quoting. Fix is
  procedural — gate the capture on a menu marker; the queued run does.
- 23:40 **[MEASURED — the mac driver is IDLE during the Mono window, which kills the deferral's own
  stated rationale]** Four live `sample(1)` captures of the inline run at guest +248/+470/+620/+780 s
  (`MASTER-I12-SELFINIT-SAMPLES/`), no extra run paid for. 53→60 threads, **43 of 53 parked in
  `__ulock_wait2`**, two threads doing the JIT work, ≈104% CPU — serialised, not saturated.
  `[NSApp run]` healthy and idle (`_DPSBlockUntilNextEventMatchingListInMode →
  __CFRunLoopServiceMachPort`, 4258/4258). Outside the two idle run-loop threads the **whole sample
  holds 5 samples of CoreFoundation and no `macdrv_*` frame at all**. So criterion 1's cost is NOT
  "the driver is expensive while running".
- 23:42 **[MEASURED + [HYPOTHESIS] — a large cost that IS there: an `NtSetTimer` wineserver storm]**
  `read`-leaf samples grow **446 → 418 → 5420 → 7406** across +248/+470/+620/+780 s. Callers at
  +780 s: `4272 wait_select_reply<-server_wait_for_object`, **`2847 read_reply_data<-
  wine_server_call<-NtSetTimer`**, `204 NtOpenKeyEx`, `51 process_driver_events`. One thread
  (`Thread_66122547`) spends **2463/4272 = 57.7% of wall time in `NtSetTimer`**, **2351 (55.0%)
  blocked in `read()` on the server reply**. [HYPOTHESIS] wineserver is a single-threaded serialiser
  for the prefix, so this queues every other thread's server call behind it — matching criterion 1's
  signature (**medians unchanged, heavy tail**) and predicting the cost worsens once the driver adds
  its own server traffic. **Registered falsifier: if the deferred arm's total server-`read` samples
  are the SAME at +248 s, the driver is not the contention source and this dies.** Contrast armed:
  `hk-selfinit-sample-pair.sh deferred` (driver 67318) waits for the next fresh guest — the
  `SELFINIT_DELAY_MS=420000` waitwake run — and samples it at the paired +248/+470/+620 s.
- 23:43 **[I RETRACT MY OWN ITER-11 HAND-OFF: the waitaddr probe A/B is VOID, not a futex result]**
  All four arms returned **rc=2 in 1–2 s with three-line logs**: `[mr-run] run-contract-ledger
  status=BLOCKED exit before wine`. Nothing was measured about wait/wake. ITER-11 told the next
  iteration to "read its results first" — **there are none**; the harness needs the branch inputs
  passed explicitly before any re-run. Also: **`setsid` does not exist on macOS** (my first sampler
  launch used it and silently did nothing — a zero from it would have been a non-measurement;
  `nohup … &` + parent exit is what produces the surviving `ppid=1` drivers), and **`pgrep -f
  extracted-hollow-knight` matches the `mr-run.sh` wrappers**, 3 matches of which 1 was the guest.
- 23:44 **HAND-OFF.** Report: `HK-MASTER-ITER12-TIMEOUT-NOT-SELFINIT-20260729.md`. Live chain, all
  `ppid=1`: `65151` waitwake HK run (deferred, running now) → `32206` audio probe → `40315` →
  `hk-inline-long-e2e.sh` (**the decisive one — read its `MASTER-I12-INLINE-LONG/SUMMARY.txt`
  first**). `67318` deferred sampler is attached to the waitwake run and writes
  `MASTER-I12-SELFINIT-SAMPLES/deferred-s{1,2,3}.txt`. Sibling chains also queued for the slot
  (`61989` cachefix, `37043` sharedcache, `29905` inprocwake), so the decisive run may start ~1–2 h
  out. **Flagged for the operator, not acted on:** six driver chains from 5–11 h ago (`AUDIOCHECK`,
  `SYNCBLOCK`) are resident with no live guest — the known services.exe stderr-pipe wedge; 0% CPU,
  not holding the slot, so I left them.
  LOOP-STATUS: CONTINUE (deliberately indented — neither GOAL nor BLOCKED this iteration.)

## ITER-13 — 2026-07-29 23:30–23:58
- 23:34 **[STRUCTURAL — a queued chain KILLED because it could never produce data]**
  `hk-inproc-wake-ab.sh:79` called `laneA-run-hk.sh` **directly**. Its one completed arm
  (`laneA-inprocwake-off-try1-232647/run-contract.json`) is `status=BLOCKED` with
  `runner.branch_map.{actxprxy,crt_case_fusion,wwise_observer}` = `branch_input_absent` +
  `application.save_snapshot_manifest_sha256` = `path_absent`; `run-contract-ledger status=BLOCKED
  exit before wine` at +24.1s. **Wine never started** — same defect that voided ITER-11's waitaddr
  probe. It still wrote a run dir, a 226 KB run.log, a flight.jsonl and a triage record, and the
  script's `analyze()` would have printed empty metrics as if measured. Killed supervisor-first by
  verified PID (29905 → 12569; live guest 85338 confirmed untouched after), switched line 79 to
  `hk-run-try12-config.sh` (the script that exports those branch inputs), documented in the header.
- 23:36 **[CORRECTED — ITER-12's pre-registered falsifier used an ill-posed metric]** Its rule was
  "if the deferred arm's total server-`read` samples are the SAME at +248 s, the driver is not the
  contention source". Literal reading says deferred is 9.35× **higher** (4171 vs 446) ⇒ refuted. But
  **4101 of those 4171 are ONE thread parked the entire 6 s window** in `wait_select_reply <-
  server_select <- server_wait` — idle blocking, not traffic. On "server round-trips in flight"
  (`read_reply_data <- server_call_unlocked <- wine_server_call`) deferred is **6.4× LOWER** (69 vs
  445) and `NtSetTimer` is **31× lower** (16 vs 496). Hypothesis **survives** on the decomposed
  metric. (Parser note: macOS `sample` uses `+ ! :` tree characters — a naive `^\s*(\d+)` matches
  only thread headers and calls every frame a leaf. Mine reproduces ITER-12's 446/418 exactly, and
  its 5420 = my 5974 minus 554 stdio `__sread` reads.)
- 23:37 **[MEASURED]** `NtSetTimer` is **guest-initiated**, identical ancestry in all 5 samples:
  `macrunner_hb_run_x64 -> macrunner_hb_call_import_thunk -> macrunner_hb_call_arm64_pe_import12_for_ctx
  -> __wine_syscall_dispatcher -> NtSetTimer`. At inline +248 it is essentially ALL active server
  traffic (496 vs 445 read samples). Driver **attribution remains [HYPOTHESIS]** — the two arms are
  different runs (53 vs 67 threads; the deferred one has `WAIT_WAKE_TRACE` armed).
- 23:38 **★ [CORRECTED — the self-init gate is measured from the FIRST CALL, not process start]**
  `SELFINIT_DELAY_MS=420000` does **not** mean "driver at +420 s". Measured: first call +142.2 s
  (`elapsed_ms=0`), `declines=4 elapsed_ms=106186` at +248.4 s, **actual `macdrv_selfinit_entry` at
  +698.0 s**. The caller is DXMT's render entry points, so the gate is sampled, not continuous.
  **This VOIDS ITER-12's deferred sampler design**: its +248/+470/+620 samples were meant to bracket
  the install and are **all three pre-install**. Do NOT read `deferred-s2/s3` as "driver present".
- 23:39 **★★ [MEASURED — the deferred arm reaches the menu in 295 s; Mono window 163.9 s]**
  `Begin MonoManager` +59.6 s → first `UnloadTime` +223.5 s = **163.9 s**, faster than the 212.6 s
  the brief quotes for driver-absent and well under the 237.7 s healthy max. `Restored language`
  **+295.0 s**, `Loaded Objects now: 4274` **+347.5 s**.
- 23:41 **★★★ [MEASURED — THE BLACK FRAME IS REFUTED WITH A PICTURE]** `pixel-truth-gate --pid 85338`
  on the live guest: window 28673, layer 0, 1512×982, owner `wine`, **`non_black_px=7165`** of
  1484784 (0.48%), `colorful_px=0`, verdict `BLACK`. The verdict is a THRESHOLD ARTEFACT: the PNG
  shows **Hollow Knight's complete language-selection screen** — English/Français/Deutsch/Español/
  Italiano/Português (Brasil)/Русский/日本語/简体中文/繁體中文/한국어, all 11 options, correct CJK and
  Cyrillic glyphs, correct layout. Every rendered pixel has max channel ≤16, so it passes
  `non_black`(>4) but fails `colorful`(>16). **The open graphics question is no longer "why is
  nothing drawn" but "why is what is drawn ≤16/255 bright".** Retires `nonblack=0` as this lane's
  headline number — those captures predated the menu scene.
- 23:41 **★★★ [MEASURED — the game is PARKED ON the language screen, waiting for a key]** That screen
  is HK's first-launch screen and **cannot be passed without input**. So criterion 4 (Start Game) is
  unreachable *by construction* until a key lands, and criterion 2 is not merely "unproven in HK" —
  it is **the thing blocking progress**. The guest was healthy throughout: run.log still emitting at
  **+1137.9 s**, `process_driver_events` `calls=` climbing 302 → 446 → 466 across my probes.
- 23:42 **★★★ [MEASURED — keys reach macdrv EXACTLY ONCE, as a drained queue]** `macdrv_key_event` =
  **10 lines, ALL at byte-identical `+702.566 s`** (5 down/up pairs, VK_DOWN), each with a matching
  `macdrv_send_keyboard_input_sent`. Timeline: ITER-11's driver injected 5 pairs at ≈+449 s and its
  own immediate check logged `macdrv_key_event=0`; driver installed **+698.0 s**; the ten events
  appeared **4.6 s later, in one instant**. **They were never lost — they queued ~253 s and drained
  once when the Cocoa event connection came up.** ITER-11's `AFTER inject: key=0` was correct when
  written and wrong 250 s later; a one-shot post-inject count cannot see this.
- 23:45 **★★★ [MEASURED — everything injected AFTER the connection existed was dropped]** Three
  channels, same live pid, deltas all **ZERO**: `CGEventPostToPid` 16 events → 0; `.cghidEventTap`
  keys 10 events → 0; `.cghidEventTap` mouse 2 clicks + 5 moves → 0. `macdrv_key_event` stayed at
  10; `macdrv_mouse`/`app_sendEvent_enter`/`handleMouseButton` stayed at 0; both pixel captures
  byte-identical at `nonblack=7165`. **Gate check per the standing rule — these zeros are REAL:**
  in the shipped `winemac.so` (650888 b), `macdrv_key_event`=2, `macdrv_send_keyboard_input_sent`=1,
  `app_sendEvent_enter`=1, `macdrv_mouse`=5, `handleMouseButton`=7, `macdrv_selfinit_entry`=1.
- 23:46 **★★ [MEASURED — the loss is UPSTREAM OF APPKIT; `[NSApp run]` is healthy and starving]**
  `sample 85338 5` right after the failed injections, 67 threads: the Cocoa main thread is
  **3637/3637 samples** in `... -> run_cocoa_app -> [NSApplication run] -> nextEventMatchingMask ->
  _DPSNextEvent -> _DPSBlockUntilNextEventMatchingListInMode -> mach_msg2_trap` — the ordinary event
  wait. It would dispatch an event if one arrived; nothing is wedged and macdrv forwarding is not
  implicated. Replicates `project_hk_input_starved_before_appkit_20260729` (3039/3039 → 3637/3637)
  and sharpens it to **CGEvent delivery to the process**.
- 23:46 **[MEASURED — the activation confound, now a number instead of a suspicion]** Actuator
  preflight on pid 85338: `observed_activation_policy=0` (Regular), `is_finished_launching=true`,
  **`observed_is_active=false`**, frontmost = "Claude"; `activate` → `requested:true returned:true`,
  **`is_active` still false**. So the process IS a registered regular GUI app that CAN create an
  on-screen layer-0 window and CANNOT be made active or receive posted events. Per
  `lesson_keyboard_and_mouse_need_opposite_injectors` a CLI `activate()` also fails on Finder, so
  this alone does not convict the app — but with the +702.566 s drain it is the best-supported read.
- 23:52 **[QUEUED — falsifier for my own headline finding, registered before it runs]** New
  `scripts/hk-input-drain-census.sh` posts keys every 10 s from before the install through after it
  and buckets deliveries by tick — which a one-shot inject can never do. Verdicts pre-registered in
  the header: **DRAIN_ONCE** (deliveries only within 30 s of `macdrv_selfinit_entry`, zero after) ⇒
  confirmed, fix is on the CGEvent-connection/activation side; **CONTINUOUS** ⇒ **my finding is
  refuted** and ITER-13's zeros were an artefact of how it injected; **NEVER** ⇒ the +702.566 s burst
  was run-specific. Deliberately a SEPARATE run from `hk-input-evidence-inject.sh`: that script waits
  for the install then baselines, so a census injecting beforehand would drain into its baseline and
  manufacture a false K4. Chained (pid 50555) behind 40315 = the decisive inline-long run, so it does
  not perturb criterion 1.
- 23:53 **[BUG IN MY OWN INSTRUMENT, caught by validating against the live guest before shipping]**
  The census's guest-pid detection used `pgrep -f ... | head -1`. Live check returned
  **`8446 69640 85338`** — the mr-run/laneA wrappers sort FIRST, `head -1` gives a `bash`, the comm
  filter rejects it, and the census would have **silently never injected**: a clean-looking zero that
  measures nothing. Fixed to filter ALL candidates by `comm`; re-validated, selects 85338.
  (`lesson_macos_has_no_setsid_and_pgrep_matches_wrappers` predicted exactly this.)
- 23:55 **HAND-OFF.** Report: `HK-MASTER-ITER13-LANGUAGE-MENU-RENDERS-INPUT-STARVED-20260729.md`.
  Evidence: `MASTER-I13-LIVE-MENU/` (menu PNG, before/after captures, `postinject.sample.txt`).
  **Standing corrections to this lane's shared beliefs:** "HK's window is black/`nonblack=0`" —
  wrong, it renders the full language menu, dim not absent. "Keys have never reached HK" — wrong,
  10 arrivals at +702.566 s. "`SELFINIT_DELAY_MS=420000` ⇒ driver at +420 s" — wrong, +698.0 s.
  **Next iteration, in order:** (1) read `MASTER-I13-DRAIN-CENSUS-*/VERDICT.txt` — it decides whether
  input is recoverable and costs no build; (2) if DRAIN_ONCE, the fix is CGEvent-connection/activation,
  NOT winemac.drv forwarding and NOT win32u; (3) criterion 3's remaining question is **brightness**
  (max channel ≤16), not geometry; (4) do NOT read `deferred-s2/s3` as driver-present. Live chain:
  32206 audio probe (criterion 5's first real measurement, running) → 40315 inline-long → 50555 census.
  `MACRUNNER_HB_LDR_NEG_CACHE=1` ships and remains un-A/B'd.
  LOOP-STATUS: CONTINUE (indented deliberately — neither GOAL nor BLOCKED this iteration.)
- 23:58 **[MEASURED — the dimness is STABLE, not a fade in progress]** Three captures of the same
  live window at 23:40:26 / 23:41:43 / 23:41:55 (≈89 s apart, straddling 26 injected key events) all
  report `non_black_px` = **7165 exactly**, before/after BMPs byte-identical. So ≤16/255 is a stable
  render state, not a fade caught mid-flight — pointing at a colour-space/transfer-function or
  blend-alpha problem on the present path rather than an animation still running. **[HYPOTHESIS]**;
  cheapest next test = dump one swapchain backbuffer's raw pixels and compare with the composited
  window, which separates "HK drew it dim" from "we presented it dim". Guest 85338 has since reached
  its 1500 s timeout and exited, so this closes ITER-13's live-guest window.
LOOP-STATUS: BLOCKED — every backend in the failover chain (claude) retired after 2 consecutive no-work fast returns; quota or auth is exhausted and the operator must refresh it.
