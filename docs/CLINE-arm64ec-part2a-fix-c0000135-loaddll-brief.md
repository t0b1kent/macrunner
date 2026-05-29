# CLINE mini-brief — ARM64EC Part 2a-FIX: name the module behind c0000135

**Agent:** Cline on Codex 5.5
**Date:** 2026-05-29
**Type:** Tiny diagnostic step. NOT implementation. One question, one log line.
**Prereq:** Part 2a run produced `c0000135` (STATUS_DLL_NOT_FOUND) at `ntdll.dll` load,
BEFORE our emulator handoff — markers (`0x6502`, `BeginSimulation REACHED`) never printed.
**Deliverable:** the **exact module name** that fails with `c0000135`, pasted from a loader log.

---

## 0. ANTI-HANG RULES (READ FIRST — you froze last time)
1. **Do NOT run anything in foreground and wait.** Every Wine run goes through `timeout 60`.
2. **After `timeout` fires, kill SCOPED only:** `WINEPREFIX=<prefix> <spike-dist>/bin/wineserver -k`.
   NEVER global `pkill wine` / `killall wine` (Kimi runs Wine in parallel — you'd destroy his work).
3. **NEVER `cat`/`tail -f` a full log.** Use `tail -n 120` / `grep`.
4. **When you have the answer, STOP.** Write the result file, end your turn. Do NOT keep going,
   do NOT rebuild, do NOT start fixing. This step ENDS at naming the module.

---

## 1. THE ONE QUESTION
`c0000135` = STATUS_DLL_NOT_FOUND. Something the x64 test PE depends on is not found, so the
loader dies BEFORE control reaches our `xtajit64.dll`. **Which module?** That's all we need.

Two likely causes (the log will tell us which):
- the spike **prefix** points at the wrong dist (old prefix, no arm64ec/wow64 emulator
  registration, x86_64 `ntdll`/CRT not found), or
- the test `.exe` has a missing dependency DLL (CRT / `xtajit64` / x86_64 `ntdll`).

## 2. WHAT TO DO (exactly)
Re-run the SAME test from Part 2a, but with the loader channel on (narrowly scoped) so we see
which DLL load fails. Reuse the same dedicated spike prefix and spike dist as before.

```bash
cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
timeout 60 env WINEPREFIX=<spike-prefix> WINEDEBUG=+loaddll,+module \
    <spike-dist>/bin/wine <same-x64-test>.exe > /tmp/arm64ec-2a-loaddll.log 2>&1; echo "exit=$?"
WINEPREFIX=<spike-prefix> <spike-dist>/bin/wineserver -k   # scoped cleanup
```

Then find the failing module:
```bash
grep -niE "c0000135|not found|cannot find|failed to load|Loading|ntdll|xtajit64" \
    /tmp/arm64ec-2a-loaddll.log | tail -n 40
```

If `+loaddll` is too quiet, the last successful `Loading ...` line before the `c0000135`
error names the dependency that follows. Paste a few lines of context around the failure.

## 3. ALSO CONFIRM (one-liners, no rebuild)
- Is the spike prefix actually pointing at the spike dist? Show it:
  `ls -la <spike-prefix>/drive_c/windows/system32/xtajit64.dll 2>&1` (present or not).
- Is `xtajit64.dll` in the dist at all? `find <spike-dist> -name xtajit64.dll`.

## 4. DELIVERABLE (this is "done")
Append to `reports/research/ARM64EC-2A-begin-simulation-result-20260529.md` a section
**"c0000135 root cause"** with:
- the **exact failing module name** (e.g. `Failed to load <X>.dll → c0000135`) — PASTED log line.
- whether `xtajit64.dll` is present in the prefix and in the dist (the two checks above).
- your one-sentence read: is this a prefix/registration problem or a missing dependency?

⚠️ Evidence = the pasted log line naming the module. NOT a status checklist. Then STOP.
Do not attempt the fix — that's the next brief.
