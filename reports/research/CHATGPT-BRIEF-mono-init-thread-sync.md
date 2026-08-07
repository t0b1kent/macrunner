# ChatGPT brief — Mono embedded-runtime init: the thread-coordination Wine+translator must satisfy

Tightly scoped so it completes in ONE session. No attached file needed — pure public
Mono/Unity/Wine knowledge. Produce the full numbered deliverable in one pass.

## Context (everything you need)
We run an UNMODIFIED Unity game (Mono scripting backend, ~Unity 2017–2020), x64 Windows build, on
Apple Silicon via a custom x86→ARM64 binary translator + native ARM64 Wine 11. We already verified
our Win32 wait/signal primitives (WaitOnAddress/WakeByAddress, WaitForSingleObject, thread-id
alerts) are CORRECT. Symptom: the guest MAIN thread parks DURING Mono managed-init (before the
graphics device is created), worker threads sit on infinite waits, and **no wake is ever fired** —
i.e. the guest never PROGRESSES to the point that would signal anyone. We've concluded the problem
is Variant A (the guest is stuck inside Mono init / a thread handshake), NOT a lost wakeup. We need
the exact thread-coordination Mono performs during init so we can find which handshake our
translator breaks (likely something involving thread suspend/resume or thread context).

## Questions (finite, answerable from mono/mono source + public reports)
1. **Threads Mono creates during embedded init** (`mono_jit_init` / `mono_runtime_init` / first
   managed exec): finalizer thread, SGen GC thread(s), threadpool, debugger/soft-debug thread, etc.
   For EACH: when it's created, what it blocks on at startup, and what signals it.
2. **SGen "stop-the-world" GC suspend/resume** (the prime suspect): on WINDOWS, exactly how does
   Mono suspend and resume managed threads — `SuspendThread` + `GetThreadContext` +
   `ResumeThread`? a cooperative self-suspend handshake via events? What's the precise protocol,
   which OS calls, and WHEN during init does the FIRST GC fire? If `SuspendThread`/
   `GetThreadContext`/`ResumeThread` on a thread mis-behaves (wrong/zero context, never actually
   suspends, resume lost), WHERE does the GC thread or the main thread block, and on what?
   Distinguish Mono's **cooperative** suspend (preemptless, hijack via safepoints) vs
   **preemptive/hybrid** suspend (real SuspendThread) — which is the Windows desktop default for
   this era, and how to tell which one is in use.
3. **Thread-attach / managed-thread registration handshake**: when a thread first enters managed
   code (`mono_thread_attach`), what lock/event coordinates it with the GC state machine? Can a
   thread that attaches but is never seen as "suspended/running" by the GC stall stop-the-world?
4. **Loader/domain locks during init**: `mono_jit_init` → root+app domain create → load `mscorlib`,
   `UnityEngine.dll`, `Assembly-CSharp.dll`. Which locks (Mono "loader lock", domain lock, image
   lock) are held, and what reentrancy/ordering would deadlock them under a slow translated path?
5. **Most common init-HANG failure mode** when Mono is ported to a new OS/threading substrate
   (Wine, WSL, consoles, mobile, emulators) — ranked, with sources (mono/mono GitHub issues, Unity
   forums, WineHQ/Proton bug reports). Especially: cases where thread suspend/resume or thread
   context retrieval was the root cause.

## Deliverable
A numbered **thread + sync-point map** of Mono init, each item tagged **[CONFIRMED + source]** or
**[INFERRED]**. End with a ranked list: **"the 3 Mono-init handshakes most likely to hang under a
binary translator that emulates guest threads but may mis-deliver SuspendThread / GetThreadContext
/ ResumeThread (or async suspend signals)"** — for each, name the exact thread that parks, the
exact object/state it parks on, and the ONE diagnostic that would confirm it (e.g. "log every
SuspendThread/GetThreadContext the guest issues; if a SuspendThread targets the GC's victim thread
but the returned CONTEXT is zero, that's it").
