# CLINE mini-brief — ARM64EC 2a-FIX2: capture the events BEFORE the unix-ntdll c0000135

**Agent:** Cline on Codex 5.5
**Type:** PURE CAPTURE. NOT analysis, NOT a fix, NOT a cpu.c edit. Grab a trace window, STOP.
**Why:** Runtime dies at the very last line with
`wine: failed to load .../aarch64-unix/ntdll.dll error c0000135`. The file EXISTS and its deps
resolve, so this is an *internal* resolution failure while bringing up the EC/x64 child. We need
the events **immediately before** that line to see what the child loader is actually resolving.
Operator does the interpretation — your job is only to capture and paste.

## 0. ANTI-HANG (READ — you hung twice already)

### 0.0 THE #1 REASON YOU FROZE LAST TIME: HEREDOCS. (the screen showed `cmdand ... heredoc c>`)
- **NEVER** use `cat > file <<EOF ... EOF`, `tee <<EOF`, or ANY `<<` heredoc in the terminal. The
  shell waits for the terminator forever and your task hangs at `cmdand heredoc>`. This is exactly
  what froze you.
- To WRITE/EDIT the deliverable `.md`, use your **`write_to_file` / `replace_in_file` tool** —
  NOT a shell heredoc. Paste the log blocks into the file via that tool.
- Every terminal command = **ONE single line**, no embedded newlines, no nested quotes.
- If you ever need a multi-line git message, write it to a file with your tool, then `git commit -F <file>`.

### 0.1 Other rules
1. Build NOT needed. Do NOT rebuild.
2. The Wine run goes through `timeout 60`. After it fires: scoped kill
   `WINEPREFIX=<spike-prefix> <spike-dist>/bin/wineserver -k`. NEVER global `pkill`/`killall wine`.
3. **NEVER `cat`/`tail -f`/Read the full trace** (it is ~3 MB / 16k+ lines). Only the bounded
   greps below.
4. **When the capture file is written, STOP immediately.** Do NOT analyze, do NOT hypothesize,
   do NOT edit any source. End your turn. (Last time you published two wrong root-causes — this
   time: capture only, zero interpretation.)

## 1. THE RUN (same prefix, same x64 test as before)
```bash
cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
timeout 60 env WINEPREFIX=<spike-prefix> WINEDEBUG=+loaddll,+file,+seh \
    <spike-dist>/bin/wine <same-x64-test>.exe > /tmp/arm64ec-2a-ntdll-ctx.log 2>&1; echo "exit=$?"
WINEPREFIX=<spike-prefix> <spike-dist>/bin/wineserver -k
```

## 2. THE CAPTURE (bounded — no full reads)
```bash
# the fatal line number:
FATAL=$(grep -nm1 "failed to load.*aarch64-unix/ntdll.dll error c0000135" /tmp/arm64ec-2a-ntdll-ctx.log | cut -d: -f1)
echo "fatal at line: $FATAL"
# the 80 lines immediately BEFORE it (this is the evidence we need):
awk -v F="$FATAL" 'NR>=F-80 && NR<=F' /tmp/arm64ec-2a-ntdll-ctx.log > /tmp/arm64ec-2a-ntdll-window.txt
wc -l /tmp/arm64ec-2a-ntdll-window.txt
# anything about wow64 / x86_64 / EC child / arch around the fatal:
grep -niE "wow64|x86_64|arm64ec|EC |emulat|machine=|loader|create.*process" /tmp/arm64ec-2a-ntdll-ctx.log | tail -n 40 > /tmp/arm64ec-2a-ntdll-grep.txt
```

## 3. DELIVERABLE (then STOP)
Paste into a NEW file `reports/research/ARM64EC-2A-ntdll-c0000135-context-20260529.md`:
- the value of `$FATAL` and `exit=` code,
- the **contents of `/tmp/arm64ec-2a-ntdll-window.txt`** verbatim (the 80 lines before the fatal),
- the **contents of `/tmp/arm64ec-2a-ntdll-grep.txt`** verbatim.
No commentary, no conclusion, no "verdict ✅". Just the two pasted blocks + the two numbers.
Then end your turn. Operator analyzes.
