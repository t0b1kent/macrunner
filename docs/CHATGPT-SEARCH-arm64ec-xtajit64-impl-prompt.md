# ChatGPT (web search) — ARM64EC research #3: xtajit64 / BTCpu64 IMPLEMENTATION reference (PASTE-READY)

> Вставь весь блок ниже в ChatGPT с web search. Самодостаточный, доступа к репо не нужно.

---

Implementation-reference research. Context: I'm building my own x86_64→ARM64 binary
translator ("HyperBridge") and wiring it into a pure-ARM64 fork of **Wine 11** as the x64
emulator behind **ARM64EC** (replacing FEX-emu). Wine loads an emulator DLL named
`xtajit64.dll` exporting a `BTCpu64*` / Microsoft-WoW64-style interface. I already have the
DLL scaffold and `.spec` with these exports, but the actual simulation entry points are
stubbed. I need the precise contract/semantics of each so I can implement them by routing
into my translator. Use Wine source (`dlls/ntdll/signal_arm64ec.c`, `loader.c`), FEX-emu's
Windows ARM64EC module (`libarm64ecfex.dll` / FEXCore), and WoW64-internals reverse
engineering. Cite source files/functions/commits.

My xtajit64.spec exports (need semantics for each):
```
BTCpu64FlushInstructionCache, BTCpu64IsProcessorFeaturePresent,
BTCpu64NotifyMemoryDirty, BTCpu64NotifyReadFile,
DispatchJump, RetToEntryThunk, ExitToX64, BeginSimulation,
FlushInstructionCacheHeavy, NotifyMapViewOfSection, NotifyMemoryAlloc,
NotifyMemoryFree, NotifyMemoryProtect, NotifyUnmapViewOfSection,
ProcessInit, ProcessTerm, ResetToConsistentState, ThreadInit, ThreadTerm,
UpdateProcessorInformation
```

## What I need, precisely:

### A. Lifecycle + simulation entry
- `ProcessInit` / `ThreadInit`: what state must the emulator set up? What does Wine's
  `arm64ec_process_init()` expect back (processor feature flags, `UpdateProcessorInformation`
  contract)? What must be returned for success vs. process abort?
- `BeginSimulation`: this is the core. When/how is it invoked, what context does it receive
  (the x64 CPU context / where execution should start), and what is the expected control
  flow — does it run an x64 execution loop until a transition back to ARM64 native code?
- The relationship between `KiUserEmulationDispatcher` (Wine ntdll export) and the
  emulator's `BeginSimulation` / `BTCpuSimulate`: who calls whom, and with what.

### B. The x64 <-> ARM64EC transition mechanism
- `DispatchJump`, `RetToEntryThunk`, `ExitToX64`: explain the fast-forward / entry / exit
  thunk mechanism. How does control pass from native ARM64EC code into emulated x64 and
  back? What are these symbols (code stubs? thunk targets?) and what must they contain.
- The **BOP code** mechanism (`BTCpuGetBopCode` in the 32-bit interface; the x64 analog):
  what is a "BOP", how does the emulator use it to mark x64→emulator transition points?

### C. Memory / cache notifications
- For each `Notify*` (MemoryAlloc/Free/Protect, MapView/UnmapView, MemoryDirty, ReadFile)
  and `FlushInstructionCache*`: what is the emulator expected to do (JIT cache invalidation,
  code-cache coherency)? Which are safe to leave as no-op stubs for a first working build,
  and which are mandatory for correctness?
- `ResetToConsistentState`: when is it called (exceptions?), and what must the emulator
  restore?

### D. FEX as concrete reference
- How does FEX's `libarm64ecfex.dll` structure these exports? Point to the FEX source files
  that implement `BeginSimulation` / the BTCpu entry points for ARM64EC, so I can mirror the
  structure (not copy — understand the contract).

## OUTPUT
- A per-function table: name → when called → inputs → required behavior → "mandatory vs
  stub-ok for first build".
- The transition-thunk mechanism explained concretely (DispatchJump/RetToEntryThunk/
  ExitToX64 + BOP).
- Links to Wine `signal_arm64ec.c` lines and FEX source for the implementation.
- "Open questions" for anything undocumented — mark unknown, do NOT guess.
