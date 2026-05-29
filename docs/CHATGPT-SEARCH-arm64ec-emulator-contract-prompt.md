# ChatGPT (web search) — ARM64EC research #2: custom emulator contract + lld linking (PASTE-READY)

> Вставь весь блок ниже в ChatGPT с web search. Самодостаточный, доступа к репо не нужно.

---

Follow-up systems-engineering research. Context: I run x86_64 Windows games on Apple
Silicon (ARM64) macOS WITHOUT Rosetta, using **my own x86_64→ARM64 binary translator
("HyperBridge")** plus a pure-ARM64 fork of **Wine 11**. Graphics go DirectX→Metal via a
native ARM64 backend reached through Wine's `WINE_UNIX_CALL`. I have confirmed my Wine was
built with `--enable-archs=aarch64,x86_64,i386` (NO `arm64ec`), so `x86_64-windows/ntdll.dll`
lacks `__wine_unix_call_dispatcher_arm64ec` and `KiUserEmulationDispatcher`. My llvm-mingw
toolchain (May 2026 build) DOES ship `arm64ec-w64-mingw32-clang++` etc.

I need two things researched precisely, with sources/links + version/commit specifics.

## QUESTION 1 — Can a CUSTOM x64 emulator replace FEX behind ARM64EC in Wine?
ARM64EC on Linux/macOS Wine normally pairs with **FEX-emu** as the x64 CPU emulator. My
product thesis is to use MY OWN translator (HyperBridge), NOT FEX, NOT Rosetta. So:

- What is the exact contract/ABI between Wine's ARM64EC ntdll and the x64 emulator? i.e.
  what must an emulator implement to be the backend that `KiUserEmulationDispatcher` and
  `ProcessPendingCrossProcessEmulatorWork` hand x64 execution to?
- How does Wine locate/load the emulator (is it a fixed `xtajit.dll`-style module name, an
  env var, a registry/spec entry, a documented entry-point set)? Cite Wine source files
  (`dlls/ntdll/signal_arm64ec.c`, `loader.c`, `unix_lib.c`, `ntdll.spec`, and any
  emulator-loading code) by function name.
- Is the emulator interface **generic/pluggable**, or is it hard-coded to FEX? Specifically:
  can a third party provide their own emulator DLL implementing the same entry points
  (the Microsoft `xtajit.dll` / `BTCpu*` style interface, e.g. `BTCpuProcessInit`,
  `BTCpuSimulate`, `BTCpuResetToConsistentState`, etc.) and have Wine-ARM64EC dispatch to
  it instead of FEX? What entry points are mandatory?
- Does Microsoft document the WoW64 x64-on-ARM64 emulator contract (the `xtajit64.dll` /
  `BTCpu64` interface)? Link the docs / headers / reverse-engineering notes if any.

## QUESTION 2 — lld ARM64EC linking state as of 2026
Earlier research said LLD historically could NOT fully link ARM64EC hybrid PEs and people
used MSVC `link.exe`. I need the CURRENT (2025-2026) state:

- As of LLVM 21 / 22 (2025-2026), can `lld` (`ld.lld` / `lld-link`) link ARM64EC and ARM64X
  hybrid PEs end-to-end, including the hybrid import/export thunks, WITHOUT MSVC link.exe?
  Cite LLVM release notes, commits, or llvm-mingw changelog entries by version.
- Specifically for building Wine's ARM64EC `ntdll.dll`: does the standard
  `--enable-archs=arm64ec` Wine build path link cleanly with llvm-mingw's bundled lld, or
  does it still require an external Microsoft linker? Any FEX-emu / wine-arm64ec build docs
  that state the exact linker requirement in 2025-2026.
- If MSVC link.exe is still required, can it be run on macOS (under emulation) as part of a
  Wine ARM64EC build, or must the build happen on Windows-on-ARM / Linux?

## OUTPUT
- Direct answer to Q1: is ARM64EC's emulator backend pluggable enough that a custom
  translator can replace FEX? Yes/No/Partially, with the exact required entry points.
- Direct answer to Q2: can llvm-mingw's lld link ARM64EC Wine ntdll in 2026, yes/no, version.
- "Open questions" for anything unconfirmed — mark unknown, do NOT guess.
- Cite every non-obvious claim with link / version / commit.
