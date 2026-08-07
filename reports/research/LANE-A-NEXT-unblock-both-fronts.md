# LANE A — NEXT: two precise fixes that unblock BOTH PE32 and the HK graphics window

**Date:** 2026-06-07. Lane A is the keystone again — both PE32 and Lane D finished their parts and are
BLOCKED on these two Lane-A fixes. Graphics-first paid off: Lane D proved all 82 HK pixel shaders
render headless; PE32 got Diablo to survive 90s. Only Lane A stands between us and (a) the real HK
window and (b) the i386 window.

## FIX 1 — cover the WOW64 thunk at image=0x87FFF7A0000 offset=0x6C374 (unblocks PE32)
PE32 evidence: i386 DLL init dies on `c0000026` at WOW64 thunk `image=0x87FFF7A0000 offset=0x6C374`.
The bulk CFI fix (commit **50c1eea**) did NOT cover this thunk address. Register unwind metadata
(`RtlInstallFunctionTableCallback` / RUNTIME_FUNCTION entries) for this thunk region so
`RtlVirtualUnwind2` stops raising `c0000026` there. PE32 already verified its own side (MEM_COMMIT
tracking in unixlib.c — Storm.dll crash gone, Diablo survives 90s); this thunk is the last gate to
Phase 2 (i386 window). Files: `macrunner_hb.c` (thunk CFI registration).

## FIX 2 — signal_arm64.c syscall-data-repair tagged-PC false-match (unblocks the real HK window)
Lane D evidence: headless DXMT→Metal path is proven (82/82 HK PS render correct), but the REAL HK x64
run is blocked by a `signal_arm64.c` **syscall-data-repair tagged-PC false-match** before graphics
engages. Fix the false-match so the real HK process gets past it → CreateDXGIFactory→D3D11CreateDevice
(graphics is ready) → window. File: `signal_arm64.c`.

## AFTER both land
- PE32 resumes → i386 window (Diablo/Notepad++ x86).
- Real HK x64 run reaches the DXMT graphics path Lane D already proved → live window.
- Verdict = real on-screen window (CG-capture), not "process alive".

## DISCIPLINE
Owned: `macrunner_hb.c`, `signal_arm64.c` (+ hb_* / xtajit64). ctx for logs (ctx_execute javascript,
NOT ctx_batch_execute — /bin/zsh ENOENT). Targeted build: `make -j8 dlls/ntdll/ntdll.so` → copy to
dist-arm64ec-spike → codesign. Run with `MACRUNNER_RUN_DIR=$RUNDIR` (auto-triage). Heartbeat each step
in LANE-A-PROGRESS.md. Scoped `wineserver -k`, never global pkill. Commit named files only.
