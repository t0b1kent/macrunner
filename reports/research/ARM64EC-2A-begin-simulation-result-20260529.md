# ARM64EC Part 2a — runtime result: BLOCKED BEFORE BeginSimulation (loader layer)

**Date:** 2026-05-29
**Brief:** `docs/CLINE-arm64ec-part2a-begin-simulation-reach-master-brief.md`
+ fix step `docs/CLINE-arm64ec-part2a-fix-c0000135-loaddll-brief.md`.
**Executed by:** Cline (ran the +loaddll trace, then hung; wrote a first draft with two factually
wrong claims) + operator-side verification from `/tmp/arm64ec-2a-loaddll.log` (16466 lines) and
direct on-disk checks. **This file is the corrected operator verdict.**
**VERDICT:** Runtime is **blocked at the loader, BEFORE `BeginSimulation`.** Per the brief,
"blocked before BeginSimulation at X" is a valid milestone result — it names the next blocker.
The blocker is a runtime STATUS_DLL_NOT_FOUND on the EC/x64 child's unix ntdll **even though that
file exists and its deps resolve** — i.e. an internal resolution failure, NOT a missing file.

## The question — answered with evidence
> At runtime, what is the furthest point reached toward `BeginSimulation`?

**Our emulator DLL code NEVER executed.** Full-trace search for our markers
(`macrunner-xtajit64`, `BeginSimulation`, `REACHED`, ProcessInit/ThreadInit MESSAGE, exit
`0x6502`): **zero hits.** No lifecycle log, no BeginSimulation, no 0x6502 marker. The process
died in the loader.

## Evidence 1 — xtajit64.dll DID load and map (corrects Cline draft #1)
The `find_builtin_dll cannot find builtin library` line is BENIGN: the loader immediately falls
back to mapping the real PE, which **succeeds** (note arm64ec-hybrid sections `.hexpthk`,
`.a64xrm`):
```
16188 find_builtin_dll looking for "xtajit64.dll" for file ...\system32\xtajit64.dll
16189 cannot find builtin library for ...xtajit64.dll      <- benign; PE fallback follows
16190 map_image_into_view mapping PE file ...xtajit64.dll at 0x6ffff9690000
16196 ...section .a64xrm ...                                <- arm64ec hybrid section present
16205 relocating ...xtajit64.dll ... mapped at 0x6ffff9690000
```
So xtajit64.dll is NOT rejected — it maps. (Cline's draft claimed it was not accepted; that's wrong.)

## Evidence 2 — the actual fatal (last line of the log)
```
16466 wine: failed to load .../dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.dll error c0000135
```

## Evidence 3 — the file EXISTS and its deps resolve (corrects Cline draft #2)
Cline's draft claimed `aarch64-unix/` has no ntdll. **False — verified on disk:**
```
$ ls -la .../lib/wine/aarch64-unix/ntdll.so
-rwxr-xr-x  872440  ntdll.so                         <- present (aarch64-unix has 34 entries)
$ otool -L .../aarch64-unix/ntdll.so
  @rpath/ntdll.so ; IOKit ; CoreFoundation ; CoreServices ; /usr/lib/libSystem.B.dylib
  (all standard frameworks, all present)
$ ls -la .../aarch64-windows/ntdll.dll → 7077888 (present)
```
So `c0000135` (STATUS_DLL_NOT_FOUND) here is NOT "file missing" and NOT "broken dylib dep". It is
an **internal resolution failure while bringing up the EC/x64 child loader** — the file loads,
but some required export/sibling/arch-view the EC child path expects is not resolved. The exact
reason is in the loader path (Codex/Opus lane), not in cpu.c.

## RED HERRINGS — ignore (all benign, NOT the blocker)
Also show `c0000135` but optional/unrelated: `msacm.imaadpcm / msadpcm / msg711 / l3acm /
msgsm610` (ACM audio codecs), `winegstreamer.dll` (media), `libgnutls` (encryption), FreeType
font lib missing, `SvchostPushServiceGlobals` ordinal. None stop the process.

## What this means for the lanes
- This is a **loader / EC-child-bringup** problem, NOT an `xtajit64/cpu.c` problem. Editing the
  emulator lifecycle (Part 2a's premise) is moot until the EC/x64 child loader comes up far enough
  to call `BeginSimulation`. The reach-proof is blocked one layer earlier than 2a assumed.
- Lane: **Codex/Opus** (loader path / EC child bringup). NOT Cline, NOT a cpu.c edit.
- Part 1 GREEN still holds (lld links arm64x, ntdll has the EC exports) — that was a *link/export*
  check. This is a *runtime loader* gap, a different layer. No contradiction.

## Process exit
Our `0x6502` marker did NOT fire (DLL never ran). Process died at the unix-ntdll load (c0000135).

## NEXT (Codex/Opus lane) — narrow, evidence-first
1. Re-run with `WINEDEBUG=+loaddll,+file,+relay,+seh` and capture the events **immediately before**
   line 16466 — what is the EC/x64 child actually trying to resolve when ntdll load returns
   c0000135? (export? a sibling like `wow64`/`xtajit64` arch-view? the x64 view of ntdll?)
2. Confirm whether the loader expects an `x86_64-windows` ntdll for a plain-x86_64 test PE that the
   arm64ec build does NOT produce (x64 is packed as arm64x inside `aarch64-windows`). If so, the
   plain-x64 PE may be taking a wow64 path that this build doesn't satisfy → choose a different
   test PE shape, or fix the loader's EC routing.
3. Only after the EC child loader comes up + xtajit64 is invoked can the Part 2a BeginSimulation
   reach-proof actually be observed.

## Process note
Cline got the right *neighborhood* (loader-side, not xtajit64-code) but published two false
specifics (xtajit64 "rejected"; aarch64-unix "has no ntdll") and hung before finalizing. Both
corrected here from the log + on-disk truth. Lesson stands: evidence, not status.
