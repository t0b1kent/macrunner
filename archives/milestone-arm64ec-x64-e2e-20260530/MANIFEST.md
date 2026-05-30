# MILESTONE ARCHIVE — ARM64EC plain x86_64 PE runs end-to-end (run_exit=0)

**Date:** 2026-05-30
**What:** snapshot of the SOURCE that achieves the milestone "a plain x86_64 PE runs to clean
exit on MacRunner via the ARM64EC/xtajit64 path, no Rosetta/FEX" + the proof logs/reports.
**Why:** insurance copy so the milestone state cannot be lost/corrupted by later work.

## Proof of the milestone (see proof/)
- ARM64EC-FINISH-x64-result-20260530.md — hello_x64.exe run_exit=0, larger fixture ok, x87 10/10.
- arm64ec-finish-run-final-20260530-104000.log — ProcessInit/ThreadInit/BeginSimulation REACHED,
  many BeginSimulation executed hb=OK faulted=0, run_exit=0.
- ARM64EC-PHASE0-...diagnosis.md — the one residual non-fatal fault (TLS-callback mis-address).
- 121-MILESTONE-...md — Obsidian milestone note.

## Source captured (src/)
- xtajit64/ — the emulator DLL (cpu.c PE side, unixlib.c unix side, private.h, spec, Makefile.in).
- xtajit32-ref/ — the WORKING 32-bit reference (oracle).
- ntdll/ + ntdll/unix/ — loader EC-entry (loader.c, macrunner_hb.c, ntdll_misc.h, unixlib.h).
- hyperbridge/src + include — the x86/x64 translator core (decode/lift/interp/jit/codegen/abi/...).
- scripts/build-wine-arm64ec-spike.sh — the spike build (--enable-archs=arm64ec,aarch64,i386,x86_64).

## Integrity
- CHECKSUMS.sha256 — SHA-256 of every captured file. Verify with: `shasum -c CHECKSUMS.sha256`.

## NOT captured (intentionally)
Build outputs (build*/dist*), .o/.d, prefixes, large binaries — reproducible from source via the
spike script. This archive is source-of-truth, not a built tree.
