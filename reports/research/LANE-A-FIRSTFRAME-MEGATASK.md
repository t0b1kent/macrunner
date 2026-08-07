# Lane A MEGA-TASK — drive Hollow Knight to the FIRST VISIBLE FRAME (autonomous)

## Mission
Get **Hollow Knight** (x64 Unity 6000.0.61f1 under ARM64EC/HyperBridge + ARM64 Wine + DXMT→Metal) to render its
**first visible frame on screen**. Run AUTONOMOUSLY wall-by-wall until a frame renders (`LOOP-STATUS: GOAL`) or a
hard blocker (`LOOP-STATUS: BLOCKED`). A **silent** frame counts — sound is a follow-up, NOT the goal.

## Where HK is now (this session's arc — all committed to main)
- **Graphics DONE:** DXMT Metal device created (`Direct3D 11.0` / `Renderer: Apple M1 Pro`). Commit `cf52a9a`.
- **JIT/engine fixes committed:** CMPPS `c76033c`, LDAR unaligned-load `19431a1`, CVT converts `287bb5e`,
  getenv-cache FIX#1 `c021d33`, block-cache grow+de-stick FIX#2a `ca56289`.
- HK reaches **Mono managed-runtime init** (`Begin MonoManager ReloadAssembly`) then LIVELOCKS at "rank 7".

## Current wall (rank-7) — software-audio spin (root pinned by heartbeat probe)
The livelock is a **software audio mixer**: FMOD fails to init any audio device → falls to an UNTHROTTLED
software PCM mixer (float→int16 saturating loop @ `UnityPlayer 0x17b2bb0`, ~60% of heartbeats) that monopolizes
a core under emulation → main thread never advances (log frozen ~219KB). REFUTED as root: x18 (0%),
getenv/environ-lock (FIX#1, real but minor), WaitOnAddress (healthy, c0000005=0), JIT-cache (FIX#2a, symptom).
`mmdevapi.dll` is present in the guest but device ENUMERATION returns nothing → FMOD finds no device.

## STEP 1 — unblock the audio-spin by the FASTEST correct means (goal = FRAME, not sound)
Pick whichever gets HK past rank-7 fastest; VERIFY via heartbeat / new Unity markers / log growth past ~219KB:
- **(fastest) throttle/yield the no-device software mixer** so it doesn't busy-spin a core when no device exists
  (sleep/yield/rate-cap on the no-device path), OR stub Unity audio (boot.config / a null *paced* device) so the
  mixer can't run hot. This likely gets a VISIBLE frame fastest.
- **OR** fix `mmdevapi → winecoreaudio` device ENUMERATION so FMOD finds a paced device. LEVERAGE existing infra
  (do NOT rebuild): `engine/wine/dlls/winecoreaudio.drv/` (+ `winecoreaudio_avaudio.drv`), `engine/wine/dlls/
  mmdevapi/`, `reports/research/AUDIO-FMOD-READINESS-AUDIT.md`. The break is enumeration in the ARM64EC +
  x64-guest config (x64-guest mmdevapi → native winecoreaudio bridge returns no device) — same guest↔native
  bridge class as the DXMT routing fixes. Pin it empirically (which call returns empty/E_NOTFOUND) before fixing.

### STEP 1 RESOLVED (operator decision 2026-06-17): ONE terminal does it all — NO Gemini/Audio-lane handoff.
Probes done: MMDevice/WASAPI **enum PASS**, **DSound full render PASS** (checksum 0x27645FE8), XAudio2 FAIL. So
the audio bridge largely works; the break is **narrow = the WASAPI `IAudioClient::Initialize/GetService/Start`
path** (FMOD's default backend), the one path with no x64 fixture.
- **(A) pin it first (cheap, do this):** run HK once with `WINEDEBUG=+mmdevapi,+winecoreaudio` → identify the
  exact failing IAudioClient call in FMOD's WASAPI init.
- **(C) then fix it yourself** in `engine/wine/dlls/winecoreaudio.drv/` (+ `mmdevapi`) — overlay build, golden
  dist UNTOUCHED. "Leverage existing / don't rebuild" meant don't rebuild the whole audio stack from scratch —
  fixing the narrow WASAPI-init path in our own overlay-buildable files IS in scope. No handoff.
- **Do NOT rabbit-hole on audio:** the FRAME is the goal. If the WASAPI fix turns out deep/slow, fall back to
  the fastest unblock — **throttle/yield the no-device software mixer** (so it stops monopolizing the core) for a
  **silent frame** — and push on to present. Real WASAPI audio can be a follow-up; it is NOT the frame-blocker.

### STEP 1 UPDATE-2 (2026-06-17): the WASAPI break is DEEP (COM-apartment), not winecoreaudio. Frame ≠ via COM fix.
Trace pinned it ABOVE mmdevapi: `err:dsound:get_mmdevenum CoCreateInstance failed 800401f0 CO_E_NOTINITIALIZED`
— and `get_mmdevenum` (dsound_main.c:154) ALREADY calls `CoInitialize(NULL)` (:158) before CoCreateInstance
(:160). So CoInitialize is INEFFECTIVE: the COM apartment isn't established for the x64-guest audio thread under
ARM64EC → no MMDeviceEnumerator → FMOD no-device → spin. Root = **combase/ole32 apartment + TEB-coherence (the
x18/host-TEB family)** — NOT the scoped winecoreaudio/mmdevapi fix.
- **#1 (do first — cheapest, AND a candidate fix in one run):** overlay `dsound.dll` build — in `get_mmdevenum`
  log `init_hr` (the CoInitialize(NULL) return), then on CO_E_NOTINITIALIZED retry `CoInitializeEx(NULL,
  COINIT_MULTITHREADED)` + re-attempt CoCreateInstance; fast `-all` re-run.
  - MTA retry SUCCEEDS → device found → no spin → boot proceeds (real audio bonus) → ride to present. WIN.
  - CoInitialize S_OK but still NOTINITIALIZED → apartment lost = TEB-DEEP. **Do NOT fix COM/TEB for the frame.**
- **#2 (only if TEB-deep): throttle/fail-fast for a SILENT frame (sidestep COM entirely).** First decide: is the
  MAIN/boot thread CPU-STARVED by the spinner, or PARKED waiting on an audio-ready signal? Probe whether
  `find_region_normalized` (29% of running samples) takes a GLOBAL region-table lock the spinner hammers →
  serializing all threads → starving main.
  - CPU-starved → throttle/yield the no-device mixer (or fix the region-lock contention) → main runs → silent
    frame → present → fold laneD winemac present.
  - Parked-on-audio-signal → make FMOD's no-device path fail-FAST/clean (no unthrottled software loop) → boot
    proceeds silent.
- **Document the COM-apartment-under-ARM64EC deep issue (x18/TEB family) as the real-audio FOLLOW-UP** in memory +
  PROGRESS. It is NOT the frame-blocker once #1-fix or #2-throttle lands. One terminal, no handoff.

### STEP 1 UPDATE-3 (2026-06-17): AUDIO IS LIKELY A RED HERRING — stop chasing it; profile the BLOCKED thread.
#1 result was decisive: STA `init_hr=S_FALSE` (COM already init), MTA-retry `CoInitializeEx=S_OK` → so COM init
WORKS, **NOT TEB-deep**; MTA got further but `CoCreateInstance=REGDB_E_CLASSNOTREG` (a subtle combase
class-resolution issue, not quick). #2 throttle is MOOT: `find_region_normalized` (hb_memory.c:1043) is LOCK-FREE
(per-context MRU + linear scan, no global lock) → the spinner does NOT serialize other threads → throttling it
can't wake a parked main. And FMOD init COMPLETED ("software output") → main probably got PAST audio init; the
float→int16 mixer spins on a SEPARATE, lock-free, non-blocking thread. **We've been chasing the BUSIEST thread
(audio, 60% of heartbeats); the BLOCKED thread is the MAIN/boot thread, which is PARKED (invisible to
busiest-thread sampling). Loud ≠ blocked.**
- **DECISIVE PROBE (do this, thread-aware):** identify the Mono MAIN thread (the tid that logged `Begin
  MonoManager ReloadAssembly`) and capture WHAT IT IS PARKED ON during the freeze — per-tid sample of THAT
  thread (not the busiest) + its stack + the address/event/handle it polls or waits for.
  - (a) main parked waiting-on-audio (device/COM-ready signal) → audio IS the blocker → combase CLASSNOTREG matters.
  - (b) main parked on a DIFFERENT dependency (Mono/CLR wait, a guest event/handle, a main-thread JIT/ISA gap,
    another COM/service activation) → audio is a RED HERRING → find + attack THAT wait; audio = sound-only follow-up.
- **This probe decides: tractable path forward → pursue to present/frame; OR genuinely deep/intractable wait →
  ACCEPT rank-7 as the documented FRONTIER** (a huge milestone in itself: HK boots natively — no Rosetta —
  through graphics + DXMT/Metal device + into Mono managed-runtime init). Don't keep grinding a dead wall; if the
  main-thread wait is intractable, write LOOP-STATUS: BLOCKED with the full scorecard and stop.

### STEP 1 UPDATE-4 (2026-06-17): REAL BLOCKER FOUND — module_from_pc Mach-scan throttle (NOT audio, NOT a wait).
The thread-aware probe answered (b): the Mono MAIN thread (Thread_354443) is **RUNNING, not parked** — actively
JIT-translating ReloadAssembly — but burns **~47% (1699/3591 samples) in `macrunner_hb_module_from_pc →
mach_vm_read_overwrite`**. Root: `macrunner_hb_module_from_pc` (macrunner_hb.c:671) on an LDR miss does a linear
downward page-scan up to **0x100000 (1M) pages** (:702), each via `mach_vm_read_overwrite` (Mach syscall per
page, :606). Mono's JIT-compiled C# lives in ANONYMOUS exec regions (not PE, not in the LDR) → every Mono-JIT PC
misses the LDR → triggers the giant Mach-syscall scan; the per-page neg-cache (128 entries) thrashes → nearly
every Mono PC re-scans. The boot CRAWLS (scales with C# JIT volume) — looks like a hang, isn't. Lock-free audio
mixer was the loud-but-idle red herring. **This is a Lane A PERF bug (overlay-buildable, golden untouched), and
the DIRECT path to the frame — fix it and the main thread accelerates through ReloadAssembly → PlayerLoop →
scene → swapchain.**
- **FIX (do now): 1+2.** (2) range-based neg-cache: on the downward scan, once nearest PE base b below page p is
  found, cache the whole span (b+size, p] with that result (downward-first-hit ⇒ provably correct) → a Mono
  region costs ONE scan, not per-page thrash; grow the 128-entry cache if still range-keyed. (1) cap the 1M-page
  scan — FIRST verify all real PE modules are in the LDR (:687) so the cap only truncates non-PE Mono scans.
  VERIFY the range-cache returns IDENTICAL results to the old per-page scan (hot, correctness-sensitive path).
- **Defer (3)** LDR-nearest-module iterate (syscall-free, cleaner, returns NULL for Mono) — behavior change,
  needs a caller-audit; do only if residual module_from_pc syscall cost remains after 1+2.
- Rebuild + re-run; expect module_from_pc's ~47% to collapse + boot to advance past ReloadAssembly toward
  present. Commit after the re-run verifies the speedup. This is the most promising lead yet (perf, not a wall).

### STEP 1 UPDATE-5 (2026-06-17): both throttles FIXED — rank-7 is now a THROUGHPUT FLOOR, not a discrete blocker.
Committed: `5126bcb` module_from_pc cap+range-cache (47%→0%), `3f53170` find_region O(log n) treap-walk
(33.5%→1.5%). Six engine wins this session. After peeling both, the boot is STILL at rank-7 because it's
**throughput-limited double-emulation of Mono ReloadAssembly** (HyperBridge emulating Mono JIT-compiling HK's
entire C#) — not a deadlock. Remaining: `macrunner_hb_run_x64` 43% (≈irreducible per-block dispatch floor) +
`macrunner_hb_ir_cache_put` 30.6% (last fixable; `MACRUNNER_HB_IR_CACHE_SIZE=8192` open-addressing degrades when
Mono's working set exceeds it — likely re-lift thrash, the FIX#2a pattern one layer up).
- **DO: (a)+(b) combined — grow the IR cache AND instrument it (do NOT grow blind = the FIX#2a-symptom trap).**
  Add a re-lift counter to ir_cache_put (same PCs re-lifted = THRASH vs new PCs = GENUINE one-time volume); grow
  MACRUNNER_HB_IR_CACHE_SIZE (8192 → ~65536; check load-factor/mem); long-haul run (30+ min / until ReloadAssembly
  completes). Watch ir_cache_put % drop + re-lift counter + boot advancing past ReloadAssembly → PlayerLoop/scene
  → present/CAMetalLayer.
- **THRASH + completes → FRAME path** (ride to present, fold laneD winemac present). **GENUINE (not thrash) →
  double-emulation FLOOR reached.**
- **⛔ STRATEGIC FORK — if it's the FLOOR, STOP and surface to the operator (do NOT autonomously commit the big
  architectural option):** (i) accept a SLOW first boot — let a 30-60min run finish ReloadAssembly → the frame
  renders = the first-frame milestone (proves the native stack end-to-end), document it; (ii) persistent
  cross-run JIT/IR cache (translate HK's C# once → disk → reuse → fast subsequent boots) — proper but big
  architectural work; (iii) document rank-7 + the throughput characterization as the frontier. Write
  LOOP-STATUS: BLOCKED with the fork + the cost breakdown; the operator picks. This is a natural decision point,
  not infinite grinding.

## STEP 2 — ride the remaining walls to the frame
After rank-7 clears, expect more engine init → swapchain/present. When HK reaches present/swapchain (grep
`CreateSwapChain` / `IDXGISwapChain` / `Present` / `CAMetalLayer`): **fold the laneD winemac D3DMetal present
path** to put the frame ONSCREEN. Cherry-pick from branch `laneD-graphics` (3330172) ONLY the winemac present
additions: `engine/wine/dlls/winemac.drv/d3dmetal.c` (+437, new), `cocoa_window.m` (+4233, CAMetalLayer present),
`event.c` (+621), `d3dmetal_objc.{h,m}`, + `wine-fork/patches/0001-cocoa-window-arm64-CAMetalLayer-fallback.patch`
and `0003-event-guard-CLIENT_SURFACE_PRESENTED-arm64.patch`. Do NOT take laneD's OLDER versions of shared engine
files (macrunner_hb.c / loader.c / signal_arm64.c — they'd revert this session's fixes); build-verify after fold.
Then any new JIT/ISA gaps → bulk them (decode/lift/codegen, like CMPPS/CVT).

## Discipline (what's worked all session — keep it)
- **EMPIRICAL verify-first:** pin every wall with trace / disasm / profile / heartbeat BEFORE theorizing. Do NOT
  declare "root" until a fix MOVES the boot — keep a refuted-scorecard. (This session: 4+ "roots" were each
  superseded; the scorecard is the stable record.)
- **Bulk-over-reactive** on ISA/opcode gaps (whole family in one pass).
- **Commit each real win:** NAMED files only (NEVER `git add -A`/`.`), golden wine-dist UNTOUCHED (DXMT/winemac
  overlay builds are OK), scoped `wineserver -k` only (NEVER global `pkill -9 wine`/`killall wine`), force-add
  gitignored fix files. Commit AFTER a re-run verifies the win.
- **No ultracode bursts** — operator's standing call; they've missed on this class, empirical wins.
- **Save at milestones:** update memory + the Obsidian journey doc `130-hk-dxmt-first-frame-push-20260615.md`
  (in /Users/timurtoby/Documents/MacRunner/) + snapshot/checkpoint via git plumbing (don't disrupt main HEAD).

## Done condition — write at column 0 in the PROGRESS file
- `LOOP-STATUS: GOAL` — a visible HK frame rendered (describe it; screenshot path if captured).
- `LOOP-STATUS: BLOCKED` — a hard wall; state it + the cheapest decisive next probe + the refuted-scorecard.
Otherwise keep going wall-by-wall toward the first frame. Report the furthest point + next wall each iteration.

---
## ⭐ LATEST STATE (UPDATE-6, 2026-06-17) — read this first; supersedes the audio/throughput steers above
**PAST the throughput floor. Mono ReloadAssembly COMPLETED. The remaining wall is a clean WAIT-DEADLOCK.**
- IR-cache thrash fixed (`e23140f`, MACRUNNER_HB_IR_CACHE_SIZE 8192→262144, **CPU 120%→26%**) → ReloadAssembly
  finished. 7 engine commits this session; refuted-scorecard fully closed (x18 / getenv / WaitOnAddress-prim /
  JIT-cache / audio-red-herring / module_from_pc / find_region / IR-cache-thrash — ALL fixed or refuted).
- **Current wall:** post-ReloadAssembly the whole process is PARKED (26% CPU). main/coordinator threads
  (460362, 460360) block in `WaitForSingleObject → server_wait` on a kernel object **never signaled**; workers
  idle on WaitOnAddress (27 on `0xff800001` = "no work dispatched"). A genuine missing-wake **wait-deadlock** —
  discrete + fixable, NOT the emulation floor. Likely the LAST wall before the boot proceeds.
- **DO NEXT — WaitForSingleObject-handle probe** (`macrunner_hb_try_kernel32_handle_semantic` + server_wait):
  for the blocked threads log the HANDLE + object TYPE (Event/Semaphore/Mutex/Thread/Process) + name + timeout;
  trace who should SetEvent/ReleaseSemaphore/thread-exit it. Distinguish **(a)** signal NEVER issued — signaler
  parked/never-started (e.g. main waits a worker job-done event but the job was never DISPATCHED → the idle
  `0xff800001` workers fit a job-system dispatch/wake deadlock) vs **(b)** signal issued but server_wait/wake
  doesn't DELIVER it (wineserver kernel-object missing-wake). Connect the blocked handle to the idle workers.
  Fix the missing-wake → boot proceeds toward PlayerLoop/scene → present/CAMetalLayer → fold laneD winemac
  present → **FRAME**. Report handle + type + signaler + a/b + the fix. Overlay/named only, golden untouched,
  scoped `wineserver -k`.

### UPDATE-7 (2026-06-17) — handle-probe DONE: it's a wait-LOOP LIVELOCK, not a dead wait. Next = caller-disasm.
Probe refined the wall: the group-(b) handles (0x68-0xb0,0xec) are HEALTHY cyclic idle waits (signaled=1, red
herring). The MAIN thread (caller `0x87efdc4f244` = UnityPlayer ~rva `0x173f244`) hits `0x1d0/0x1dc` via
`WaitForSingleObjectEx` **40,487× before ≈ 40,486× after = a LOOP, not a stuck block** (the prior "never-signaled
deadlock" was a misread). INFINITE timeout, alertable (APC/IO-completion-driven), NOT SetEvent-signaled. So it's
form (a) — awaited completion/job **never produced** — realized as a **poll-loop LIVELOCK** while the worker pool
is idle (`0xff800001`, "no work dispatched").
- **DO NEXT (supersedes the handle-probe above): caller-disasm pin.** Map+disasm `0x173f244`: (1) what flag/
  condition the loop polls between waits; (2) identify `0x1d0/0x1dc` (NtQueryObject type+name / their CreateEvent
  /CreateIoCompletionPort); (3) the PRODUCER that should post it + why it never runs — suspects: job-system
  dispatch never reaching the idle workers / an async-IO/IOCP completion that never fires under emulation / an
  APC never queued to the alertable wait. Report polled-condition + 0x1d0/0x1dc identity + missing-producer +
  which → the fix → boot proceeds toward present/CAMetalLayer → fold laneD winemac present → FRAME.

### UPDATE-8 (2026-06-17) — caller-disasm DONE: rank-7 = WORKER-DISPATCH/WAKE DEADLOCK, fully localized.
The loop is `while (WaitForSingleObjectEx(event 0x1d0/0x1dc, INFINITE, alertable) == WAIT_IO_COMPLETION)` at
UnityPlayer rva `0x173f244` — main APC-pumps, waiting for an Event a worker should SetEvent on job completion.
The Event is never set because the **worker is never woken**: the 27 workers parked on `0xff800001` were never
in the wake log. Chain: main enqueues job → should wake a worker → worker runs → signals the Event → main
proceeds; the BREAK = the worker-wake never fires.
- **DO NEXT (supersedes caller-disasm above): producer/dispatch pin.** (1) Instrument SetEvent → confirm
  0x1d0/0x1dc never set + NtQueryObject its name. (2) Capture the worker wait address+comparand (the 0xff800001).
  (3) On job-enqueue, does the path CALL the worker-wake (RtlWakeByAddressSingle/All / SetEvent / ReleaseSemaphore)?
  **(a)** no call = dispatch→wake step missing/broken in emulation → fix the dispatch-wake; **(b)** call made but
  doesn't reach the workers = RtlWakeByAddress/futex-match/wineserver wake not delivered → fix delivery/matching.
  Report Event identity + worker wait-addr/comparand + a/b + the exact missing/broken wake = THE FIX. Then
  workers wake → job done → Event signaled → main proceeds → boot → present → fold laneD winemac present → FRAME.
  This is the final pin before the fix.

### UPDATE-9 (2026-06-17) — producer/dispatch pin DONE: ROOT = Unity job-system worker-wake broken (likely sem↔futex mismatch).
Confirmed: Event 0x1d0/0x1dc is NEVER SetEvent'd; the 27 workers (WaitOnAddress, comparand 0xff800001) are NEVER
WakeByAddress'd; but `ReleaseSemaphore` IS called. ⇒ jobs dispatched never run → completion never produced →
main APC-pumps forever. **Strong hypothesis: PRIMITIVE MISMATCH** — dispatch wakes via a SEMAPHORE while the
workers are parked on a futex/WaitOnAddress, so our `ReleaseSemaphore` emulation never issues the WakeByAddress
that would wake them.
- **DO NEXT (supersedes producer-pin above): final disasm** — worker-park `0x87efca87c92` vs dispatch-wake
  `0x87efd9e1f30` (ReleaseSemaphore): (1) what primitive the worker blocks on (semaphore-via-futex vs raw
  WaitOnAddress) + its handle/addr; (2) which sem handle the dispatch releases; (3) SAME object? If worker parks
  on sem S (→ futex F) and dispatch ReleaseSemaphore(S) but our release doesn't WakeByAddress F → **that's the
  bug**: ReleaseSemaphore/server-semaphore-release must wake the futex-parked workers. Fix → workers wake → job
  done → Event set → main proceeds → boot → present → fold laneD winemac → FRAME. THIS is the final pin → fix.

### UPDATE-10 (2026-06-17) — final disasm DONE: TWO pools; pick which feeds Event 0x1d0/0x1dc.
Worker disasm shows two pools. **Semaphore pool** (0x577c60): park `WaitForSingleObjectEx(jobSem[this+0x58])` →
server_wait; wake `ReleaseSemaphore` (IS called). **Job.Worker pool** (27 thr): park `RtlWaitOnAddress(0xff800001)`;
wake `WakeByAddress` (NEVER called). Whichever feeds main's never-set Event 0x1d0/0x1dc is the bug.
- **(a) Job.Worker [PRIME SUSPECT]:** the enqueue's `RtlWakeByAddress` for the 0xff800001 workers is never issued
  / routed off `try_wait_address_semantic`. (Why prime: a broken CORE wineserver sem-wake would fail far more
  than HK — so the HK-specific job-dispatch WakeByAddress is the likelier gap.) Fix = targeted HB-side
  WakeByAddress/dispatch fix — **overlay, golden untouched, SAFE**.
- **(b) Semaphore [unlikely, core wine]:** wineserver NtReleaseSemaphore doesn't wake the waiter → higher-risk
  core sync change.
- **DO NEXT — wake-delivery test (diagnostic only, NOT the risky change):** for one jobSem — ever
  ReleaseSemaphore'd + does the worker wake? for Job.Worker — does enqueue ever RtlWakeByAddress a 0xff800001
  slot? → picks (a)/(b) + the exact file. **GATE: if (a) → implement the targeted overlay WakeByAddress fix →
  re-run → frame. If (b) → STOP and surface to operator (core-wine change = deliberate decision, not an
  auto-dive).** Report (a)/(b) + file.
