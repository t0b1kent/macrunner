# ChatGPT research brief — Prism / ARM64EC handling of SMC, JIT code-cache, and managed runtimes

**Date:** 2026-06-05
**Why:** MacRunner is an x86-64→ARM64 binary translator (analogous to Microsoft's **Prism** on
Windows-on-ARM) running unmodified x64 Windows games on Apple Silicon via pure-ARM64 Wine +
**ARM64EC** + DXMT. We just cleared the Mono vtable + thread-context gates booting Hollow Knight
(Unity + Mono/Boehm GC). The **current blocker** is a fault inside a Mono runtime helper:

```
macrunner-hb-runtime-fail: thread block_pc=0x87ef186e309 module=mono-2.0-bdwgc.dll
rva=0x4fe309 out=MEMORY_FAULT reason=JIT helper fault
```

Strong hypothesis: **Mono is itself a JIT** — it generates x86 machine code at runtime (managed
method compilation, trampolines, write-barriers, generic-sharing thunks). Our translator must
discover, translate, and keep coherent this **dynamically generated / self-modifying x86 code**.
The fault is likely our **code-cache coherence / SMC detection** failing when Mono writes new code
(or patches existing code: backpatching call sites, hotpatch trampolines) and then jumps to it.

Microsoft's Prism solves exactly this on WoA (it runs .NET, Mono, and JIT-heavy games fine). We
cannot use Prism's code, but its **publicly documented design is a correct reference**. Mine it.

## What to research (concrete, with sources)

1. **Prism / xtajit code-cache & SMC handling.** How does Prism (and its predecessor xta/xtajit on
   WoA) detect that an emulated x86 app wrote to a page it previously translated, and invalidate /
   re-translate? Specifically:
   - Page-protection trick (mark translated code pages read-only, trap writes via fault handler)?
   - How is the granularity handled (page vs cache-line vs exact range)?
   - How are **self-modifying** and **just-generated** code distinguished, and what about code that
     is written then immediately executed (JITs)?
   - Any documented "guest icache flush" hooks Windows/Prism rely on (e.g. does Prism intercept
     `FlushInstructionCache`, `NtFlushInstructionCache`, VirtualProtect→PAGE_EXECUTE transitions)?

2. **How JITs (.NET CLR, Mono) cooperate with Prism.** Do managed runtimes on WoA emit a signal
   the emulator uses (e.g. allocate code via a specific API, call FlushInstructionCache, use
   W^X / dual-mapping)? Is there a documented contract a JIT must follow to be emulated correctly?
   What does Mono do on ARM/emulated targets to publish generated code?

3. **ARM64EC specifics relevant to managed code & helpers.** How do x64↔ARM64EC transitions work
   for **runtime-generated** code (which has no ARM64EC counterpart)? Fast-forward sequences,
   entry/exit thunks, `__os_arm64x_*` dispatch, the emulation entry point. When Mono-generated x64
   calls a helper, how does the boundary get crossed under emulation, and what invariants
   (register/stack/`RSP` alignment) must hold? Could a MEMORY_FAULT in a "helper" actually be a
   mis-handled EC thunk / mismatched calling convention rather than a bad pointer?

4. **Memory ordering (already solved, confirm).** We implemented software TSO (LDAR/STLR + LSE +
   no-hoist). Confirm Prism/WoA do the same in software and that Apple's hardware TSO
   (ACTLR.TSOEN / Rosetta) is genuinely not third-party usable on macOS.

5. **Boehm GC (bdwgc) under emulation.** mono-2.0-bdwgc uses Boehm conservative GC. Any known
   emulation hazards: stack scanning, `setjmp`/register flushing for root scanning, mprotect-based
   incremental GC / dirty-page tracking colliding with the emulator's own page-protection SMC trap?
   (A GC that mprotects heap pages + an emulator that mprotects code pages can interfere.)

## Deliverable I want back
- A ranked list of the **most likely root causes** for "MEMORY_FAULT in a Mono JIT helper under an
  x86→ARM64 translator," each tied to the mechanism above.
- For the top causes, the **concrete fix recipe** a translator must implement (SMC detection +
  code-cache invalidation design; FlushInstructionCache/VirtualProtect interception; EC-thunk
  handling for generated code; GC-mprotect vs SMC-mprotect deconfliction).
- Citations: MS Learn (ARM64EC ABI, Prism, "Arm64X"), Windows Internals, dev blogs, conference
  talks (e.g. on Prism/xta), and any patents describing the WoA emulation code cache.
- A short **decisive test** we can run on MacRunner to confirm which cause it is (e.g. log every
  guest write that hits a translated page; intercept FlushInstructionCache; check whether the
  faulting target is a just-written page).

Keep it actionable for translator engineers; we own the engine source.
