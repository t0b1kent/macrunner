# CODEX ADDENDUM — also own the 2b build + run (Cline is out)

**Agent:** Codex / Opus (engine lane)
**Date:** 2026-05-29
**Why:** Cline died mid-2b. You already wrote the xtajit64 execution wire
(`engine/wine/dlls/xtajit64/{cpu.c,unixlib.c,xtajit64_private.h,Makefile.in}` — reviewed, looks
right). Now you also do the build+run that was Cline's 2b, so your wire can actually execute.
**Parent brief:** `docs/CODEX-arm64ec-xtajit64-wire-hyperbridge-x64-execution-master-brief.md`.
**Deliverable:** pasted log showing the furthest runtime point + a result file.

---

## 0.1 MCP 120s CEILING — why you keep hitting "timed out awaiting tools/call after 120s"
You run commands through context-mode's MCP tools (`ctx_execute`/`ctx_batch_execute`/
`ctx_execute_file`). **Each MCP tool-call has a hard ~120s ceiling** (no per-call timeout knob in
`~/.codex/config.toml`). A full Wine build takes MINUTES and a hot-spinning x64 run never returns,
so running either synchronously in ONE ctx_execute ALWAYS times out — and the underlying process
keeps running detached (leftover Wine). This is not a context-mode bug; it is long work in one
blocking call. Structure every call to return in well under 120s:
- **Build → background + poll.** Start it in ONE quick call: `nohup bash scripts/... > /tmp/log
  2>&1 &` (returns instantly). Then poll in SEPARATE short calls: `tail -n 80 /tmp/log`. Never put
  the build itself in a call that waits for completion.
- **Wine run → `timeout 90` + redirect to a file** (so the call self-terminates inside the budget
  and returns a tiny payload). On timeout, scoped `wineserver -k` — never leave it detached.
- Never stream MB of trace stdout back through a ctx_execute; redirect to a file, then grep a
  bounded window.

## 0. THE BUILD-ARCH ROOT CAUSE (already proven)
`reports/research/ARM64EC-2A-ROOT-CAUSE-c0000135-no-x86_64-arch-20260529.md`: the spike build had
`--enable-archs=arm64ec,aarch64,i386` (no `x86_64`); a plain-x64 PE needs the WoW64 x64-guest
ntdll (`x86_64-windows/ntdll.dll`) which that build never produced → `c0000135` before
`BeginSimulation`. Fix = add `x86_64` to the archs, rebuild, then your wire is reachable.

## 1. THE ONE BUILD EDIT
In `scripts/build-wine-arm64ec-spike.sh` (line ~47), change
`--enable-archs=arm64ec,aarch64,i386` → `--enable-archs=arm64ec,aarch64,x86_64,i386`.
Nothing else in the script. Baseline `build-wine-pure-arm64-experiment.sh` stays untouched.

## 2. BUILD (background, logged)
```
cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
WINEDEBUG=-all nohup bash scripts/build-wine-arm64ec-spike.sh > /tmp/arm64ec-2b-build.log 2>&1 &
```
- Poll `tail -n 80 /tmp/arm64ec-2b-build.log`; do not block.
- Fail check: `grep -ciE "ld.lld: error| Error [0-9]|configure: error" /tmp/arm64ec-2b-build.log` → 0.
- **Confirm x86_64 landed:**
  `ls -la engine/wine/dist-arm64ec-spike/lib/wine/x86_64-windows/ntdll.dll` — MUST exist now.
- **Watch item from the wire review:** if compile fails in `dlls/xtajit64/cpu.c` on `src->Rax`/
  `src->Rip` (i.e. `CONTEXT` resolving to the ARM64 layout, not amd64, in that TU), that's the one
  typing risk I flagged — fix the pack/unpack to use the amd64 context type explicitly
  (`ARM64EC_NT_CONTEXT`/`AMD64_CONTEXT`) rather than `CONTEXT`. (`ContextAmd64` is
  `ARM64EC_NT_CONTEXT *`; `.AMD64_Context` is `AMD64_CONTEXT` — verified in winternl.h:357 /
  winnt.h:1996. `NtContinue(&...AMD64_Context, FALSE)` matches Wine's own signal_arm64ec.c:1331.)

## 3. RUN (fresh scoped prefix, timeout, MESSAGE visible)
```
rm -rf /tmp/wineprefix-arm64ec-2b
timeout 120 env WINEPREFIX=/tmp/wineprefix-arm64ec-2b WINEDEBUG=-all engine/wine/dist-arm64ec-spike/bin/wine wineboot -i > /tmp/arm64ec-2b-boot.log 2>&1; echo "boot_exit=$?"
timeout 120 env WINEPREFIX=/tmp/wineprefix-arm64ec-2b WINEDEBUG=-all engine/wine/dist-arm64ec-spike/bin/wine tests/native-fixtures/build/hello_x64.exe > /tmp/arm64ec-2b-run.log 2>&1; echo "run_exit=$?"
WINEPREFIX=/tmp/wineprefix-arm64ec-2b engine/wine/dist-arm64ec-spike/bin/wineserver -k
```
- x64 may hot-spin → `timeout` mandatory; kill **scoped** only (never global `pkill wine` — Kimi
  runs Wine in parallel).
- MESSAGE prints regardless of WINEDEBUG, so your `macrunner-xtajit64:` lines will show.

## 4. INSPECT
```
grep -niE "macrunner-xtajit64|BeginSimulation|REACHED|ProcessInit|ThreadInit|simulate|DispatchJump|RetToEntryThunk|ExitToX64|c0000135|steps=" /tmp/arm64ec-2b-run.log | tail -n 60
```
Expected ladder (each step is progress):
1. `ProcessInit`/`ThreadInit status=…` → emulator lifecycle up.
2. `BeginSimulation REACHED rip=… rsp=…` → **plumbing works end-to-end** (the milestone).
3. `macrunner-xtajit64-unix: phase=enter …` then `… steps=N blocks=M` with **N>0** → real x64
   executed.
4. Likely then a transition thunk fires (`DispatchJump`/`RetToEntryThunk`/`ExitToX64`, exit
   0x6503/4/5) at the first call into an ARM64 EC function — those are still stubbed to kill.
   **Reaching a thunk = success for this stage**; wiring the thunks is the next piece, not now.
   Or a `HB_ERR_UNSUPPORTED_OPCODE` at a specific RIP → feeds the opcode-coverage campaign.

## 5. DELIVERABLE
`reports/research/ARM64EC-2B-execution-result-20260529.md` with:
- did `x86_64-windows/ntdll.dll` build? (`ls -la` line)
- `boot_exit`, `run_exit`
- the **furthest point** on the ladder above, with PASTED log lines (BeginSimulation REACHED?
  steps=N? which thunk / which opcode+RIP?)
- one honest sentence on where we are.
Evidence, not status. "Reached execution, N steps, died at thunk X / opcode Y @ RIP Z" is the win.

## 6. UNCHANGED GUARDRAILS
Don't touch baseline build script, golden snapshot, `engine/graphics/**`, `signal_arm64ec.c`
(beyond reading). No commit unless operator asks. You may edit HyperBridge + xtajit64 per the
parent brief.
