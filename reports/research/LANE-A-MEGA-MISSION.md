# LANE A — MEGA-MISSION (one continuous mission, not a list of small tasks)

**Operator order (2026-06-02):** This is ONE big standing mission. Do NOT do a single step,
declare it "done", and stop waiting for a new prompt. Keep going down the ladder yourself.
The mission ends ONLY at a STOP condition (see §STOP). Everything between here and a visible,
rendered Hollow Knight window is yours to drive autonomously.

Base: main `f048792` (Lane B i386 legacy opcodes + CS_MODE_32 coverage merged — includes new
`hb_interpreter.c`/`hb_decode_x86.c`; rebuild picks them up). Lane A last checkpoint `1bc8b0b`.
Lane B owns ISA/decode/i386 — DO NOT touch their files (see §LANES).

---

## THE GOAL (the only definition of "done")

A **visible, rendered Hollow Knight window** on screen via the ARM64EC + DXMT path:
GOG DRM-free `Hollow Knight.exe` (Unity/Mono, x64) → HyperBridge JIT → ARM64EC Wine 11 →
DXMT (D3D11→Metal). Success evidence = `D3D11CreateDevice` returns a real device (count > 0),
a swapchain is created, and a window with pixels appears (screenshot or
`CreateSwapChain>0 && present>0`). Anything short of that is "still in progress" — keep climbing.

---

## ★ NEXT PRIORITY (2026-06-03) — x86 ATOMICS / TSO is the confirmed root of the window blocker
Claude's diagnosis (reports/research/CLAUDE-GATE-DIAGNOSIS-20260603.md UPDATE 4) found the real
gate: the Hollow Knight init livelock at guest rva ~0x513xxx is a spin-reader that never observes a
cross-thread publisher, because guest x86 atomics are non-atomic + UNORDERED on weakly-ordered
ARM64. This is an ENGINE-WIDE multithreading bug, not HK-specific. **Take this BEFORE more gate
tracing — it's the root.**

Already landed by Claude: **fix `6c1f519`** — interpreter CMPXCHG/CMPXCHG8B/XCHG/XADD now emit
`__atomic_thread_fence(SEQ_CST)` (x86 LOCK = full barrier). JIT routes these ops to the interp
helper, so both paths get it. ISA fuzz still 0-mismatch, tests 444/2, fast_validate PASS.

YOUR next steps (in order):
1. **Rebuild + HK-test `6c1f519`:** relink ntdll.so against libhyperbridge.a, install + codesign
   dist-arm64ec-spike, run HK. Does the 0x513xxx livelock clear / init advance past ~1.74M blocks
   toward GfxDevice? Report blocks trajectory.
2. **READER side (the real fix = software TSO + no-hoist; ChatGPT research confirmed — see
   CLAUDE-GATE-DIAGNOSIS UPDATE 5). Apple HARDWARE TSO is NOT usable** (privileged EL1 reg, no public
   macOS API for third parties — Rosetta/private-entitlement/KEXT only). Do B+C:
   - **DIAGNOSE FIRST:** instrument the 0x513xxx loop per-iteration (guest PC, load address, emitted
     host op LDR/LDAR/cached-reg, value; + writer stores). One real load then spin-on-register →
     hoisting (C). Reloads LDR but stale → ordering (B).
   - **C (mandatory, FIRST):** stop the JIT hoisting/CSE/LICM of guest memory loads across loop
     backedges/calls/atomics in `hb_arm64_codegen.c` — guest loads are volatile vs other threads.
   - **B (STRICT_TSO):** guest scalar load→`LDAR`, store→`STLR` for SHARED mem (heap/globals/module/
     Mono/Boehm/escaped); plain LDR/STR only for proven-private stack/TLS. LOCK/XCHG→`DMB ISH`,
     MFENCE→`DMB ISH`, SFENCE→`DMB ISHST`, LFENCE→`DMB ISHLD`. Keep interp fix 6c1f519.
   - Validate: message-passing + spin-flag + CAS litmus tests under translated threads MUST
     terminate; ISA fuzz 0-mismatch; then HK (0x513xxx must exit → GfxDevice). Optimize later (FEX
     model): vector/memcpy TSO as separate toggles.
3. **True host-atomic RMW:** add a LOCK flag to the IR (decode→lift→interp), expose a 64-bit
   guest→host pointer, and do real `__atomic` CAS/fetch_add/exchange for LOCK-prefixed ops (prevents
   lost updates under contention; current fence-only fix orders but doesn't make RMW atomic).
4. **Real fences:** LFENCE/MFENCE/SFENCE are dropped at `hb_decode_x64.c:2370` — make MFENCE a full
   `dmb ish`, LFENCE acquire, SFENCE release (add an IR fence op or handle in interp+codegen).
5. **Validate:** a 2-thread producer/consumer spin test (writer sets flag, reader spins) MUST
   terminate; keep ISA fuzz 0-mismatch; then HK. This is the single highest-impact fix in the project
   and benefits ALL future multithreaded games.

NOTE: `hb_interpreter.c` is shared with Lane B (x87) — Claude reconciles that merge; your atomics/TSO
work should stay in `hb_arm64_codegen.c` + (if adding the IR LOCK flag) decode/lift, and coordinate
the interpreter region via Claude.

---

## THE BLOCKER LADDER (climb it end-to-end, don't stop between rungs)

You flow from each rung straight into the next. Each rung is a means, not a finish line.

1. **Mono managed-init throughput** ← current barrier.
   The thread executes (blocks ~705K+) but does not reach `D3D11CreateDevice` inside the run
   window. FIRST run the decisive long warm run (§DECISIVE-RUN) to answer the binary question:
   *does Mono init COMPLETE if given enough time, or is it parked on a gate?* Then:
   - If it COMPLETES given time → it's throughput. Attack throughput at the architectural level
     (AOT cache hit-rate, hot-block promotion, bulk codegen of the Mono init hot families,
     direct-mem/direct-stack fast paths) until init finishes well inside a normal run. Loop-warming
     is never wasted, but prefer structural wins over blind warming.
   - If it PARKS (plateaus, 0 CPU, frozen heartbeat) → it's a GATE. Go to rung 2.
2. **Gate diagnosis** (if parked). Last verdict (`HB-GRAPHICS-speed-vs-gate-verdict-20260601.md`)
   flagged UnityPlayer RVA `0xcba8b2` (zero-timeout poll, main) + workers at `0x577c92/0x577f44`
   (infinite waits). Diagnose exactly what handle/event/timer/result the poll expects, fix the
   missing wait/scheduler/thread-state semantic at the root (ntdll sync path), confirm the
   heartbeat un-freezes and progress resumes. Then flow back up — init continues.
3. **Reach `main()` / engine bring-up** — past Mono init into Unity engine startup.
4. **GfxDevice creation** — Unity selects/creates its graphics device. Get `GfxDevice>0`.
5. **`D3D11CreateDevice` returns a REAL device** — currently `0` everywhere. This is the make-or-
   break rung for the product path. Drive DXMT until the device-create succeeds (count > 0).
6. **Swapchain + window** — `CreateSwapChain>0`, window appears, first present. Pixels = GOAL.

At every rung the same loop applies: run → read evidence (bounded) → identify the single active
blocker → fix at the root (bulk where a finite set + reference exists) → rebuild green → re-run.
Don't optimize rungs you haven't reached; don't declare victory on a rung that isn't the window.

---

## DIAGNOSTIC UPDATE 2 (2026-06-02 ~21:15) — SGen-suspend hypothesis REFUTED; it's THROUGHPUT

Evidence from the post-context-fix HK run (`run-20260602-post-getcontext-head240-signed-abs`):
`GetThreadContext / NtGetContextThread = 0` (the guest never calls it in this phase), and the only
"suspend" hits were `CreateThread(flags=0x4, CREATE_SUSPENDED)` — normal create-suspended, NOT
`SuspendThread`. **So this is NOT GC stop-the-world parking.** The context-materialization fix
(`9892ba9`) is a valid correctness improvement but was NOT exercised here and is NOT this gate.

Actual state: guest thread at `rva=0x14f180` (Mono metadata bsearch region), HK at **~100% CPU**,
semaphores releasing normally. That is **THROUGHPUT**, not a dead wait — the thread is alive and
churning Mono init, just not reaching D3D11CreateDevice within 240s. So Variant (iii) below is
DEAD; pursue the throughput branch:

THE decisive open question (answer FIRST): **does Mono init COMPLETE if given enough time?**
Run a long boot (900s, ideally 1800s) and sample the heartbeat `blocks`/`steps` at ~60/300/600/900s:
- If `blocks` keeps CLIMBING toward graphics across the run → pure throughput. Make Mono init faster
  (bulk native-promotion of the metadata bsearch/decode-row/string hot chain around `0x14f180`;
  keep AOT cache warm) until it reaches GfxDevice within a normal run, OR accept a slow first boot
  if the AOT cache makes subsequent boots fast.
- If `blocks` PLATEAUS at ~600k and never grows with more time (it ended ~0x94025–0x96ae0 ≈ 605–617k
  across several runs) → it's a **LIVELOCK**, not slow throughput: the thread is re-looping the same
  region forever, waiting for a condition that never becomes true. Then find WHAT `0x14f180`'s loop
  is spinning on (which memory value / which other thread should change it) — that's the real gate,
  and it is upstream of graphics.
The blocks counts being similar (~600k) across multiple runs already HINTS at livelock — confirm or
refute with the long run + per-interval blocks sampling. Do NOT keep micro-optimizing the bsearch
helper until throughput-vs-livelock is settled.

---

## DIAGNOSTIC UPDATE (2026-06-02) — settle A-vs-B BEFORE grinding; new prime suspect
**(SUPERSEDED by UPDATE 2 above — GetThreadContext=0 refuted the SGen-suspend lead. Kept for record.)**

Two findings reshaped rung 1/2. Act on these FIRST:

**(i) Our wait/signal layer is VERIFIED CORRECT — stop suspecting a lost wakeup in it.**
A full review of `macrunner_hb.c`'s futex (`macrunner_hb_rtl_wait_on_address` / `WakeByAddress*`)
and the `WaitForSingle/MultipleObjects` forwarders found NO lost-wakeup: we park via
`NtWaitForAlertByThreadId` and wake via `NtAlertThreadByThreadId`, whose alert is **sticky** (an
alert delivered before the wait makes the wait return immediately) — this closes the enqueue→park
race. `macrunner_hb_get_nt_timeout` maps `INFINITE→NULL` and `0→zero-poll` correctly (not
infinite). Wake filters by exact `addr`, so bucket collisions don't cause false/missed wakes. So a
parked thread means **Wake was never CALLED** — the guest never progressed to the point that signals
it. This is **Variant A** (guest stuck in Mono init), not a primitive bug. Do NOT spend time
"fixing" the futex unless trace evidence below contradicts this.

**(ii) Settle A-vs-B from the EXISTING trace (no new code needed):**
Run with `MACRUNNER_HB_TRACE_WAIT_SEMANTIC=1` (the shim already logs `address-wait-before`,
`address-wake`, and `before/after` for WaitForSingleObject with the addr/handle + caller). For each
address/handle a worker is parked on, grep whether a matching `address-wake` / signal EVER appears:
- **No wake ever for that addr** → Variant A confirmed: the guest is stuck UPSTREAM (Mono init).
  Attack throughput / the Mono-init stall — NOT the sync layer.
- **Wake logged but the thread stays parked** → THEN and only then it's an alert-delivery bug —
  dig into the `NtAlertThreadByThreadId` path for emulated threads.

**(iii) PRIME SUSPECT (confirmed by research): Mono SGen "stop-the-world" thread suspend on our
EMULATED threads.** Mono (Unity's scripting runtime) stops all managed threads before each GC. On
Windows its preemptive backend does `SuspendThread` → `GetThreadContext` (CONTROL|INTEGER|FP) →
later `ResumeThread`/`SetThreadContext`; Mono's own comments say it calls `GetThreadContext` AFTER
`SuspendThread` precisely to wait until the thread is REALLY suspended and to read its register
state (used for conservative GC root scanning). The FIRST GC fires during managed-init (loading
mscorlib/UnityEngine/Assembly-CSharp allocates enough to fill the nursery). The hybrid/cooperative
mode instead self-suspends at safepoints via TLS thread-state + thread-id alerts.

Why this is almost certainly our gate: our guest x64 threads are SOFTWARE-emulated (CPU state lives
in HyperBridge `hb_context_t`, not the host ARM64 register file). If `GetThreadContext` on a guest
thread returns the HOST ARM64 context, a zero/stale context, or the guest isn't actually stopped at
a safe point — Mono's stop-the-world initiator waits forever on its `suspend_semaphore`
(`pending_suspends` never drops to 0). That is EXACTLY: main parked, workers on infinite waits,
nobody wakes.

PROBE SEQUENCE (do in this order; the first step is free and may also be a WORKAROUND):
1. **FREE EXPERIMENT — flip Mono's suspend mode.** Run the boot once with
   `MONO_THREADS_SUSPEND=preemptive`, once with `=coop`, once unset (Unity's bundled Mono reads
   this env var; pass it through `mr-run.sh`'s Wine env). If the hang DISAPPEARS or MOVES under one
   mode → the suspend handshake IS the gate, and you may have an immediate workaround. Record which
   mode changes behavior.
2. **Instrument the suspend path in our ntdll/translator:** log every guest
   `SuspendThread` / `ResumeThread` / `NtGetContextThread(GetThreadContext)` /
   `NtSetContextThread(SetThreadContext)` with: caller, **target guest TID**, and for GetContext
   whether the returned **guest** RIP/RSP/RBP/XMM are sane/non-zero. The decisive question:
   **does `GetThreadContext` on a suspended guest thread return the EMULATED x64 context (from
   `hb_context_t`) or the host ARM64 / zero?** If it's not the real emulated guest context, THAT is
   the root — fix our `NtGetContextThread` to materialize the guest's `hb_context_t` as a WOW64/x64
   `CONTEXT`, and `SetThreadContext` to write back into it.
3. **Verify the guest-TID ↔ host-thread ↔ `hb_context_t` ↔ HANDLE mapping** is 1:1 and consistent —
   Mono targets suspend by the handle/TID it registered at `mono_thread_attach`; if that doesn't map
   to the actual emulated thread, suspend targets the wrong context.
4. **Confirmation signal:** is the main (or any) thread parked while a Mono suspend is outstanding?
   In our terms: a guest `SuspendThread`/`GetThreadContext` was issued against a live guest thread
   and no progress follows. If yes → Mono STW context problem (fix step 2). If the boot has NO
   SuspendThread/GetContext activity at all and main is just churning JIT/metadata → it's genuine
   throughput, attack rung-1 throughput instead.

(Full research with Mono function names, thread map, and the 3 ranked handshakes is in
`reports/research/CHATGPT-BRIEF-mono-init-thread-sync.md` + the ChatGPT answer pasted into the
status log. The `MONO_THREADS_SUSPEND=preemptive` flip is the single highest-value first move.)

Sequence: DECISIVE-RUN with wait-semantic trace → classify A vs B via (ii) → if A, run (iii) probe
(start with the env flip) → fix `GetThreadContext`/suspend fidelity at the root → init progresses →
continue up the ladder.

---

## DECISIVE-RUN (do this FIRST, before more grinding)

Answer "does Mono init complete given time?" once and for all:

```bash
scripts/mr-run.sh engine/wine/dist-arm64ec-spike "Hollow Knight.exe" 900 -- -logFile - \
  ; scripts/mr-clean.sh --prune
```
Env already in mr-run.sh: `MACRUNNER_HB_X64_LOADER=1`, `MACRUNNER_HB_BACKEND=jit`,
`MACRUNNER_HB_JIT_DIRECT_STACK=1`, `MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN=1`,
`MACRUNNER_HB_TRANSLATION_CACHE=1`, `MACRUNNER_GRAPHICS_BACKEND=dxmt`,
`WINELOADERNOEXEC=1`. Add `MACRUNNER_HB_TRACE_HEARTBEAT=1` to sample progress.
Sample heartbeat at ~60s intervals. The verdict is binary:
- progress keeps climbing toward `D3D11CreateDevice` → **throughput** (rung 1 throughput branch).
- progress plateaus + 0% CPU + frozen heartbeat → **gate** (rung 2).
Write the verdict into `reports/research/` and ACT on it immediately — don't wait for a prompt.

---

## SELF-CONTINUATION RULES (this is the part the operator cares about)

- This is ONE mission. After finishing any rung, **immediately start the next rung** in the same
  session. Do not summarize-and-halt. Do not ask "what next" — the ladder above is "what next".
- Treat your own checkpoints as progress markers, not stop signs. Commit a checkpoint
  (`checkpoint(Lane A): <what>`), then keep working.
- Only produce a STOP-and-report when a §STOP condition is genuinely hit.
- Bulk-over-reactive: if a rung's failure is driven by a finite set with a reference impl (e.g. a
  family of Mono-init opcodes / IR ops), cover the WHOLE set once + publish a matrix — never one
  fix per run.
- Keep `reports/research/CODEX-MEGA-PROGRAM-status.md` current as you climb (it's the resume anchor).

## STOP conditions (the ONLY reasons to stop and report)

1. **GOAL reached** — Hollow Knight window with pixels. Report with screenshot/marker evidence.
2. **Hard external blocker** you cannot pass after 3 distinct, genuinely different attempts —
   report the exact blocker (pasted log line / missing symbol), your 3 attempts, and the decision
   you need. Not a fake "blocked" — a real, evidenced wall.
3. **Operator decision required** — an architectural fork with real product trade-offs
   (e.g. "DXMT can't do X without feature Y; ship a stub or implement Y?").
Anything else → keep climbing.

## LANES — do NOT touch (Lane B's files; merge-conflict risk)

`hb_decode_x64.c`, `hb_decode_x86.c`, `hb_lift_x64.c`, `hb_lift_x86.c`, `hb_interpreter.c`,
the differential fuzzer (`hb_fuzz_diff.py`, `hb_diff_case_runner.c`), the oracle
(`tools/hb_oracle/*`), and the golden x64 snapshot. Lane A owns: `hb_arm64_codegen.c` (JIT),
`macrunner_hb.c`, `loader.c`, `signal_arm64.c`, `virtual.c`, graphics/DXMT, `scripts/mr-run.sh`.
If you need a decode/lift fix, note it for Lane B — don't edit their files.

## GUARDRAILS (from CLAUDE.md — never violate)

- Kill Wine ONLY scoped: `WINEPREFIX=<prefix> <dist>/bin/wineserver -k`. NEVER global
  `pkill wine` / `killall wine` (Lane B / other agents run Wine in parallel).
- NEVER `git add -A` / `git add .` — add only specific named files. NEVER edit golden x64 snapshot.
  NEVER update git config. Commit only checkpoints (allowed: this is your mission).
- Every run via `scripts/mr-run.sh` + `timeout`; clean with `scripts/mr-clean.sh --prune`.
  `./scripts/disk-guard.sh` before any long cycle / Wine build (STOP if <30 GB free).
- Never read full trace logs — bound output, sample heartbeat.

## VERIFICATION (must stay green at every checkpoint)

```bash
cd engine/hyperbridge && make            # rc=0
./tests/hb_test_runner                   # NNN passed / 0 failed
bash tools/hb_oracle/fast_validate_family.sh phase1_core rep_movs string_ops   # PASS
```
A checkpoint that breaks the build or any test is not a checkpoint — fix before moving on.
