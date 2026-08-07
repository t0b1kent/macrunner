# ChatGPT research brief — COMPLETE technical reference on Microsoft Prism (Windows-on-Arm x86/x64 emulation)

**Date:** 2026-06-05
**Goal:** Produce the most complete, public-sourced technical reference on **Prism** (and the wider
Windows-on-Arm x86/x64→ARM64 emulation stack) so the MacRunner team has a full mental model of how a
shipping, mature x86→ARM64 translator is designed. MacRunner is our own x86-64→ARM64 binary
translator (HyperBridge) + pure-ARM64 Wine 11 + ARM64EC + DXMT, running unmodified x64 Windows games
on Apple Silicon **without Rosetta**. Prism is the closest existing system to what we are building —
we cannot use its code (closed Microsoft IP), but its **publicly documented design is our reference
oracle**. Dig up EVERYTHING that is public and turn it into an engineering reference.

## Ground rules for the answer
- **Cite every claim** with a source (MS Learn, Windows Dev Blog, Build/Ignite talks, Black Hat /
  conference research, patents, reputable reverse-engineering writeups, Wikipedia only as pointer).
- Clearly mark each item **[CONFIRMED + source]**, **[INFERRED]**, or **[UNKNOWN/not public]**.
- Where Prism internals are not public, say so explicitly and give the **closest public analog**
  (Rosetta 2, QEMU TCG, FEX-Emu, Box64, Apple's docs, academic DBT papers) as the reference model.
- Keep it actionable for translator engineers (we own the engine source).

## Sections to cover (be exhaustive)

### 1. History & lineage
- The evolution: Windows 10 on Arm x86-only emulation → `xtajit` / `XtaCache` → **Prism** in
  Windows 11 24H2 (x64 support, performance). Names, versions, when x64 emulation arrived, what
  Prism changed vs the old emulator. CHPE → ARM64EC transition.

### 2. Overall architecture
- The components and their roles: the emulator DLL(s) (`xtajit.dll`/`xtajit64.dll` equivalents),
  the cache compiler (`xtac.exe`), the cache service (`XtaCache.exe`), how an x86/x64 process is
  launched and how control enters the emulator, the ARM64X/ARM64EC system DLLs it calls into.
- Process model: how an emulated x64 app coexists with native ARM64 system DLLs in one address
  space (ARM64EC), the "hybrid" image layout (Arm64X PE), and the boundary mechanism.

### 3. Translation pipeline (the JIT)
- Block/trace formation: basic-block vs superblock/trace; decode → IR → ARM64 codegen.
- Register mapping x64→ARM64 (GPR/XMM/flags), flag emulation strategy, FP/SIMD (SSE/AVX) mapping.
- Direct block chaining / dispatch loop design; how indirect branches and returns are handled
  (return-address prediction, indirect-branch target cache).
- Hot-path re-optimization: does Prism re-JIT hot blocks at higher quality? Tiering?

### 4. Code cache (this is critical for us)
- The **persistent on-disk cache** (`XtaCache`): file format, where stored, keyed by what
  (module identity/hash/RVA), how it is reused across launches and across processes, security model
  (the Black Hat "Jack-in-the-Cache" research is directly relevant — summarize its findings on the
  cache file format and trust model).
- The **in-memory** code cache: per-module vs per-page organization, eviction, size limits.
- How translated code for **anonymous / JIT-generated** memory (no module identity) is handled vs
  PE-image code.

### 5. Self-modifying code (SMC) & code-cache coherence
- Everything public about how WoA emulation detects guest writes to previously-translated code and
  invalidates/re-translates. Page-protection traps? `FlushInstructionCache`/`NtFlushInstructionCache`
  interception? Granularity? Inline-SMC (writing the currently executing block)?
- How JIT-heavy guests (.NET CLR, Mono, Java, browsers' JS JITs, games) work correctly under it —
  the contract a guest JIT must follow.
- Compare to the public SMC designs of **QEMU TCG** (page write-protect + per-page TB invalidation),
  **FEX-Emu** (page SMC + inline-SMC reconstruction), **Rosetta 2** (what's known), **Box64**.

### 6. ARM64EC / Arm64X deep dirt
- The ABI in detail: how x64 and ARM64EC interoperate in one process; entry thunks, exit thunks,
  call checkers, `__os_arm64x_dispatch_call_no_redirect` and the `__os_arm64x_*` family; the
  "fast-forward sequences"; how the emulator decides a call target is EC-native vs x64-emulated.
- Arm64X PE format: how one binary carries both ARM64 and x64/EC code, the dynamic value relocations.
- How runtime-generated x64 (which has no EC alternate) calls into native ARM64EC code safely.

### 7. Memory model / ordering
- What is publicly known about how Prism implements x86 **TSO** on ARM64 (weak) — software
  barriers vs any hardware assist on Qualcomm Snapdragon (is there a TSO-like mode?). Contrast with
  Apple Silicon's Rosetta hardware TSO (ACTLR.TSOEN) which is NOT third-party usable on macOS.
- Atomics/LOCK-prefix handling, fences (MFENCE/LFENCE/SFENCE).

### 8. System-call & API boundary
- How emulated x64 code reaches native NT syscalls; the syscall thunking; how Win32 (user32, gdi32,
  d3d11, dxgi) is serviced — natively in ARM64 or emulated? The role of ARM64EC system DLLs.
- GPU/graphics path under emulation (D3D translation, if anything public).

### 9. Performance techniques & numbers
- Public benchmarks/claims (Prism vs old emulator, % of native), and the techniques that get there:
  caching, chaining, flag-elision, SIMD width handling, hot re-JIT, large-page code cache, etc.

### 10. Comparison table: Prism vs Rosetta 2 vs QEMU vs FEX vs Box64 vs MacRunner
- Columns: translation unit, cache (persistent?), SMC strategy, memory-order strategy, EC/interop
  model, syscall model, OS. Rows = each engine. Mark what MacRunner should adopt.

### 11. Direct lessons for MacRunner (the payoff)
- A prioritized list of concrete design decisions we should make based on the above:
  code-cache structure, SMC detection, FIC/VirtualProtect interception, EC thunk handling for
  generated code, memory ordering, dispatch/chaining — each with the Prism/analog evidence behind it.

## Deliverable
A single comprehensive reference doc with the above sections, every claim sourced, gaps marked
[UNKNOWN], plus the section-11 prioritized recommendations and the section-10 comparison table.
This is a long research task — be thorough; it's meant to be our standing Prism reference.
