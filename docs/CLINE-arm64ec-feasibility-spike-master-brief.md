# CLINE master brief — ARM64EC feasibility spike (Part 1: build + verify exports)

**Agent:** Cline on Codex 5.5
**Date:** 2026-05-29
**Type:** Bounded feasibility spike. NOT greenfield, NOT open-ended.
**Deliverable:** a yes/no answer to ONE architectural gate + evidence. Read to the end before touching anything.

---

## 0. ANTI-HANG RULES (READ FIRST — non-negotiable, you have a known tendency to freeze)

These exist because this repo has long builds and Wine processes that **hot-spin at 100% CPU and never exit**. If you block on one in the foreground, you hang. Obey all:

1. **NEVER run a long command in the foreground and wait.** Builds, `configure`, `make` → run in **background with a logfile**, then poll with `tail -n 80 <logfile>` every so often. Do NOT `tail -f`. Do NOT block.
2. **NEVER run Wine/any game binary without a hard `timeout`.** Always `timeout 120 <cmd>`. There is NO step in this brief that requires running a Windows binary — if you find yourself about to, STOP, you've gone off-script.
3. **NEVER `cat`/`tail -f`/read a full trace log.** They reach millions of lines. Use `tail -n 200` / `head -n 200` / `grep -c` only.
4. **Always export `WINEDEBUG=-all`** in any shell that might touch Wine, to avoid log floods.
5. **Kill ONLY scoped by prefix:** `WINEPREFIX=<prefix> wineserver -k`. **NEVER** global `pkill -9 wine` / `killall wine` — Kimi runs Wine in parallel and you would destroy his work.
6. **No interactive commands** (`git rebase -i`, `git add -i`, anything that opens an editor/pager). Pipe pagers to `cat` or use `--no-pager`.
7. If any single command runs longer than its expected wall-clock with no progress in the logfile, **kill it, capture the last 80 log lines, and report** — do not sit waiting.

---

## 1. THE PRODUCT CONTEXT (so you can make judgment calls)

MacRunner runs x86_64 Windows apps/games on Apple Silicon (ARM64) macOS **without Rosetta**. Stack: our own x86_64→ARM64 translator **HyperBridge** + a pure-ARM64-native fork of **Wine 11**.

We are pivoting toward the **ARM64EC** model: system DLLs (ntdll, CRT) compiled as hybrid PE that run **natively on ARM**, while only the game's x64 code is emulated by HyperBridge. Microsoft's reference emulator for this is `xtajit64.dll`; FEX uses `libarm64ecfex.dll`. Wine selects the emulator **by DLL name** (registry `HKLM\Software\Microsoft\Wow64\amd64`), so HyperBridge can take FEX's place — this is already confirmed by research.

**Why this spike exists:** all of ARM64EC research is DONE and GREEN (3 reports, see §6). Exactly ONE killing risk remains unverified by actually running it: **does OUR `lld` actually link an arm64ec hybrid PE?** Research says "yes, LLVM 21+ / llvm-mingw 22.x should" — but nobody has run our toolchain to confirm. Your job is to convert that "should" into "does / does not", with evidence.

---

## 2. THE SINGLE QUESTION YOU ARE ANSWERING

> If we add `arm64ec` to Wine's `--enable-archs` and build, **does our toolchain's `lld` successfully link the arm64ec hybrid PEs**, and does the resulting `x86_64-windows/ntdll.dll` export `__wine_unix_call_dispatcher_arm64ec` + `KiUserEmulationDispatcher`?

GREEN = the whole "own engine, no Rosetta" path is unblocked. RED = we escalate to operator (do NOT improvise an MSVC `link.exe`-via-Wine hack).

---

## 3. WHAT YOU MUST NOT DO

- **DO NOT modify or delete the working build script** `scripts/build-wine-pure-arm64-experiment.sh`. Create a NEW script; build into a NEW dist dir. The current working build must stay intact.
- **DO NOT touch `engine/graphics/**`** — that is Kimi's lane, hands off entirely.
- **DO NOT touch the golden x64 snapshot** (read-only oracle). If a change there seems needed, escalate.
- **DO NOT edit HyperBridge sources** (`engine/.../hb_decode_*.c`, `hb_lift_*.c`, `hb_interpreter.c`) in this spike — Codex/Opus is working there in parallel. Part 1 is build+verify only; no HyperBridge edits.
- **DO NOT commit anything** unless the operator explicitly says so. Leave changes staged/working; report.
- **DO NOT** start implementing `xtajit64/cpu.c` in Part 1. That is Part 2, gated on a GREEN Part 1 (see §7).
- If lld fails to link arm64ec: **STOP and escalate.** Do not try to wire in MSVC `link.exe`. The spike's job is the feasibility answer, not a workaround.

---

## 4. GROUND TRUTH ALREADY ESTABLISHED (don't re-derive)

- **Smoking gun:** `scripts/build-wine-pure-arm64-experiment.sh:47` →
  `--enable-archs=aarch64,x86_64,i386` — **no `arm64ec`**. That's why
  `x86_64-windows/ntdll.dll` lacks `__wine_unix_call_dispatcher_arm64ec` + `KiUserEmulationDispatcher`,
  which Wine's loader hardcodes as required for x86_64 PE on an ARM64 host.
- **Toolchain CAN target arm64ec:** `engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/`
  contains `arm64ec-w64-mingw32-clang++`, `arm64ec-w64-mingw32-strip`, etc.
- **Config per research:** `--enable-archs=arm64ec,aarch64,i386 --with-mingw=clang --disable-tests`.
- **Skeleton already in tree:** `engine/wine/dlls/xtajit64/cpu.c` (250 lines) + `xtajit64.spec`
  already export the right interface (`BeginSimulation`, `ProcessInit`, `ThreadInit`, `DispatchJump`,
  `RetToEntryThunk`, `ExitToX64`, `BTCpu64*`, …) — but cpu.c bodies are STUBS (`ERR("x64 emulation
  not implemented")`). `engine/wine/dlls/ntdll/signal_arm64ec.c` also already present.

---

## 5. EXACT STEPS — Part 1 (the deliverable)

Work from repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

**Step 1 — read the originals (no edits yet).**
- Read `scripts/build-wine-pure-arm64-experiment.sh` fully. Note: the output dist dir, the
  `--enable-archs` line, the `--with-mingw` setting, the configure invocation, and any env that
  points at the llvm-mingw toolchain.

**Step 2 — create a NEW build script** `scripts/build-wine-arm64ec-spike.sh` as a copy of the
  working one with exactly these changes:
- `--enable-archs=arm64ec,aarch64,i386` (was `aarch64,x86_64,i386`).
- Ensure `--with-mingw=clang` (or whatever invokes the llvm-mingw clang) is present.
- Add `--disable-tests`.
- Output to a **separate** build/dist dir (e.g. `engine/wine/build-arm64ec-spike` and
  `dist-arm64ec-spike`) so the working build is untouched.
- The build command itself MUST be runnable in background with a logfile. If the script runs
  configure+make inline, that's fine — YOU invoke the script in background (Step 4), don't make
  the script block-and-wait on anything interactive.

**Step 3 — run `configure` only first (cheap gate).** If the script separates configure from make,
  run configure, capture output to a log, and verify it accepts `arm64ec` without erroring. If it
  errors at configure on the arch, that's an early useful result — report it. (configure is short;
  still capture to a logfile and read with `tail -n 120`.)

**Step 4 — build, in BACKGROUND with a logfile.** Example shape (adapt to the real script):
```bash
cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
WINEDEBUG=-all nohup bash scripts/build-wine-arm64ec-spike.sh > /tmp/arm64ec-spike-build.log 2>&1 &
echo "build pid $!"
```
Then **poll** (do not block): every so often `tail -n 80 /tmp/arm64ec-spike-build.log` and
`grep -ciE 'error:|undefined symbol|cannot|fatal' /tmp/arm64ec-spike-build.log`.
You only need ntdll to link to answer the question — if a full build is slow, it's fine to let it
run to ntdll and inspect, but don't sit in a blocking wait; poll.

**Step 5 — THE lld GATE.** Watch specifically for arm64ec **link** failures (the real risk):
  `lld` errors like unsupported relocation, `unknown machine type`, hybrid/`arm64x` link errors,
  or "use link.exe". Capture the exact error text if any. This is the single most important signal.

**Step 6 — verify exports** (only if ntdll linked). Find the built `x86_64-windows/ntdll.dll` under
  the new dist/build dir and run (paths adapt):
```bash
NM=engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/llvm-nm
OBJDUMP=engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/llvm-objdump
"$OBJDUMP" -p <path>/x86_64-windows/ntdll.dll | grep -iE 'wine_unix_call_dispatcher_arm64ec|KiUserEmulationDispatcher'
```
  (Use whichever of llvm-nm/llvm-objdump shows PE exports; for a DLL export table, `llvm-objdump -p`
  or `llvm-readobj --coff-exports` is most reliable. Try `llvm-readobj --coff-exports` too.)
- Also confirm `xtajit64.dll` itself built/linked (it's an arm64ec target).

**Step 7 — REPORT (the actual deliverable).** Write findings to
  `reports/research/ARM64EC-SPIKE-build-result-20260529.md` AND give the operator a tight summary:
  - GREEN / RED on the lld link.
  - Whether the two exports are present (paste the grep output).
  - Whether `xtajit64.dll` linked.
  - If RED: the exact error text + the failing command. Do NOT attempt a workaround.

---

## 6. RESEARCH ALREADY DONE — read these, don't redo research

- `docs/ARM64EC-FINDINGS-and-next-spike.md` — the master findings (root cause, plan, both gates GREEN in theory, xtajit64 contract). **Read this first**, it frames everything.
- `reports/research/ARM64EC-NTDLL-RESEARCH-chatgpt-20260529.md`
- `reports/research/ARM64EC-EMULATOR-CONTRACT-chatgpt-20260529.md` (HyperBridge CAN replace FEX; lld SHOULD link arm64ec)
- `reports/research/ARM64EC-XTAJIT64-IMPL-chatgpt-20260529.md` (the BeginSimulation/BTCpu64 implementation contract for Part 2)

---

## 7. PART 2 (only if Part 1 is GREEN — do NOT start without operator go-ahead)

Wire the stubs in `engine/wine/dlls/xtajit64/cpu.c` to route into HyperBridge x64, mirroring the
working 32-bit reference `engine/wine/dlls/xtajit/cpu.c`. Mandatory exports per research:
`ProcessInit`, `ThreadInit`, `BeginSimulation` (the core — context comes from
`get_arm64ec_cpu_area()->ContextAmd64`; run interp until an ARM64 address, then exit saving context),
`DispatchJump`/`RetToEntryThunk`/`ExitToX64` (x64↔ARM64 transition thunks via `x9` + `RtlIsEcCode`),
`UpdateProcessorInformation`, `BTCpu64IsProcessorFeaturePresent`. Stub-OK for first build:
`ThreadTerm`, `ProcessTerm`, all `Notify*`, `FlushInstructionCache*`, `ResetToConsistentState`.
**Key difference from 32-bit:** no BOP code in ARM64EC-x64 — entry is via BRK exception →
`KiUserEmulationDispatcher` → `BeginSimulation` + thunks, not via `BTCpuGetBopCode`.

But again: **Part 2 is gated. Finish and report Part 1 first.**

---

## 8. DEFINITION OF DONE (Part 1)

A written report + operator summary that answers: did our lld link arm64ec? are the two ntdll
exports present? did xtajit64.dll link? — each backed by pasted command output. No HyperBridge edits,
no commits, working build script untouched. That's it. Don't scope-creep into Part 2 or into running
binaries.
