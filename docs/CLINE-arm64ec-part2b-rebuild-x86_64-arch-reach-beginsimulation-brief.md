# CLINE master brief — ARM64EC 2b: add x86_64 arch, rebuild spike, re-run, reach BeginSimulation

**Agent:** Cline on Codex 5.5
**Date:** 2026-05-29
**Type:** Bounded build + runtime-proof. Bigger than the capture step, but still tightly scoped.
**Prereq / why:** Root cause of the `c0000135` is known
(`reports/research/ARM64EC-2A-ROOT-CAUSE-c0000135-no-x86_64-arch-20260529.md`): the spike Wine was
built `--enable-archs=arm64ec,aarch64,i386` (**no `x86_64`**), but the test PE `hello_x64.exe` is a
**plain x86_64 PE** that needs the WoW64 x64-guest ntdll (`x86_64-windows/ntdll.dll`) which this
build never produced. Add `x86_64`, rebuild, re-run, and prove how far we now get.
**Deliverable:** a pasted LOG LINE — either `BeginSimulation REACHED rip=…` (our marker fired) OR
the exact new furthest point. Evidence, not status.

---

## 0. ANTI-HANG RULES (READ FIRST — you froze twice on heredocs)

### 0.0 THE #1 REASON YOU FROZE: HEREDOCS / multi-line shell. (screen showed `cmdand ... heredoc c>`)
- **NEVER** use `cat > file <<EOF ... EOF`, `tee <<EOF`, or ANY `<<` heredoc in the terminal — the
  shell waits forever for the terminator and your task hangs at `cmdand heredoc>`.
- WRITE/EDIT every file (this report, any edit) with your **`write_to_file` / `replace_in_file`**
  tool — NEVER a shell heredoc.
- Every terminal command = **ONE single line**, no embedded newlines, no nested quotes.
- Multi-line git message → write it to a file with your tool, then `git commit -F <file>`.

### 0.1 Build / run rules
1. **Build in BACKGROUND to a logfile**, poll with `tail -n 80 <log>`. NEVER `tail -f`, never block.
2. **EVERY Wine / x64-PE run goes through `timeout`** (e.g. `timeout 90`). The x64 PE may hot-spin
   once emulation starts — that is EXPECTED; the timeout is mandatory, then scoped kill + read log.
3. **NEVER `cat`/`tail -f`/read a full trace log** (these reach millions of lines). Use
   `tail -n 200` / `grep` / `awk` windows only.
4. **Kill ONLY scoped:** `WINEPREFIX=<prefix> <dist>/bin/wineserver -k`. **NEVER** global
   `pkill -9 wine` / `killall wine` — Kimi runs Wine in parallel; you'd destroy his work.
5. If a command exceeds expected wall-clock with no log progress → kill it, capture last 80 lines,
   report. Never sit waiting.

---

## 1. SCOPE — what you MAY and MUST NOT touch
**MAY edit (only these):**
- `scripts/build-wine-arm64ec-spike.sh` — change ONE thing: the `--enable-archs` value (add
  `x86_64`). This is the spike script, NOT the baseline.
- The deliverable report file (via your tool).

**MUST NOT touch:**
- The **baseline** build script `scripts/build-wine-pure-arm64-experiment.sh` — untouched.
- `engine/wine/dlls/xtajit64/cpu.c` — leave the 2a lifecycle/logging code as-is (its MESSAGE logs
  + `0x6502` marker are exactly what we want to see fire now). Do NOT add HyperBridge calls.
- **HyperBridge core** (`hb_decode_x64.c`, `hb_lift_x64.c`, `hb_interpreter.c`, any `hb_*`).
- `dlls/ntdll/**`, `dlls/xtajit/**` (32-bit) — read-only reference.
- **`engine/graphics/**`** — Kimi's lane.
- No commits unless the operator says so.

## 2. THE ONE EDIT
In `scripts/build-wine-arm64ec-spike.sh`, line ~47, change:
```
--enable-archs=arm64ec,aarch64,i386 \
```
to:
```
--enable-archs=arm64ec,aarch64,x86_64,i386 \
```
(use your `replace_in_file` tool — NOT sed/heredoc). Change nothing else in the script.

## 3. BUILD (background, polled)
```
cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
WINEDEBUG=-all nohup bash scripts/build-wine-arm64ec-spike.sh > /tmp/arm64ec-2b-build.log 2>&1 &
```
- Poll with `tail -n 80 /tmp/arm64ec-2b-build.log` periodically (do NOT block / tail -f).
- Done when the log ends with the install line. Then check for failures (single-line):
  `grep -ciE "ld.lld: error| Error [0-9]|configure: error" /tmp/arm64ec-2b-build.log` → expect 0.
- If bison/other tool errors appear like before, report and stop (do not improvise deep fixes).
- **Confirm the new arch landed:**
  `ls -d engine/wine/dist-arm64ec-spike/lib/wine/x86_64-windows 2>&1` and
  `ls -la engine/wine/dist-arm64ec-spike/lib/wine/x86_64-windows/ntdll.dll 2>&1`
  → this directory + ntdll.dll MUST now exist (it did not before). If it still doesn't, stop and
  report — the build did not pick up x86_64.

## 4. RE-CREATE A CLEAN PREFIX + RUN (exact paths)
Use a FRESH dedicated prefix so old state can't mask the result:
```
rm -rf /tmp/wineprefix-arm64ec-2b
timeout 120 env WINEPREFIX=/tmp/wineprefix-arm64ec-2b WINEDEBUG=-all /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/bin/wine wineboot -i > /tmp/arm64ec-2b-boot.log 2>&1; echo "boot_exit=$?"
```
Then run the plain-x64 test with MESSAGE visible (MESSAGE prints regardless of WINEDEBUG):
```
timeout 120 env WINEPREFIX=/tmp/wineprefix-arm64ec-2b WINEDEBUG=-all /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/bin/wine /Users/timurtoby/Documents/MacRunner/Main/MacRunner/tests/native-fixtures/build/hello_x64.exe > /tmp/arm64ec-2b-run.log 2>&1; echo "run_exit=$?"
```
Scoped cleanup (single line):
```
WINEPREFIX=/tmp/wineprefix-arm64ec-2b /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/bin/wineserver -k
```

## 5. INSPECT (bounded greps only)
```
grep -niE "macrunner-xtajit64|BeginSimulation|REACHED|ProcessInit|ThreadInit|c0000135|failed to load" /tmp/arm64ec-2b-run.log | tail -n 40
```
Note `run_exit` — if it equals **25858** (0x6502) our BeginSimulation marker fired (success).
If c0000135 is gone but a new blocker appears, capture the 60 lines around it with `awk`.

## 6. DELIVERABLE — write with your tool (NOT heredoc), then STOP
Create `reports/research/ARM64EC-2B-rebuild-x86_64-result-20260529.md` containing:
- Did `x86_64-windows/ntdll.dll` get built now? (paste the `ls -la` line.)
- `boot_exit` and `run_exit` values.
- The **furthest runtime point**, with a PASTED log line:
  - best: `macrunner-xtajit64: BeginSimulation REACHED rip=… rsp=…` → paste it. PLUMBING WORKS.
  - or: ProcessInit/ThreadInit fired but not BeginSimulation → paste those.
  - or: a NEW blocker (paste the exact error + the 10 lines before it).
- One-sentence honest read of where we are.

⚠️ Do NOT report "BeginSimulation wired ✅" as status. The verdict is the pasted log line showing
what actually happened. "Reached ProcessInit but blocked at X" is a valid, useful result. STOP
after writing the file — no analysis beyond pasting evidence, no further edits, no commits.

## 7. OUT OF SCOPE (reserved for Codex/Opus)
- Editing `xtajit64/cpu.c` to actually EXECUTE x64 (wiring HyperBridge `hb_decode_x64`/`hb_lift_x64`/
  `hb_interpreter`) — that is the real emulator, NOT this task.
- Any change to HyperBridge core, ntdll, the 32-bit xtajit, or the baseline build.
- Chasing the `try_map_free_area mmap() Cannot allocate memory` VA-reservation warning if it
  appears — just report it; do not attempt VA-layout surgery.
