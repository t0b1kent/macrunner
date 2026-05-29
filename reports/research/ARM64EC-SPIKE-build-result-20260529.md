# ARM64EC feasibility spike — BUILD RESULT: GREEN

**Date:** 2026-05-29
**Spike:** Part 1 (build + verify exports) of `docs/CLINE-arm64ec-feasibility-spike-master-brief.md`.
**Executed by:** Cline (build) + operator-side verification (export check — Cline did not produce
the verdict, so it was done here).
**VERDICT: GREEN.** Our lld links arm64ec/arm64x. ntdll exports the required symbols.

## The one question — answered
> Does OUR toolchain's lld link an arm64ec hybrid PE, and does `ntdll` export
> `__wine_unix_call_dispatcher_arm64ec` + `KiUserEmulationDispatcher`?

**YES on both.** Hard evidence below.

## Evidence

### 1. Build config (the only change vs baseline)
`scripts/build-wine-arm64ec-spike.sh:47` → `--enable-archs=arm64ec,aarch64,i386`
+ `--disable-tests`. Baseline `build-wine-pure-arm64-experiment.sh` untouched. Separate
build/dist dirs: `engine/wine/build-arm64ec-spike` / `engine/wine/dist-arm64ec-spike`.

### 2. configure accepted arm64ec + lld (the risk gate)
From `/tmp/arm64ec-spike-build.log`:
```
checking for arm64ec-w64-mingw32-clang... arm64ec-w64-mingw32-clang
checking whether arm64ec-w64-mingw32-clang supports -target arm64ec-windows -fuse-ld=lld
  -Wl,-subsystem:console -Wl,-WX --no-default-config... yes
```
→ our clang+**lld** accept `-target arm64ec-windows -fuse-ld=lld`.

### 3. Build completed, ZERO errors
Build log tail: `arm64ec spike installed: .../dist-arm64ec-spike`.
`grep -c "Error [0-9]| *** |ld.lld: error| ..."` → **0**. No lld link failures.

### 4. ntdll is an ARM64X hybrid + exports present (THE deliverable)
```
$ llvm-objdump -f dist-arm64ec-spike/lib/wine/aarch64-windows/ntdll.dll
  file format coff-arm64x        <-- hybrid ARM64 + ARM64EC binary

$ llvm-readobj --coff-exports .../aarch64-windows/ntdll.dll | grep -iE 'arm64ec|KiUserEmulation'
  Name: KiUserEmulationDispatcher
  Name: __wine_unix_call_dispatcher
  Name: __wine_unix_call_dispatcher_arm64ec
```
The arm64ec/EC code is emitted as an **ARM64X hybrid inside `aarch64-windows/ntdll.dll`**
(not a separate `arm64ec-windows/` dir — that's expected for Wine's arm64x model). Both
hardcoded-required exports are present.

## What this unblocks
- Kimi's blocker (`__wine_unix_call_dispatcher_arm64ec not found`, ntdll EXEC_FAULT) was caused
  by the missing `--enable-archs=arm64ec`. This build produces those exports → root cause fixed
  at the build level.
- The "own translator, no Rosetta, no FEX" thesis is build-feasible: lld links the hybrid PE.
- Research Q2 (does our lld link arm64ec) is now CONFIRMED empirically, not just theoretically.

## Notes / caveats (open)
- **Verified: lld links arm64x ntdll.** NOT yet verified: that a full game-side x86_64 PE +
  xtajit64.dll link cleanly, and that runtime emulation works (that is Part 2 — wiring
  `xtajit64/cpu.c` → HyperBridge, currently still stubbed).
- bison-too-old blocked the first configure attempt (`/tmp/arm64ec-spike-configure.log`); it
  was resolved before the successful build. If reproducing on a clean machine, ensure bison ≥ 3.0.
- Cline produced a discipline checklist + a source-grep dump (`reports/phase-h/
  h2-upstream-local-arm64ec.txt`) but did NOT produce this verdict/export check — that gap was
  closed operator-side. Lesson: "build installed ✅" ≠ "arm64ec linked + exports present".

## NEXT (Part 2, now unblocked — needs operator go-ahead)
Wire `engine/wine/dlls/xtajit64/cpu.c` stubs → HyperBridge x64 (BeginSimulation/transition
thunks) per `reports/research/ARM64EC-XTAJIT64-IMPL-chatgpt-20260529.md`. Mirror the working
32-bit `dlls/xtajit/cpu.c`. Then rebuild this spike tree and try a real x86_64 PE.
