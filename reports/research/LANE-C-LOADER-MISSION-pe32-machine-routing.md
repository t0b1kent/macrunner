# LANE C (loader/server) MISSION — route an I386 main image into WOW64 on ARM64 (unblocks PE32)

**Owner:** Lane C — `ntdll` Unix loader + Wine server. **Date:** 2026-06-07
**Source of truth:** `reports/research/PE32-NEEDS.md` (PE32 diagnosed this exactly; do not re-diagnose).
**Why critical:** this is THE single blocker for the entire 32-bit front. PE32's syswow64 deploy,
runner-env, and CPU-provider exports are ALL already correct (xtajit exports BTCpuProcessInit/
ThreadInit/Simulate; syswow64 has x86 start.exe+kernel32). The ONLY thing missing is the loader
routing an i386 main image into the WOW64 path instead of rejecting it and substituting native start.exe.

## THE BUG (verified, 3 locations)
1. `engine/wine/server/mapping.c:1697-1715` — keeps `current->process->machine` native (ARM64) and
   raises `STATUS_IMAGE_MACHINE_TYPE_MISMATCH` when the PE32 main image (`req->machine ==
   IMAGE_FILE_MACHINE_I386`) maps into the ARM64 loader process. ← root reject.
2. `engine/wine/dlls/ntdll/unix/env.c:1960-1988` — `load_main_exe()` treats that non-success map as
   invalid → calls `load_start_exe()`.
3. `engine/wine/dlls/ntdll/unix/loader.c:2368-2379` — `load_start_exe()` uses
   `get_machine_wow64_dir(current_machine)` → loads native `C:\windows\system32\start.exe`
   (machine=aa64) BEFORE WOW64 → `BTCpuProcessInit` never reached.

## THE FIX (PE32's requested change — implement exactly this)
- On ARM64 MacRunner, allow the FIRST PE32 main image to **become the process guest machine**
  (`IMAGE_FILE_MACHINE_I386`) instead of returning machine-type-mismatch and falling back to
  start.exe. I.e. when the main image is I386 and the host is ARM64, set `process->machine = I386`
  and proceed down the WOW64 bring-up (the same way an x86_64 guest on ARM64 becomes a WOW64-ish
  guest today — mirror that path).
- KEEP the existing "a 32-bit process MAY map the native machine" allowance at `mapping.c:1714` so
  that once the process machine is I386, the native ARM64 support images (ntdll/wow64/xtajit) still
  map cleanly.

## HARD GUARDRAILS (do not regress the shipped x64 window — Lane A depends on it)
- Change ONLY the **I386-main-image-on-ARM64** branch. Do NOT alter AMD64/ARM64 main-image behavior:
  Lane A's Hollow Knight x64/ARM64EC boot path uses this exact loader and MUST stay byte-for-byte
  behaviorally identical. If in doubt, gate the new behavior on `req->machine == I386`.
- BEFORE editing: `git status` the loader tree — there are pre-existing uncommitted changes
  (loader.c/x64). Do NOT clobber them; commit ONLY your own named hunks (`feat(Lane C): ...`), never
  `git add -A`.
- AFTER editing: run a regression smoke of the x64 path (the existing HK x64 / x64 loader smoke) and
  paste evidence that AMD64/ARM64 main-image loading is unchanged, in addition to the PE32 gate below.
- Owned files: `engine/wine/server/mapping.c`, `engine/wine/dlls/ntdll/unix/env.c`,
  `engine/wine/dlls/ntdll/unix/loader.c`. **NEVER touch** `macrunner_hb.c` / `signal_arm64.c`
  (Lane A) or `xtajit/**` interp (PE32).

## DISCIPLINE
- ctx (`ctx_execute_file`/`ctx_execute`) for logs — not raw Read/Bash on big logs. NOTE: in this env
  `ctx_batch_execute` shell mode fails (`spawn /bin/zsh ENOENT`) — use `ctx_execute` with language
  `javascript` (execFile/FS) instead. Heartbeat each step → `reports/research/LANE-C-PROGRESS.md`.
- Rebuild targeted: `make -j8 dlls/ntdll/ntdll.so` + the server, copy to dist, codesign; re-run NPP x86.
- Scoped kill only (`WINEPREFIX=$PWD/bottles/generic-x86 wineserver -k`), never global pkill.

## VALIDATION GATE (done = all three)
1. `build_main_module ... notepad++.exe ... machine=014c` (I386, not aa64 start.exe).
2. `wow64.dll` / `wow64cpu.dll` / `xtajit.dll` load attempt appears in the trace.
3. `BTCpuProcessInit` or `BTCpuThreadInit` marker appears.
Plus: x64 regression smoke shows the Hollow Knight / x64 loader path unchanged.

## AFTER THIS LANDS
PE32 lane resumes its mega-program from Phase 1 (i386 CPU executes → ISA bulk → window). Coordinator
re-arms the PE32 autoloop. End the turn with `LOOP-STATUS: GOAL` (gate met + x64 unregressed),
`LOOP-STATUS: BLOCKED <why>`, or `LOOP-STATUS: CONTINUE` (written at column 0).
