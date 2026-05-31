# MacRunner — standing rules for Claude (read every session)

MacRunner runs x86_64 Windows apps/games on Apple Silicon **without Rosetta**: HyperBridge
(x86→ARM64 translator) + pure-ARM64 Wine 11 + ARM64EC + DirectX11→Metal (DXMT). Multiple AI
agents work in parallel (Codex/Opus = engine, Kimi = graphics, Cline = bounded tasks, ChatGPT =
research). Claude here = strategic coordinator / memory-keeper.

## TOOLING — USE context-mode, NOT raw Bash+grep+Read (operator's explicit standing order)
For ANY heavy read/search/index work, route through the **context-mode MCP** (`ctx_search`,
`ctx_index`, `ctx_execute`, `ctx_stats`, etc.) — it exists for token economy. Do NOT default to
`grep`/`cat`/`tail`/`Read` over large logs or the repo. Specifically:
- Searching the repo / many files → `ctx_search` (not Bash `grep -r`).
- Reading/scanning large logs (Wine traces, build logs) → index + `ctx_search`, not `Read`/`tail`.
- Run `ctx_doctor`/`ctx_stats` if unsure context-mode is healthy; fall back to Bash ONLY if
  context-mode is genuinely unavailable, and say so explicitly.
This is a recurring miss — the operator has flagged it repeatedly. Honor it.

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

## VERDICT DISCIPLINE
Evidence (pasted log line / exported symbol / pixels), NOT agent status. "blocked at X" with the
exact blocker named is a valid, useful result — do not fake forward progress.
