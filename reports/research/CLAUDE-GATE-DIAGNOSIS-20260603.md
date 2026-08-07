# Claude gate diagnosis (2026-06-03) — for Lane A

Mined Lane A's wait-semantic traces (run `run-20260603-195215-direct-stack-fresh260` + peers).
Conclusion: **the synchronization layer is NOT the blocker. Stop cache/JIT micro-opt AND stop
chasing the wait/futex layer.**

## Evidence
- **Worker-pool handshake WORKS.** Every semaphore main waits on (0x48/0x54/0x60/0x6c/0x78/0x84/
  0x90/0xa4) has a matching `ReleaseSemaphore`. Workers (tid 64/68/72/76/80, all entry
  `0x87efcb89320` = Unity JobQueue pool) run and signal. Waits return.
- **Futex unused here:** 0 `address-wait` events. The earlier "lost WaitOnAddress wakeup" theory is
  dead for this path.
- **`handle=0xffffffffffffffff` waits are benign:** timeout=0, 9 before + 9 after, all return
  (WaitForSingleObject(GetCurrentProcess(),0) polls).
- **Guest PROGRESSES past the pool:** trace tail shows `WaitForSingleObjectEx(0xa4)` return →
  `ReleaseSemaphore(0xa4)` → `CreateEventW(0xb8/0xbc)` → `DuplicateHandle(src_proc=-1, 0xb4→0xc0)`.
  i.e. it's into further init (event/handle setup), not parked on the pool.

## Caveat
wait-semantic logging is BUDGET-CAPPED, so the trace ending ≠ a park. The only reliable
park locator is a **blocks-over-time run** (the earlier 900s `samples.tsv` that pinned `0x5158b4`
with 0% CPU). That was an OLDER build; current configs (direct-stack / semaphore-outer) progress
further, so the plateau likely MOVED.

## What Lane A should do (NOT cache, NOT wait-layer)
1. On the CURRENT build, run HK 600–900s with `MACRUNNER_HB_TRACE_HEARTBEAT=1` and emit
   `samples.tsv` (blocks/steps/rva/cpu at 60/300/600/900s) — same harness as the 900s livelock run.
2. Read the verdict: does `blocks` climb then FLATTEN at a new rva with ~0% CPU? That rva is the
   current gate. If CPU stays ~100% and blocks climb slowly → throughput (different problem).
3. Only THEN, trace that specific rva (what it reads/waits on). Do not pre-optimize.
4. Commit a checkpoint with the samples.tsv + the named plateau rva before any fix attempt.

Net: the sync primitives are fine; the remaining barrier is "where does init plateau on the current
build" — answer that with one blocks-sampling run, don't keep tuning the translation cache.

---
## UPDATE 2 (ChatGPT systems brief + deeper trace mine)

ChatGPT confirmed: the gate is **native Unity graphics/window/render-thread startup, NOT Mono
managed init** (public Unity logs show `GfxDevice: creating device client; threaded=1` + D3D init
BEFORE `Begin MonoManager ReloadAssembly`). Ranked gates between job-pool-ready and D3D11CreateDevice:
1. **render-thread-ready / GfxDevice client handshake** (MOST LIKELY) — main posts "create device"
   to the render/Gfx thread and waits on a manual-reset "ready/device-created" event; render thread
   never reaches the D3D11 call → main parks 0% CPU, D3D11CreateDevice never seen.
2. HWND/window message-pump readiness.
3. globalgamemanagers/UnitySubsystems file-I/O.

Our trace (run-20260603-195215) AGREES with #1: after the job pool, main creates manual-reset
events `0xb8`(initial=0) + `0xbc`(initial=1) via pc=0x6f0...44f0 and `DuplicateHandle 0xb4→0xc0` —
classic gfx/render handshake objects — then the trace cuts off. d3d11.dll/dxgi are LOADED but
`D3D11CreateDevice`/`CreateDXGIFactory` are NEVER reached. No window calls, only boot.config I/O.

CAVEAT: wait-semantic trace is BUDGET-CAPPED (`MACRUNNER_HB_TRACE_WAIT_SEMANTIC_BUDGET`) and
truncates BEFORE the real park — that's why we can't see the final wait.

### Decisive probe for Lane A (ChatGPT's test, adapted)
1. Re-run with a MUCH higher `MACRUNNER_HB_TRACE_WAIT_SEMANTIC_BUDGET` (or unbounded) so the trace
   reaches the actual park, plus heartbeat.
2. Find the FIRST blocking wait AFTER the last successful worker semaphore — print its handle value
   + which CreateEventW/Duplicate created it (events 0xb8/0xbc/0xc0 are prime suspects).
3. Identify the PRODUCER thread for that handle:
   - producer = a CreateThread whose start addr is in UnityPlayer gfx/render code → render-thread
     gate: the render thread is parked BEFORE its D3D11 call. Trace what IT waits on.
   - producer = user32/win32u → HWND/message-pump gate.
   - wait stack = file/server I/O → resource gate.
4. Check: is a render/Gfx thread even CreateThread'd after the pool, and does it RUN (we know the
   engine schedules threads — job workers ran — so if the render thread exists but never executes
   its init, that's the bug; if it runs but parks before D3D11, trace that sub-wait).

Bottom line: STOP cache/JIT work. Raise the wait-semantic budget, capture the real park, name the
handle + its producer thread. The answer is one of the 3 gates above — most likely the render-thread
handshake (events 0xb8/0xbc).

---
## UPDATE 3 (Claude ran 3 HK traces: 240s/600s/900s on CURRENT build) — DECISIVE

The earlier 0% CPU hard-park (0x5158b4) is GONE on the current build. New clean 900s (heartbeat
only, no trace overhead) verdict:
- D3D11CreateDevice / GfxDevice: NEVER reached. No render thread created (only 7 job-pool workers,
  all entry 0x87efcf29320).
- Thread is ALIVE: blocks climb 0x26 → ~0x1a911a (~1.74M) and STILL incrementing at 900s. Not a
  park. But pathologically slow and it does NOT advance to the render/graphics stage.
- **It converges to and LOOPS in guest rva ~0x507xxx–0x515xxx** (last samples: 0x507a8e, 0x5135f4,
  0x5135a9, 0x51359f, 0x51589d). ALL THREE runs (240s/600s/900s) end in this same region.
- block_pc 0x87ef20f35a9 @ rva 0x5135a9 → guest module base ≈ 0x87EF1FE0000, module >5MB ⇒ likely
  **UnityPlayer.dll** or the Mono runtime DLL.
- The ~1.74M block ceiling matches the OLD park point → same barrier; the build change turned a hard
  freeze into a slow LIVELOCK looping the same code.

### Verdict
NOT a sync/wake gate (handshakes complete), NOT the render-thread handshake (never reached),
NOT Mono managed-init. It's a **LIVELOCK in a guest loop at rva ~0x513xxx** (in the 0x507–0x515
span) that re-tests a condition which never becomes true, so init never advances to GfxDevice.

### Precise next step for Lane A (do THIS, nothing else)
1. Resolve rva `0x5135a9` (and the 0x507–0x515 span) to MODULE + nearest export/symbol (use the
   loaded-module map / the base ≈ 0x87EF1FE0000). Name the module (UnityPlayer? mono?).
2. Disassemble the loop at that rva: what memory address/value does each iteration LOAD and compare?
   That value is the livelock condition.
3. Find WHO is supposed to change that value (another thread? a callback? a syscall result our
   engine returns wrong?) and why it never flips. Common causes under translation: a spin-wait on a
   value another thread writes but our memory-ordering/visibility drops it; a counter that depends
   on a Win32/Nt call returning a value we stub wrong; a `QueryPerformanceCounter`/timer/`Sleep`
   loop with a deadline that never elapses because our time source is wrong.
4. Commit the resolved module+symbol+polled-address as checkpoint(Lane A) BEFORE any fix.

This supersedes the render-thread-handshake hypothesis (UPDATE 2) — we never reach thread creation;
the wall is the 0x513xxx loop FIRST. (Traces: claude-gatetrace-*, claude-gatetrace600-*,
claude-clean900-* under reports/phase4-hollow-knight/.)

---
## UPDATE 4 (ROOT CAUSE CONFIRMED at source level) — x86 atomics non-atomic + NO memory barriers (TSO gap)

ChatGPT's #1 hypothesis (x86 TSO / atomic-visibility failure on ARM64) is CONFIRMED by reading the
engine source:
- `hb_interpreter.c` HB_IR_CMPXCHG (~3006): plain read_operand → compare → write_operand. NON-ATOMIC
  read-then-write, no host CAS, no barrier.
- HB_IR_XADD (~3119): plain read→add→write. NON-ATOMIC.
- `hb_arm64_codegen.c`: ZERO ARM64 barriers emitted (no dmb/ldar/stlr/ldaxr/stlxr).
- `hb_decode_x64.c:2370`: LFENCE/MFENCE/SFENCE decoded as "ordering no-ops" (dropped).
- `hb_memory.c`/`hb_runtime.c`: only `__ATOMIC_RELAXED`, and only for internal caches/counters — NOT
  for guest atomics.

### Why this is THE livelock (and an engine-wide bug)
ARM64 is weakly ordered; x86 guests assume TSO. Lock-free producer/consumer code (Boehm GC thread
table/flags, Mono/Unity native scheduler counters) relies on release-store + acquire-load. With no
barriers and non-atomic RMW, a spinning reader never observes the writer's flag → infinite livelock
with the block counter still climbing (exactly the 0x513xxx symptom: pure memory spin, no Win32
calls, never advances). Also causes lost updates / torn RMW on contended atomics. Affects ALL
multithreaded x86 software, not just Hollow Knight — this is why diagnosis kept not converging.

### FIX (Lane A — high impact, engine-wide)
1. **Make guest LOCK-prefixed ops truly atomic:** implement CMPXCHG/CMPXCHG8B/CMPXCHG16B/XADD/XCHG/
   LOCK ADD|OR|AND|XOR|BTS|... via HOST atomics (C11 `_Atomic`/`__atomic_*` with acq_rel/seq_cst, or
   ARM64 LSE `casal`/`ldaddal`/`swpal`, or `ldaxr`/`stlxr` loops). Both interpreter AND JIT.
2. **Honor x86 TSO / fences:** stop treating LFENCE/MFENCE/SFENCE as no-ops — emit `dmb ish`
   (MFENCE = full barrier). Give guest loads/stores x86 ordering.
3. **Best option on Apple Silicon: enable the M-series TSO mode for guest threads** (the per-thread
   total-store-ordering toggle Rosetta uses) so plain translated loads/stores get x86 ordering for
   free — then only LOCK-atomicity needs explicit host atomics. This is the clean, proven approach.
4. Validate: a multithreaded producer/consumer spin test (writer sets flag, reader spins) must
   terminate; then re-run HK — the 0x513xxx loop should exit and init advance toward GfxDevice.

### Confidence / caveat
Source-confirmed that atomics are non-atomic + unordered (definite engine bug). STRONGLY implicates
the 0x513xxx livelock (textbook missing-acquire spin) but not yet 100% proven that THAT loop reads a
cross-thread atomic — final proof = disasm the loop + see a writer thread store the value the loop
reads. But the fix is correct regardless (the non-atomic/no-barrier atomics MUST be fixed for any
multithreaded guest). Prime suspect remains; fix-and-retest is the fastest confirmation.

---
## UPDATE 5 (ChatGPT TSO research) — PRODUCTION FIX = B+C (software TSO + no-hoist), NOT hardware TSO

ChatGPT researched Apple-Silicon TSO concretely. Verdict for a third-party (non-Rosetta) macOS
translator:
- **A. Hardware TSO (ACTLR.TSOEN, AIDR_EL1<9>):** real (Asahi/FEX use it via Linux prctl), best perf
  (~9% vs weak on M1), but **NO public macOS API for third parties** — privileged EL1 register, set
  by kernel for Rosetta only; reachable only via private entitlement (com.apple.private.oahd) or a
  KEXT (TSOEnabler). DO NOT build the product on it. Optional experiment at most.
- **B. JIT software TSO (SHIP THIS):** guest scalar load→`LDAR`, store→`STLR`,
  LOCK/XCHG→`DMB ISH`+ldaxr/stlxr, MFENCE→`DMB ISH`, SFENCE→`DMB ISHST`, LFENCE→`DMB ISHLD`.
  Must apply LDAR/STLR to ALL guest scalar loads/stores to shared memory (heap/globals/module/
  Mono/Boehm/UnityPlayer/escaped), NOT just LOCK ops — x86 TSO orders plain MOVs too. Plain LDR/STR
  only for PROVEN-private stack/TLS/rodata. (FEX: TSO on by default; disabling "highly likely to
  break multithreaded apps". Vector/memcpy TSO are separate expensive toggles — defer.)
- **C. No-hoist guest loads (MANDATORY, do FIRST):** guest memory loads are volatile vs other
  threads — NO CSE/LICM/hoist across loop backedges/calls/atomics/safepoints. Even with TSO, a
  hoisted `mov [flag]` (loaded once, then spin on a host register) = infinite loop. THIS could be
  our 0x513xxx livelock by itself.

### Disambiguation diagnostic (run FIRST — tells which of B/C)
Instrument the hot 0x513xxx loop, per iteration: guest PC, the guest load address, the emitted host
op (LDR / LDAR / cached-register), the value read; and every store to that address from any thread.
- reader does only ONE real load then loops on a register → **C bug (hoisting)** — fix anti-hoist.
- reader reloads (LDR) every iter but sees stale value after writer's store → **B bug (ordering)** —
  add LDAR/STLR.
- reader reloads (LDAR) and sees the new value → fixed.

### Lane A plan (supersedes "enable Apple TSO")
1. Run the disambiguation diagnostic on 0x513xxx (hoist vs ordering).
2. **C:** ensure guest loads aren't hoisted/CSE'd across backedges/calls in hb_arm64_codegen.c.
3. **B:** add a STRICT_TSO mode — LDAR/STLR for guest scalar loads/stores (shared mem), DMB ISH for
   LOCK/XCHG/MFENCE (LFENCE→ISHLD, SFENCE→ISHST). Keep my interpreter LOCK-barrier fix 6c1f519.
4. Validate: message-passing + spin-flag + CAS litmus tests under translated threads MUST terminate;
   ISA fuzz stays 0-mismatch; then Hollow Knight (0x513xxx must exit → GfxDevice).
5. Optimize later (FEX model): private-stack skip, vector/memcpy TSO as separate toggles.
Do NOT chase hardware TSO. (Full ChatGPT answer pasted in session.)

---
## UPDATE 6 (ChatGPT implementation recipe + litmus harness) — READY TO IMPLEMENT

ChatGPT delivered the full code-level recipe + a ready regression harness. TSO research is now
COMPLETE — next is Lane A implementing + running the litmus.

### Rollout order (do in this sequence)
1. **No-hoist (FIRST):** invalidate the guest-memory-load CSE/LICM table at every loop backedge,
   call/helper, atomic, fence, safepoint. Allow reuse only in straight-line runs / proven-private
   stack. (A hoisted `mov [flag]` alone = infinite loop, independent of ordering.)
2. **STRICT_TSO_SAFE:** aligned scalar guest load→`LDAR[B/H/W/X]`, store→`STLR[B/H/W/X]`;
   LOCK/XCHG/MFENCE→`DMB ISH` + atomic + `DMB ISH`; unaligned plain→byte helper wrapped in DMB.
   (Note: LDAR/STLR is RCsc and may OVER-fence Store→Load to SC — fine for the fix; the `sb` litmus
   `both0==0` just flags that, not a correctness fail.)
3. **LSE atomics (aligned 1/2/4/8):** CASAL(B/H) for CMPXCHG, LDADDAL for ADD/SUB/INC/DEC/XADD,
   LDSETAL/LDCLRAL(~mask)/LDEORAL for OR/AND/XOR + BTS/BTR/BTC (CF=old bit), SWPAL for XCHG,
   CASPAL for aligned CMPXCHG16B (#GP if unaligned). LL/SC `ldaxr/stlxr` fallback if no LSE.
   Recover x86 flags from the OLD value using existing non-locked ALU flag helpers (table in the
   ChatGPT answer). INC/DEC preserve CF; OR/AND/XOR set CF=OF=0.
4. **Split-lock fallback:** unaligned/cacheline-crossing LOCK → a global split-lock helper
   (DMB; byte read; rmw+flags; byte write; DMB) — FEX's StrictInProcessSplitLocks model.
5. **Validate with `reports/research/tso_litmus.c`** (compile via llvm-mingw → run under MacRunner):
   `mp`, `spin`, `cas`, `xadd`, `split` MUST pass; `sb both0` allowed (0 = over-fenced note).
   Keep the existing ISA fuzz at 0-mismatch. THEN Hollow Knight → must leave 0x513xxx → GfxDevice.
6. **Exact-ish TSO later (perf):** switch aligned scalar LOADS `LDAR`→`LDAPR` (RCpc) where available
   (keeps x86's allowed Store→Load relaxation, avoids SC over-fencing); keep STLR stores + full LOCK
   barriers; add proven-private-stack plain LDR/STR fastpath; vector/memcpy TSO as separate toggles.

Litmus harness saved at `reports/research/tso_litmus.c`. Full instruction table + flag-recovery in
the ChatGPT answer (pasted in session). This is the complete spec — implementation is mechanical now.

---
## UPDATE 7 (post-TSO: vtable+context gates CLEARED; new gate = Mono JIT helper fault = SMC/code-cache) — ChatGPT Prism/SMC memo

After TSO landed, Lane A cleared two gates on Hollow Knight (run hk-context-snapshot-300):
`invalid_vtable=0` (Mono metadata-fusion guard fixed the bad vtable slot), `context_invalid=0`
(guest-thread context capture fixed). NEW gate, earlier than D3D (GfxDevice=0, D3D11CreateDevice=0):

```
macrunner-hb-runtime-fail: thread block_pc=0x87ef186e309 module=mono-2.0-bdwgc.dll
rva=0x4fe309 out=MEMORY_FAULT reason=JIT helper fault
```

### Root class (ChatGPT memo CHATGPT-BRIEF-prism-smc-jit-codecache.md, sourced from MS Learn /
### QEMU / FEX / BDWGC / ARM64EC ABI):
Mono is itself a JIT — it generates/patches x64 at runtime (managed methods, trampolines,
write-barriers, generic thunks, backpatched call sites). MacRunner caches guest→host translations.
If a guest write/patch/protect to code is NOT reflected as a translation invalidation, Mono jumps
into a host block that is no longer coherent with the guest bytes → helper/trampoline faults.
This is classic SMC / code-cache coherence — the exact thing QEMU (page write-protect + per-page TB
lists) and FEX (page SMC + inline-SMC reconstruct) implement.

### Ranked likely roots (highest first):
1. **Missing SMC invalidation for Mono-generated/backpatched x64 code.**
2. **FlushInstructionCache / NtFlushInstructionCache treated as no-op** for our translation cache
   (MS contract: apps MUST call FIC after generating/modifying code).
3. **VirtualProtect/NtProtectVirtualMemory RW→RX (or RWX→RX) executable transition not treated as
   code-publication** (no invalidation).
4. **Write to already-executable/RWX page not trapped/instrumented** (Mono backpatch with no FIC).
5. **ARM64EC thunk/calling-convention mismatch**: generated x64 calling a native ARM64EC helper
   directly (raw body) instead of via an x64-callable entry/import thunk (RSP align, 32B shadow,
   RCX/RDX/R8/R9). MEMORY_FAULT can be this, not SMC.
6. **BDWGC mprotect dirty-page tracking colliding with our SMC page-protection** (one fault, two
   owners) — needs per-page protection-owner stack.
7. Residual TSO/no-hoist in helper metadata (lower now litmus passes).

### THE single most decisive probe (do FIRST, before any fix):
For fault target `mono-2.0-bdwgc.dll+0x4fe309` (and the pointer it dereferences), log:
- target page generation vs the TB's generation snapshot;
- last guest write to that page (tid/pc/range/value/time);
- last FlushInstructionCache/NtFlushInstructionCache range;
- last VirtualProtect/NtProtectVirtualMemory transition on that range;
- call-target classification: anonymous Mono JIT heap (→SMC) vs PE x64 helper (→patched callsite)
  vs ARM64EC thunk / `__os_arm64x_*` (→thunk/ABI).
If the faulting page was **written or flushed after its translation generation** → SMC confirmed.

### Fix recipe (mechanical once probe confirms):
1. Hook FlushInstructionCache + NtFlushInstructionCache → invalidate guest translations for range
   (base==NULL → whole process).
2. Hook VirtualProtect/NtProtectVirtualMemory → invalidate on EXECUTE transitions; keep SMC-watch
   when EXEC→WRITE.
3. Page→TB ownership: every translated guest page tracks its TBs + incoming/outgoing direct chains.
4. Write-to-translated-page invalidation (page-granular first, exact-range later); if can't host-
   mprotect (Wine/GC also mprotect), instrument guest stores to check per-page "has translated code".
5. page.generation check at dispatch → stale TB discarded + retranslate live bytes.
6. Inline-SMC: if current TB writes its own guest range, reconstruct state, do 1-insn slow write,
   invalidate current TB, return to dispatcher (FEX's fix).
7. ARM64EC target classifier: generated x64 must reach ARM64EC via entry/import thunk, never raw body.
8. Per-page protection-owner stack: keep BDWGC dirty-tracking separate from SMC traps.

### Decisive tests (memo Tests 1-6): page-lifecycle trace; global write-to-translated-page trap
(if HK passes the helper fault → SMC confirmed); FIC-only invalidation; "target is just-written
page" check; ARM64EC boundary validation; GC/SMC mprotect deconfliction.

NOTE: this supersedes UPDATE 3/4's "livelock" framing for the CURRENT gate — TSO is done; the live
barrier is now managed-JIT code-cache coherence. Do NOT chase D3D11 until SMC is settled.
Full memo: reports/research/CHATGPT-BRIEF-prism-smc-jit-codecache.md
