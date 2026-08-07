# Lane PE32 / WOW64 — MISSION (handoff brief, 2026-06-06)

**Model:** Claude Opus (thinking) — engine-debug default. (Codex xhigh acceptable. **NOT Gemini/Flash.**)
**Terminal:** dedicated T2 (separate from Lane A / Codex).
**Working copy:** `/Users/timurtoby/Documents/MacRunner/Main/MacRunner` (canonical, local SSD).
**Read first:** `docs/CONTINUE-PE32-qsi-class102-hotspin-master-brief.md`, `docs/ACTIVE-INVESTIGATION.md`,
`AGENTS.md` (x64 golden snapshot = read-only oracle; scoped wine kill only; family-audit before
opcode work; append `docs/ENGINE-CHANGE-JOURNAL.md` per edit).

## ZEROTH RULE (verdict discipline)
Verdict = a REAL on-screen Notepad++ x86 window (CoreGraphics capture). NOT "process alive",
NOT "2.2M-line trace ran". A prior agent mistook a long syscall trace for "launched" — there was
no window. Do not report done until the window is on screen OR you have a pasted failing call.

## FILE OWNERSHIP (hard boundaries)
- **YOURS:** `engine/wine/dlls/xtajit/**` (32-bit module — **NOT** `xtajit64`), `engine/wine/dlls/wow64`,
  `wow64cpu`, `wow64win`, and the i386 WOW64 mirror bits you already touched (`gdi32/gdiobj.c`
  peb32 mirror).
- **NEVER touch (Lane A):** `xtajit64/**`, `engine/hyperbridge/**` (`hb_*`), `macrunner_hb.c`,
  `signal_arm64.c`. If you need a change there, write it to `reports/research/PE32-NEEDS.md` and
  ping the Coordinator — do NOT edit (merge collision with the active critical path).
- **NEVER touch (Lane B):** `hb_decode_*`, `hb_lift_*`, `hb_interpreter.c`. This is NOT an ISA task.

## CURRENT GATE — `PE32_WOW64CPU_NOT_LOADED` (Lane X conf 0.95, PATCH_SAFE_NOW=yes)
PE32 Notepad++ loads and lives — **no** crash, **no** unsupported opcode, **no** MEMORY_FAULT,
**no** c0000005 — BUT it never reaches `BTCpuProcessInit` / `BTCpuThreadInit` / `BTCpuSimulate`.
The i386 CPU **never starts**. The break is BEFORE CPU execution: the **wow64cpu / xtajit /
i386-ntdll handoff** never wires up BTCpu. Latest run: `reports/phase-h/npp-x86-diagnostic-20260606-150550`.

**Do NOT chase opcodes** — no x86 instruction can run until BTCpu is initialized. The bug is in the
loader/handoff that is supposed to map `xtajit.dll` (BTCpu provider) into the WOW64 process and call
its `BTCpuProcessInit`.

## TASK (stepwise, evidence-first)
1. **Trace the BTCpu provider load + handoff.** Find where WOW64 is supposed to (a) locate the CPU
   backend DLL (`xtajit.dll`), (b) resolve its `BTCpu*` exports (`xtajit.spec`), (c) call
   `BTCpuProcessInit`. Instrument with the existing xtajit traces
   (`MACRUNNER_XTAJIT_TRACE_SYSCALLS=1`, `MACRUNNER_XTAJIT_TRACE_STACK=1` in `xtajit/cpu.c`) plus a
   one-shot marker at each handoff step. Capture: does `wow64cpu` load? does it find `xtajit.dll`?
   are BTCpu exports resolved? is `BTCpuProcessInit` ever called? — pasted log lines for each.
2. **Classify the break** (one of):
   - (a) WOW64 never selects/loads the CPU backend (registry `Wow64\\x86\\...` / engine config /
     `config/engines.json` lane env not setting the BTCpu DLL path).
   - (b) `xtajit.dll` loads but BTCpu exports don't resolve (spec/forwarder/arch mismatch — i386 PE
     vs ntdll expectations).
   - (c) handoff reached but `BTCpuProcessInit` early-returns / faults before arming the simulator.
3. **Fix at the root in your files** (loader/handoff/spec), rebuild the i386 + wow64 pieces, re-run.
4. **Gate:** log shows `BTCpuProcessInit` → `BTCpuThreadInit` → `BTCpuSimulate` reached. Then push
   toward the GUI (CreateWindow / NtUser*). Final verdict = window on screen.

## REPRO (clean, 1 command)
```bash
cd "/Users/timurtoby/Documents/MacRunner/Main/MacRunner"
bash reports/phase-h/run_window_clean.sh > /tmp/npp32.log 2>&1 &
# ~20s later: ps -o pcpu -p <npp pid>; tail /tmp/npp32.log
python3 tools/triage/classify_run.py <run-dir>          # MANDATORY before next step
# scoped kill ONLY:
WINEPREFIX="$PWD/bottles/generic-x86" engine/wine/dist-pure-arm64/bin/wineserver -k
```

## HYGIENE
- Scoped wine kill only (`WINEPREFIX=… wineserver -k`), never global `pkill` (kills Lane A's run).
- After every run: `python3 tools/triage/classify_run.py <run-dir>` (Lane X gate) before iterating.
- Append `docs/ENGINE-CHANGE-JOURNAL.md` on each edit. Self-commit named files only.

## WHY THIS LANE MATTERS (downstream)
PE32/WOW64 reaching `BTCpuSimulate` unblocks Lane F (Diablo 1 DDraw, GTA VC D3D8/RenderWare) and
Lane G (installers / FreeArc / lolz repacks) — all of which are 32-bit and currently prep-only.
This is the single gate in front of the entire legacy-32-bit + installer half of the product.
