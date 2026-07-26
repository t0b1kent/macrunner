# MacRunner — standing rules for Claude (read every session)

MacRunner runs x86_64 Windows apps/games on Apple Silicon **without Rosetta**: HyperBridge
(x86→ARM64 translator) + pure-ARM64 Wine 11 + ARM64EC + DirectX11→Metal (DXMT). Multiple AI
agents work in parallel (Codex/Opus = engine, Kimi = graphics, Cline = bounded tasks, ChatGPT =
research). Claude here = strategic coordinator / memory-keeper.

## ★ CURRENT STATE & LANE MAP — 2026-06-07 (supersedes the stale lane line above)
**MILESTONE:** SEH host-boundary `c0000026` (STATUS_INVALID_DISPOSITION) **PASSED** via bulk CFI/unwind
metadata on HyperBridge host-call thunks. Triage class progression on live HK x64 runs:
`SEH (pc 106A9F014→1095AF014) → GENERIC_ACCESS_VIOLATION → CREATE_DXGI_FACTORY_MISSING`. The HK window's
remaining path is ALL graphics. See Obsidian note 123 + `reports/research/MILESTONE-20260607-SEH-passed-graphics-critical-path.md`.

**ORDERING — GRAPHICS-FIRST (operator's call):** Lane A reached the CreateDXGIFactory wall → running it
again is pointless until Lane D delivers `CreateDXGIFactory → D3D11CreateDevice → swapchain → present`.
**Lane A stays PARKED until graphics is ready;** then ONE Lane A run reveals if graphics was the only wall.

**MODELS — Codex quota EXHAUSTED until ~Jun 12** (Spark + general account). Engine lanes run on Claude + Gemini:
- **Lane A** (x64 runtime→window; `macrunner_hb.c`/`signal_arm64.c`/`xtajit64`/hb_*): Claude **Opus** (operator terminal). PARKED pending graphics.
- **Lane D** (graphics/DXMT; `engine/dxmt|graphics|vkd3d`): Claude **Sonnet** — ACTIVE, **critical path #1**.
- **PE32** (32-bit; `xtajit`/`wow64`/`wow64cpu`/`wow64win`/deploy): Claude **Sonnet #2** — parallel (SEH unblocked it; i386 Diablo/Terraria already reach BTCpu).
- **Gemini (agy)** — proven CODE agent (native unarc macOS ARM64 / `___chkstk_darwin` fix). Repack CRC harness done (`tools/repack`); ISA-coverage tool done (`tools/hb_isa_coverage` + matrices). **NOW: Audio+Input lane** (`GEMINI-MEGA-PROGRAM-audio-input-lane.md`) — DSound/XAudio2/DInput/XInput → CoreAudio/GameController, arm64; owns those audio/input DLLs (disjoint, Lane C parked). NB: ISA coverage is ALREADY done (x64 matrix from Lane B Jun-1 + x86-32 + atlases) — remaining ISA = engine-lane *implementation* (PE32/Lane A), not re-coverage.
- **Lane C** (ntdll loader/server) — PARKED (real blocker was the SEH, not the loader machine-reject).
- **Triage analyzer** = `tools/triage/classify_run.py` (Lane X, Sonnet — hardened, generic-fallback).

**AUTOLOOP — ROOT CAUSE of every recurring "agent stopped again":** single-turn agent runtimes
(`codex exec`, `agy --print`, `claude -p`) RETURN at end-of-turn no matter what the prompt says — the
"don't stop / chain for months" instruction is advisory and CANNOT override the runtime. Any autonomous
lane MUST be wrapped in an autoloop from the start (and verify the loop is alive — ps the driver+agent).
Drivers (each reruns the agent in fresh turns that resume from the lane PROGRESS file, until it writes
`^LOOP-STATUS: GOAL|BLOCKED` at col 0):
- `scripts/lane-autoloop2.sh <lane> <progress> <max> <prompt> <model>` — **codex** (needs explicit
  `-m`; v2 = anchored `^LOOP-STATUS` grep so template text can't false-match).
- `scripts/lane-autoloop-agy.sh <lane> <progress> <max> <prompt> [model]` — **agy/Gemini** (`agy
  --print`, verified headless 2026-06-07; NB macOS bash 3.2 — no `${arr[@]}` under set -u).
- **Claude** lanes: Agent tool (background) + re-spawn on completion; `claude -p` is not authed headless.

**AUTO-TRIAGE:** `mr-run.sh` auto-runs classify_run + writes flight.jsonl INTO the run dir when the
caller sets `MACRUNNER_RUN_DIR=$RUNDIR` (opt-out `MACRUNNER_NO_AUTOTRIAGE=1`). Use it on every run.

**ctx caveat:** `ctx_batch_execute` shell mode dies here (spawn `/bin/zsh` ENOENT) → use `ctx_execute`
language=javascript instead.

**★ COORDINATOR RULE #1 (standing operator order, flagged 5×): ALWAYS read the lane ACTIVITY BEFORE
answering ANY status question.** Sources of truth, in order: (1) the agent's terminal output the
operator shows — that IS ground truth, trust it; (2) the lane's `LANE-*-PROGRESS.md` heartbeat; (3) the
relevant run's `triage-summary`. **NEVER assert "idle/stopped/done/almost-window" from absence of
commits / run-dirs / low CPU — those LAG badly** (agents churn 50+ min in GUI/worktree sessions,
uncommitted, run-in-progress). Do NOT contradict a live terminal with grep-derived state. If unsure,
ASK "what does your terminal show?" When killing a background agent I spawned, hunt its orphaned
ctx/bun/wine children (the "tails").

## ★ ULTRACODE POLICY — effort tier + workflow bursts (decided 2026-06-07)
"Ultracode" = two things; use each surgically, NOT blanket (cost-conscious):
- **Effort tier (max Opus):** ideal for the gnarly engine root-causing — **Lane A** (SEH/CFI/signal/
  tagged-PC/ARM64EC). BUT ultracode/Opus is gated behind the 1M-context credit requirement (operator
  avoids it) → **Lane A runs on Sonnet.** COMPENSATE the lost depth: the coordinator runs **Workflow
  bursts (model-independent — they don't hit the terminal's credit gate)** to do the deep diagnosis and
  hand Lane A precise, line-level fixes, then verify Lane A's work. **Workflow = the brains; the Sonnet
  terminal = the hands.** Everything else (PE32 mechanical, Audio plumbing, Gemini tooling, configurator)
  = high/standard regardless.
- **Workflow / multi-agent bursts (the Workflow tool):** fire at DECISION POINTS, not as the lane driver.
  Good triggers: (a) adversarially VERIFY a tricky engine fix BEFORE relying on it (the 2026-06-07 run
  found a CRITICAL per-thread concurrency bug + an unguarded unwind-twin that solo Lane A missed); (b)
  BULK SWEEP/audit a finite class against a reference (find all sibling bugs); (c) judge-panel of N
  approaches at a design fork. NOT for the months-long stateful lane loops (those stay autoloops/agents).
- **Cadence:** the continuous grind = the lanes (autoloops/terminals). Coordinator fires ~1 workflow per
  significant fix/milestone (verify-before-commit / sweep / pre-window audit). Each ≈5 agents / ~250k
  tokens / ~6 min — fire when stakes justify (window-blocking fix, bug class), skip for routine.

## TOOLING — USE context-mode, NOT raw Bash+grep+Read (operator's explicit standing order)
For ANY heavy read/search/index work, route through the **context-mode MCP** (`ctx_search`,
`ctx_index`, `ctx_execute`, `ctx_stats`, etc.) — it exists for token economy. Do NOT default to
`grep`/`cat`/`tail`/`Read` over large logs or the repo. Specifically:
- Searching the repo / many files → `ctx_search` (not Bash `grep -r`).
- Reading/scanning large logs (Wine traces, build logs) → index + `ctx_search`, not `Read`/`tail`.
- Run `ctx_doctor`/`ctx_stats` if unsure context-mode is healthy; fall back to Bash ONLY if
  context-mode is genuinely unavailable, and say so explicitly.
This is a recurring miss — the operator has flagged it repeatedly. Honor it.

**★ ENFORCED BY HOOK (2026-06-08):** a `PreToolUse(Bash)` hook (`.claude/settings.json` → `scripts/ctx-guard-hook.py`) **physically blocks** raw heavy reads/searches and tells you to use `ctx_search`/`ctx_execute` instead. BLOCKED: `cat|grep|tail|head|less|sed|awk` on `*.log`/`run.log`/`stderr.log`/`reports/*/run`; recursive `grep -r`/`-R`; `rg` as a command. NOT blocked: pipes (`ps|grep`, `git|grep`), single-file `grep file.c`, `make`/`mr-run.sh`/`ls`/`wc`/`stat`/redirects (`> run.log`), `Read` of small sources. If you get the deny, do NOT fight it — `ctx_index` then `ctx_search`. (Activates on session start; if not firing, open `/hooks` once or restart.)

## ★ BULK-OVER-REACTIVE — default reflex (operator's standing order 2026-05-31)
Whenever a failure class is driven by a **finite, externally-specified set** AND a **reference
implementation already exists**, cover the WHOLE set ONCE against that reference — never add one
item per failing run. Always also publish a coverage matrix so the gap is visible.
- Known bulk sets so far: **x86-64 opcodes** (ref = vendored `engine/wine/libs/capstone`),
  **JIT IR codegen** (163 ops in `hb_ir.h`, ref = the interpreter which implements all).
- When I (Claude) see Codex grinding one-item-per-run on something that has a finite list + a
  reference, I proactively flag it and convert it to a bulk task in the program + status — without
  waiting for the operator to notice. New such sets get the same treatment + a matrix.
- Does NOT apply to Win32-API behavior or memory/ABI correctness — no finite checklist, surface
  only by running real software → those stay reactive.
- Engine fundamentals (opcodes, JIT, ISA correctness) are built ONCE and reused by ALL games.
  Per-game work = Win32-API surface + graphics path + game-specific bugs, and it shrinks per
  ENGINE: first Unity game is costly, later Unity games are nearly free; cover by game-ENGINE
  (Unity → Unreal → GameMaker), not by individual title.

## HARD GUARDRAILS (never violate)
- **Lanes (updated 2026-05-30):** Kimi is sidelined; **Codex now owns everything incl. graphics**
  (`engine/dxmt/**`, `engine/graphics/**`, `engine/vkd3d/**`) + audio + engine. The old "never
  touch graphics — Kimi's lane" rule is LIFTED. Still: don't reach into Kimi's separate worktree if
  it exists; work in the canonical tree.
- **NEVER edit the golden x64 snapshot** (read-only oracle) — escalate instead.
- **Kill Wine ONLY scoped:** `WINEPREFIX=<prefix> <dist>/bin/wineserver -k`. NEVER global
  `pkill -9 wine` / `killall wine` — Kimi runs Wine in parallel; you'd destroy his work.
- **NEVER commit unless the operator explicitly asks.**
- **NEVER `git add -A` / `git add .`** — repo is massively untracked (Half-Life.7z, KeePass.exe,
  GTA, bottles/, artifacts/, secrets risk). Add only specific named files.
- **NEVER update git config** (despite the auto-config warning git prints on commit).
- Baseline build script `scripts/build-wine-pure-arm64-experiment.sh` is untouched. ARM64EC work
  uses `scripts/build-wine-arm64ec-spike.sh` → `build-arm64ec-spike` / `dist-arm64ec-spike`.

## RUNTIME DISCIPLINE
- **Use `scripts/mr-run.sh` for runs and `scripts/mr-clean.sh [--prune]` after each run/phase** —
  they guarantee no orphaned hot-spinning Wine and no leftover 1.5G prefixes (the recurring "tails"
  that load CPU + eat disk). Scoped to `dist-arm64ec-spike`; never global `pkill wine`.
- Every Wine / x86_64-PE run goes through `timeout` (e.g. `timeout 90 ...`) — x64 PEs under the
  half-wired emulator hot-spin/hang; the timeout is mandatory, then scoped `wineserver -k`.
- `WINEDEBUG=-all` unless a channel is deliberately needed; then scope narrowly.
- Never read full trace logs; bound output.

## AGENT NOTES
- **Cline (Codex 5.5) HANGS — and the #1 confirmed cause is HEREDOCS / multi-line shell.**
  Cline's shell wrapper freezes forever at `cmdand ... heredoc c>` waiting for a terminator.
  This is the actual freeze seen on screen, NOT a reading problem. So EVERY Cline brief §0 MUST
  forbid (loudly, never drop this rule again):
  - NO `cat > file <<EOF ... EOF`, NO `tee <<EOF`, NO any `<<` heredoc in the terminal.
  - Write/edit files ONLY with Cline's `write_to_file` / `replace_in_file` tools.
  - git commit multi-line message → write message to a file with the tool, then `git commit -F <file>`.
  - Every terminal command = a SINGLE line, no embedded newlines, no nested quotes.
  Other anti-hang rules still apply: background+logfile builds, timeout-wrapped runs, "when you
  have the answer STOP", no `tail -f`/full-log reads. Cline also reports a green checklist instead
  of the real verdict — demand pasted evidence, not status.
- Flash NOT for engine debugging; Opus default for engine.
- Obsidian vault at `/Users/timurtoby/Documents/MacRunner/` is NOT a git repo (notes just persist).

## DISK HYGIENE (added 2026-05-30 after 238 GB blowup)
Codex накопил 238 ГБ за 2 недели (79 копий wineprefix + 551 папка `reports/phase-h/*`).
Не повторять. **Перед длинным циклом / сборкой Wine / любым snapshot — вызови:**
```bash
./scripts/disk-guard.sh              # авточистка + проверка
./scripts/disk-guard.sh --check-only # только проверить
```
Exit 2 = меньше 30 GB после чистки → STOP, сказать юзеру. Полные правила в `AGENTS.md` → "Disk hygiene". Кратко:
- **NEVER `cp -r $WINEPREFIX`** без явного запроса юзера. Полный prefix = 1–2 ГБ × N прогонов = катастрофа.
- **Snapshots — rolling, max 3 последних.** Перед новым `artifacts/wineprefix-<tag>-*` удалить старые:
  `ls -dt artifacts/wineprefix-<tag>-* | tail -n +3 | xargs rm -rf`.
- **Reports в `reports/<phase>/latest/`, перезаписывать.** Версии с таймстемпом — только по явному запросу.
- **Перед длинными циклами (>10 итераций) — `df -g`.** Если свободно < 30 ГБ — STOP, сказать юзеру.
- **Запрещённые авто-имена:** `*-backup-*`, `*-snapshots*`, `*-offload-*`, `*codex-session-backup*`.
- **`reports/phase-h/` старше 7 дней — удалять перед новым прогоном:**
  `find reports/phase-h -maxdepth 1 -mindepth 1 -type d -mtime +7 -exec rm -rf {} +`
- **★ Осиротевшие `artifacts/_mr-run.*` префиксы (это укусило 2026-06-08).** mr-run.sh удаляет свой
  throwaway-префикс на выходе, НО убитый/упавший прогон оставляет его (накопилось 18 шт = 2.4 ГБ).
  Когда НЕТ живого прогона — снести: `rm -rf artifacts/_mr-run.*`.
- **★ `reports/` пухнет от run.log** (30+ МБ каждый × сотни прогонов; накопилось 1883 phase-h дир = 34 ГБ).
  Гонять с `MACRUNNER_RUN_DIR=reports/<phase>/latest` (ПЕРЕЗАПИСЬ, не новый timestamp-дир каждый раз),
  либо регулярно прунить: `find reports/phase-h reports/pe32 -maxdepth 1 -mindepth 1 -type d -mtime +1 -exec rm -rf {} +`.
- **★ ПАРИТЕТ С AGENTS.md (для ВСЕХ лайнов — Codex И Claude):** disk-hygiene правила в AGENTS.md
  («Disk hygiene (обязательно)») и здесь — ОДИНАКОВЫЕ. Перед длинным циклом — `disk-guard.sh`; после
  прогонов — чистить хвосты (orphan-префиксы + лишние run-диры); `cp -r $WINEPREFIX` запрещён; snapshots
  rolling max 3. Лайн, который копит на десятки ГБ — нарушает правило.

## VERDICT DISCIPLINE
Evidence (pasted log line / exported symbol / pixels), NOT agent status. "blocked at X" with the
exact blocker named is a valid, useful result — do not fake forward progress.

## ★ CHECK TERMINAL BUSY BEFORE EVERY PROMPT (operator flagged 4+ times, 2026-07-25/26)
**Before writing ANY prompt for a terminal, verify what that terminal is doing RIGHT NOW.**
Handing a prompt to a busy terminal either interrupts real work or the prompt sits unused
while status gets reported from a false assumption.

1. **Read the `LANE ACTIVITY` hook block** that arrives with every message — it is the
   designed source of truth (`scripts/lane-activity.sh` builds it from Claude.app/Codex
   session `.jsonl`). `cx 0м` / `main 0м` = working now; `cx 40м+` = idle.
   Do **NOT** use `LANE-*PROGRESS.md` for busy/idle — the script says they aren't updated by GUI lanes.
2. **Live run:** `ps -Ao pid,%cpu,etime,args | grep -iE 'mr-run|winetemp|extracted-hollow|wineserver'`.
   ⚠️ Never grep `'hollow knight'` with a space — the real path is `game-hollow.knight-(89718)`
   **with a dot**, so that pattern silently misses a running game.
3. **Writing a report = BUSY** even with no game process:
   `find reports -type f -newermt '<8 min ago>' | grep -v jsonl`.
4. **A report FILE appearing ≠ terminal finished** — it keeps working after writing the `.md`.
   The operator's terminal shows `Working (Nm Ns)`; a posted chat report or an idle hook entry
   means free.
5. **Verify staged artifacts actually landed** before telling anyone to run: a publish-monitor
   can die silently, leaving the old SHA in the live dist (happened 2026-07-26 with
   winemetal.so cf093248).

Only after these checks: name the specific free terminal and give it the prompt. If everything
is busy, say "all busy" — inventing filler analysis is worse than waiting (we already carry
30+ offline reports; the bottleneck is experiments, not hypotheses).
