# MacRunner — standing rules for Claude (read every session)

MacRunner runs x86_64 Windows apps/games on Apple Silicon **without Rosetta**: HyperBridge
(x86→ARM64 translator) + pure-ARM64 Wine 11 + ARM64EC + DirectX11→Metal (DXMT). Multiple AI
agents work in parallel (Codex/Opus = engine, Kimi = graphics, Cline = bounded tasks, ChatGPT =
research). Claude here = strategic coordinator / memory-keeper.

## ★★★ ГЛАВНОЕ ПРАВИЛО — КОПИЯ ПЕРЕД ПЕРЕЗАПИСЬЮ (оператор ловил ДВАЖДЫ)

**Ни один рабочий двоичный файл не перезаписывается без копии рядом.** Не «если важно»,
не «если не забуду» — ВСЕГДА, до `make install` / `cp` в дист / `build.sh` в тот же путь:

```bash
cp -p <файл> <файл>.БЫЛО-$(date +%H%M%S)
```

**Два случая, оба потеря невосстановима:**

1. **28.07** — дист, на котором взято главное меню Hollow Knight, не сохранён.
   Меню с тех пор не воспроизводилось НИ РАЗУ. Оператор предупреждал заранее.
2. **04.08** — `mono-profiler-hk_language.dll` (`45a0bef1d2b6`), при котором на экране
   был курсор игры, перезаписан своей пересборкой. Не под гитом. Пересобрать не вышло:
   точного среза исходника уже нет — его правил другой лейн в течение дня.

**Знать список правок НЕДОСТАТОЧНО.** Двоичный файл по списку изменений не
восстанавливается: нужен либо сам файл, либо точный срез исходника со ВСЕМИ чужими
правками того момента.

**Что из этого следует:**

- Этаж (`dist-MMDD-floorNN`) снимать с **ОБОИХ** дистов: прогон запускается из
  `dist-arm64ec-spike/bin/wine`, а `ntdll.so` грузит из `engine/wine/dist`.
  На одном дисте floor19 оказался неполным.
- В этаж класть и то, чего нет в гите: наблюдатель, сценарий прогона, окружение
  (БЕЗ учётных данных из `final-child.json`).
- Файлы вне гита — копия единственная защита; `git checkout` для них не работает.
- Артефакт вот-вот перезапишет чужая сборка (лейн, автолуп) — СНАЧАЛА копия.

Память: `copy-before-overwrite-any-working-artifact`.


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

## ★ ВЕХУ СОХРАНЯЕМ DIST-ОМ, А НЕ КОММИТОМ (операторское правило, 2026-08-01)

**Коммит НЕ восстанавливает рабочее состояние. Восстанавливает только сохранённый `dist`.**
Доказано в один час 2026-08-01, на двух путях к одной и той же цели:

- Коммит рекордного прогона (`05f3f3f9`, 29.07) известен точно — и откат к нему **не собрался**:
  старый `engine/hyperbridge` против HEAD-ового остального дерева несовместим. Чтобы поднять,
  надо согласованно откатывать всё дерево и пересобирать wine + DXMT — часы с неясным исходом.
- Сохранённый этаж `hk-rung13-rtv-BREAKTHROUGH-20260706` (`/Volumes/MacOS 1/MacRunner-ARM64EC-floors/`)
  запустился **без единой сборки** и вышел на swapchain за 135 с.

Причина: работает не исходник, а собранный артефакт вместе с окружением — конфигурация сборки,
версии зависимостей, состояние `dist`. В коммите этого нет. Поэтому в манифестах этажей и стоит
пометка **zero-drift**: предпочтителен сам dist, а не «воспроизведение из коммита».

**Правило:** как только прогон берёт НОВУЮ ступень лестницы — немедленно класть этаж в
`/Volumes/MacOS 1/MacRunner-ARM64EC-floors/<тег>-<дата>/` с `MANIFEST.md` (что достигнуто, чем
подтверждено, что НЕ достигнуто, гипотеза о следующем шаге) + `dist/` + `evidence/` (run.log,
classify). Коммит указывать в манифесте **справочно**, а не вместо dist.

Цена ошибки уже заплачена: прогон 29.07 дошёл ДАЛЬШЕ сохранённого этажа — ступень 14, `Present1`
и 39 `DrawIndexed`, — но его dist никто не сохранил. Остался только лог, и восстановить это
состояние сейчас нечем. Этаж от 06.07 (ступень 13) сохранён — и он работает.

**★ СВЯЗКА ЭТАЖ ↔ КОММИТ ↔ МАНИФЕСТ ПРОВЕРЯЕТСЯ, А НЕ ПОДРАЗУМЕВАЕТСЯ.**
`scripts/floor-verify.sh [каталог-этажей]` — гонять после снятия этажа И перед тем, как на этаж
опереться. Проверяет три вещи по фактам: SHA носителей из манифеста против файлов в дисте;
существование названного коммита в репозитории; комплектность (есть dist, манифест непуст).

Не теоретическая предосторожность — первый же прогон 2026-08-06 нашёл:
- `hk-rung13-rtv-BREAKTHROUGH-20260706`: манифест пишет `ntdll.dll 64570dd05286610a`, в этаже
  лежит `022ff35787d4452d`. Этаж разъехался со своим описанием — и в тот же день не воспроизвёл
  задокументированное в нём окно `WINSHOW 1512×982`;
- манифест есть только у ДВУХ этажей из девяти, остальные семь не описаны вовсе.

Этаж без манифеста или с разъехавшимся манифестом **выглядит сохранённым и не восстанавливает
ничего**, а узнаём мы об этом через месяцы — ровно тогда, когда чинить уже нечем.

Запускать этаж НЕ копируя его: `MACRUNNER_LANEA_WINE_DIST=<этаж>/dist` (и `MACRUNNER_WINE_DIST`).
Текущий dist при этом не затрагивается.

## ★ МАРКЕР-КРИТЕРИЙ ПРОВЕРЯТЬ GREP-ОМ ДО ПРОГОНА (2026-08-07, цена — день)

**Прежде чем назначить маркер критерием, убедиться что он СУЩЕСТВУЕТ и печатается БЕЗУСЛОВНО.**
Двадцать секунд `grep` против дня работы.

Как это стоило дня 07.08. Критерием трижды подряд был `macrunner-hb-nullcall-vtable` — «печать,
которая назвала бы пустой метод». Три прогона по 900 с ждали эту строку. Её **не существует в
коде**: имя встречается только в комментариях, реально написаны `nullcall-site`, `-engine`,
`-frame`, `-iat`, `-map`, а `-vtable` не реализован. Описание намерения было принято за прибор.

Три проверки, все дешёвые:

1. **Существует ли печать:** `grep -rn '"marker-name' engine/ | grep -i fprintf`. Совпадения
   только в комментариях = прибора нет.
2. **Печатается ли безусловно:** посмотреть, под каким `if` он стоит. Гейт env, период в 400 000
   событий, лимит «первые 8», фильтр по конкретному PC — всё это делает маркер лотереей, а не
   критерием. Аудит 07.08: из 86 маркеров движка **живых 8, молчащих 78**.
3. **Если условие может не наступить — критерий негодный.** `nullcall-vtable` требовал отказа
   `pc=0`, `guest-image-view` стоял за выключенным гейтом. Оба прогона отработали честно и не
   доказали ничего.

**Правильный критерий — БЕЗУСЛОВНЫЙ ЗОНД:** печать, поставленная туда, где нужные данные заведомо
есть, срабатывающая всегда. `macrunner-hb-vmprobe` (`macrunner_hb.c`, в
`module_from_pc_cache_put`) — образец: спрашивает `NtQueryVirtualMemory` по адресу внутри
гостевого образа и печатает `state/alloc_base/type` первые 8 раз. Он за 400 с ответил на вопрос,
на который три прогона по 900 с ответить не смогли.

**И следствие для выводов.** Тот же зонд показал, что стены не было: гостевые образы отвечают
`MEM_COMMIT + MEM_IMAGE` с верными базами. Наблюдение `alloc_base=0 MEM_FREE`, с которого всё
началось, было частным случаем, а обобщение до «образы не заведены в учёте» — непроверенным. На
нём успели вырасти спека и две правки, обе откачены.

## ★ ПРЕФЛАЙТ ПЕРЕД КАЖДЫМ ПРОГОНОМ (операторское правило, 2026-08-01; ужесточено 08-05)

**05.08 правило было нарушено мной же: весь день прогоны шли через `mr-run.sh` напрямую, минуя
`preflight-run.sh`.** Итог — полдня на диагностику падения `exit=3`, которого не было бы:
потерялся `MACRUNNER_GRAPHICS_BACKEND=dxmt`. Текст правила от этого не спасает, спасает отказ
инструмента запускаться. Поэтому в `preflight-run.sh` добавлены проверки 3b (графика задана и
её `dxgi` на месте) и 3c (набор переменных сверяется с `PREFLIGHT_REF_ENV` — `final-child.json`
прошлого удачного прогона; потерянные переменные = отказ, если не перечислены в `PREFLIGHT_DROP_OK`).

**Запуск игры напрямую через `mr-run.sh` — только для одноразовой диагностики, и в отчёте это
надо называть вслух. Любой прогон, чей результат пойдёт в вывод, — через `preflight-run.sh`.**


**Прогон, у которого не доказано, что проверяемая вещь включена, — это потерянные 4 минуты и,
хуже, ложный результат, который выглядит как ответ.** Случилось: лестница из пяти прогонов
бисекта по числу патчей ушла без `MACRUNNER_HB_BLOCK_CHAIN=1` (мастер-гейт, дефолт 0). Все пять
рук мерили ВЫКЛЮЧЕННОЕ сцепление и отчитались «клина нет ни при каком N». Выдало только то, что
времена совпали с базовыми до секунды.

**Гонять через `scripts/preflight-run.sh <tag> <secs> <маркеры-через-запятую> <ENV=VAL>...`.**
Он отказывается запускать прогон, пока не выполнено всё:

1. **Имя гейта есть в исходниках** — ловит опечатки и выдуманные переменные.
2. **Гейт есть в РАЗВЁРНУТОМ `.so`**, не только в дереве — ловит «собрал, но не задеплоил»
   (публикация умирает молча, в dist остаётся старый бинарь; укусило 2026-07-26 с winemetal.so).
3. **Мастер-гейт на месте**: любые `MACRUNNER_HB_CHAIN_*` без `MACRUNNER_HB_BLOCK_CHAIN=1` —
   отказ. Это ровно та ошибка, ради которой правило написано.
4. **Бинарь новее исходников** — иначе правки не в прогоне.
5. **Постпроверка**: заявленные маркеры-улики обязаны появиться в логе. Нет маркера — результат
   объявляется недействительным, а не интерпретируется.

**Гейт, активность которого не доказана, считается выключенным. Маркер, печать которого не
доказана, считается несуществующим.** За сессию 2026-08-01 счётчики четырежды решали исход вместо
измеряемого явления: слишком грубый период; предикат, до которого не доходило из-за
короткого замыкания выше; и дважды имя маркера, не совпадавшее с тем, что печатает код.

**Отдельно про A/B:** обе руки — ОДИН бинарь (сверять по SHA) и чередование рук, а не «сначала все
A, потом все B». Нагрузка машины здесь конфаунд первого порядка: одна конфигурация давала 87 с и
121 с от одной только нагрузки, что больше любого измеряемого эффекта.

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

## Не повторять один и тот же текст дважды

Оператор поправил трижды за 01.08. Реплики между вызовами инструментов и итоговое сообщение
видны В ОДНОМ окне: если объяснение сказано по ходу, а потом повторено в конце теми же словами,
это читается как шум и удваивает ответ без единого нового факта.

Правило: между вызовами инструментов — только «что делаю» и свежее число, одной-двумя строками.
Все выводы, таблицы и объяснения — ОДИН раз, в финале. Если вывод уже озвучен по ходу, в конце
на него ссылаться коротко, а не пересказывать.

## Перед прогоном: проверять ТОТ артефакт, который пойдёт в прогон

Правило «проверяй строкой в собранном ntdll.so» существует, но 02.08 оно не спасло: фикс был
проверен в ИСХОДНОМ дистрибутиве, откуда копировали, а целевой (на который указывал
MACRUNNER_LANEA_WINE_DIST) остался без него. Файлы приехали с внешнего диска только для чтения,
`cp` отказал по правам, ошибка была заглушена `2>/dev/null` — и два прогона по 20 минут
проверяли не то, что объявлено.

Перед КАЖДЫМ прогоном, который что-то подменяет:
1. `strings -a <ЦЕЛЕВОЙ артефакт> | grep -c <маркер правки>` — именно тот путь, который стоит
   в MACRUNNER_LANEA_WINE_DIST, а не тот, откуда копировали;
2. время файла у цели новее времени правки;
3. никаких `2>/dev/null` на шагах подготовки — ошибка копирования обязана быть видна.

Ноль на шаге 1 означает «прогон бессмыслен», а не «наверное сойдёт».

## Останавливая прогон — снимать всё дерево процессов, а не родителя

02.08 трижды: убитый цикл оставлял живых `laneA-run-hk.sh`, `mr-run.sh` и `winetemp-*`, они
вставали в очередь за слотом и перехватывали его вразнобой. Последствия не косметические:
`/tmp/laneA-current-rundir.txt` начинал указывать на чужой прогон, наблюдатель окна снимал не ту
игру, а вердикты приписывались не той сборке. Один раз в системе одновременно висело 15 таких
сирот возрастом до 13 часов.

Снимая прогон, проверять и добивать по именам: `hk-run-try12-config.sh <TAG>`, `laneA-run-hk.sh
<TAG>`, `mr-run.sh`, `Hollow Knight.exe`, `wineserver`, `winetemp-*`. Убивать по проверенному
PID (никаких общих `pkill`/`killall` — рядом работают чужие сессии). После — убедиться, что
`ps` не показывает ничего лишнего, и только потом запускать следующий замер.

## Диагностику печатать в stderr, а не через ERR/каналы wine

02.08: трасса `macrunner-hb-pe-stack` была написана через `ERR(...)` и молчала — потрачен
15-минутный прогон. Проверка показала: строк `err:` нет НИ В ОДНОМ прогоне за всю историю,
и `WINEDEBUG=+err` этого не меняет. Канал ошибок wine до наших логов не доходит.

Все работающие зонды проекта печатают `fprintf(stderr, ...)`. Новую диагностику писать так же.
И перед прогоном ради диагностики — убедиться, что строки этого класса вообще встречаются в
любом прежнем логе. Это пять секунд против пятнадцати минут.

## Воспроизведение конфигурации прогона

Повторяя чужой или прошлый прогон, бери переменные из его `final-child.json` (лежит рядом с
`run.log`, машинный список всех переменных дочернего процесса), а НЕ из текста вехи или отчёта.
Сверяй весь список разом, а не по одной переменной. Цена ошибки измерена 03.08: фраза вехи
«DIRECT_MEM=0» относилась к `MACRUNNER_HB_JIT_DIRECT_MEM`, тогда как `MACRUNNER_HB_DIRECT_MEM`
в том прогоне был 1 — полдня прогонов ушли на конфигурацию, вывернутую наизнанку.

**Не собирать набор переменных «снизу», по памяти.** 05.08: строил конфигурацию от заведомо
рабочего минимума и потерял `MACRUNNER_GRAPHICS_BACKEND=dxmt`. Игра пошла через `wined3d`+OpenGL
и умерла на `assert(wine_factory->lpVtbl == &dxgi_factory_vtbl)`, `dlls/dxgi/factory.c:559`,
`exit=3`. Половина дня ушла на диагностику того, чего не было. Брать `final-child.json` целиком.

## Дист — НЕ вся установка (05.08)

`make install DESTDIR=` кладёт в `engine/wine/dist`, а НЕ в `dist-arm64ec-spike`. Графика живёт
**отдельным деревом**, и `cp -Rp engine/wine/dist` её НЕ переносит:

- `engine/graphics/dist/dxmt/aarch64-windows/` — DXMT несёт СВОИ `dxgi.dll`, `d3d11.dll`,
  `d3d9.dll`, `d3d10core.dll`, `winemetal.dll`;
- включается `MACRUNNER_GRAPHICS_BACKEND=dxmt` (+ `MACRUNNER_DXMT_ROOT`), см. `scripts/mr-run.sh:397`;
- в снимках этажа лежит как `graphics-dist/` РЯДОМ с дистом, а не внутри него.

Снимая этаж — брать оба дерева. Снимок floor22 (16 КБ) уехал без графики именно поэтому.

Отдельный капкан описан в самом `scripts/mr-run.sh` (строки 60-66): при запуске через
`.app`-бандл тот тянет свой `wine`, и аргумент диста игнорируется. То есть можно собрать всё
верно и прогнать старое.

## Выравнивание секций 16 КБ — при линковке, не в загрузчике (05.08)

`dlls/ntdll/unix/virtual.c:4033` — `align_mask = max(image_info->alignment - 1, page_mask)`.
`image_info->alignment` берётся из ЗАГОЛОВКА PE, поэтому модуль, собранный с
`SectionAlignment=16384`, получает хостовое выравнивание сам. Править загрузчик НЕ нужно и
вредно: он двигал бы секции, а заголовок остался бы от 4 КБ, и поехали бы RVA и релокации.

Рецепт пересборки (605 модулей, шторм отказов 254 млн → 0, время до инициализации движка
28.6 с → 16.8 с):

```
PATH=<дерево>/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin:...
PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig
make -k -j8 aarch64_LDFLAGS="--no-default-config -Wl,--section-alignment,16384"
make install DESTDIR=
```

Без `PKG_CONFIG_PATH` сборка встаёт на `winegstreamer` (нет `glib.h`).

## ★★★ ВЫКЛЮЧЕННЫЙ ГЕЙТ — ГЛАВНАЯ ПОТЕРЯ ВРЕМЕНИ (05-06.08.2026, ТРИ случая за сутки)

Трижды за одни сутки дефект был **уже починен в нашем коде**, а гейт остался выключенным —
и каждый раз это стоило часов диагностики того, чего не существует:

| гейт | что даёт | как проявлялось без него |
|---|---|---|
| `MACRUNNER_HB_CACHE_SHARED=1` | один обработчик кеша на процесс | 191.7 млн выделений вместо 6.0 млн, `cacheopen` 100–230 мс вместо 0.00, подкачка 68 ГБ, диск с 80 до 18 ГБ |
| `MACRUNNER_WINEMAC_GAROOT_FALLBACK=1` | окно видно на экране | окна созданы на стороне Wine, macOS их не показывает |
| `MACRUNNER_HB_START_GAME_ACTUATOR=1` | нажать кнопку меню | игра ждёт нажатия, которого некому сделать: `UIManager::MakeMenuLean` зовут только `RunStartNewGame`/`RunContinueGame`, оба — действия игрока |

Первый из них **описан в комментарии к собственному коду** (`macrunner_hb.c:13130`:
«13777x hb_cache_open->load_entries [O(N)]») — то есть о нём знали и оставили выключенным.

**Правило.** Столкнувшись с симптомом, ПЕРВЫМ делом искать, нет ли для него гейта:
`grep -rhoE 'MACRUNNER_[A-Z0-9_]+' engine scripts tools --include='*.c' | sort -u`.
Дешевле пяти минут поиска не будет ничего. И наоборот: гейт, включённый в рабочем
прогоне, обязан попасть в профиль — иначе он теряется при следующей сборке набора.

## Профили прогонов: запуск одной командой (06.08.2026)

`scripts/mr-profile.sh` — самодостаточный снимок прогона: внутри профиля лежат `dist/`,
`graphics/`, `prefix-template/`, `final-child.json` и журнал-образец. Пересборка
`engine/wine/dist` профиль не ломает.

```
scripts/mr-profile.sh save <имя> <папка-прогона>
scripts/mr-profile.sh run  <имя> [сек] [дист] [ENV=VAL...]
scripts/mr-profile.sh desktop <имя> [сек] [ENV=VAL...]
```

Третий аргумент `run` различается по знаку `=`: путь → подмена диста, `ИМЯ=ЗНАЧЕНИЕ` →
переменная поверх профиля. Подмена ТОЛЬКО диста — правильный способ проверить новую
сборку при прочих равных.

`scripts/mr-archive.sh` — прогоны ПЕРЕНОСИТЬ на внешний диск, а не удалять
(05.08: снесено 46 папок, а место к тому моменту уже вернулось — удаление было не нужно).

## Накопление уроков в лейнах: lane-autoloop3.sh (06.08.2026)

В v1/v2 промт читался ОДИН раз и скармливался неизменным все N итераций — механизма
накопления не было ни в одном из семи скриптов автолупа. Отсюда повторяющийся класс
отказа: знание добыто, следующая итерация его не получила. В памяти проекта это помечено
«ловили ДВАЖДЫ», «ловили 3×», «оператор ловил 3+ раза».

`scripts/lane-autoloop3.sh` собирает промт заново перед каждой итерацией: базовый текст +
`reports/lanes/LESSONS-<лейн>.md`. Потолок 20 строк, старые вытесняются. Агенту разрешено
дописать 1-2 строки за итерацию и ТОЛЬКО по факту грабель («не отчёт, только правило») —
иначе откатилось бы правило «Prompts: solve, don't document».

**Урок состоит из двух частей: правило и КОМАНДА-ПРОВЕРКА.** Это из наблюдения 06.08:
правило, оставшееся прозой, игнорируется — я сам весь день нарушал записанное 01.08
«прогоны только через preflight-run.sh». Сработало не то, что текст дописали, а то, что
ИНСТРУМЕНТ НАЧАЛ ОТКАЗЫВАТЬ. Уроки с проверкой переносить в `preflight-run.sh` и делать
отказом.

Старые скрипты не тронуты. Запуск тот же:
`scripts/lane-autoloop3.sh <лейн> <прогресс> <макс> <промт> <модель>`

## ★★★ 06.08 — МЕНЮ ЯЗЫКА НА ЭКРАНЕ И ВВОД РАБОТАЕТ. Чем это оказалось

Одиннадцать языков отрисованы (включая японский, китайский, корейский), движок выдаёт
кадры 1512×982. Причина трёхсуточного клина — **файл САМОЙ ИГРЫ**, а не движок:

`Galaxy64.dll` был переименован в `Galaxy64.dll.ОТКЛЮЧЁН` в двух местах.
`GalaxyCSharpGlue.dll` его **импортирует**. Итог: вместо штатного `GOG failed to
initialize` летел `DllNotFoundException`, на который обработчик игры не рассчитан —
`DesktopPlatform.Awake()` не завершался, и вставало ВСЁ.

**Ввод тоже заработал** — игра отвечает на клавиши диалогом `Are you sure? Yes/No`.
Блокер, стоявший с 28.07 («меню есть, реакции нет»), снят тем же одним возвратом файла.

**Правило, которое из этого следует:** отключённый файл игры меняет **ПУТЬ ОТКАЗА**, а не
просто убирает функцию. Игра ловит свои ошибки и идёт дальше; на отсутствие нативной
библиотеки — нет. Переименование **не меняет mtime**, по датам это не находится.
Стережёт преполётная проверка 3e (обход `PREFLIGHT_GAME_DLLS_OK=1`).

**Метод, которым нашли — применять при любом «раньше работало»:**

1. Взять сохранённую веху из `reports/phase4-hollow-knight/checkpoints/` — там и сценарий
   запуска, и `evidence/run.log`, и скриншот.
2. Сверить переменные. **Смотреть ДВА места:** `final-child.json` профиля И
   `ДОП-ПЕРЕМЕННЫЕ.txt` — у профиля есть переключатели сверх снимка, я сравнил только
   первое и чуть не сделал ложный вывод «наборы совпадают».
3. Сравнить журналы: выкинуть строки приборов, **нормализовать ВСЕ числа**
   (`sed 's/[0-9][0-9.]*/N/g'`), сравнить множества строк. Без нормализации
   `Loaded All Assemblies, in 31.560 seconds` против `… 17.219 seconds` попадут в
   «есть только там» и уведут в сторону.
4. Смотреть не только «чего нет», но и **чем отличается то, что есть**: одно и то же
   место падало по-разному (`RuntimeError` против `DllNotFoundException`) — вот это и
   был ответ.

## Шрифты: freetype грузится по ГОЛОМУ ИМЕНИ

Wine собран с поддержкой freetype, но зовёт её `dlopen`'ом по имени, а библиотека лежит
в Homebrew, куда dyld не смотрит. `otool -L` показывает пусто — ложное «поддержки нет».
Было 3 жалобы «cannot find the FreeType font library» за прогон, стало 0.
Лечение: `scripts/mr-fonts.sh <дист>`; преполётная проверка 3d это стережёт.
Важно: 28.07 при меню на экране жалоба **тоже была** — это гигиена, а не блокер.

## ★★★ Эталон Windows кончается ТАМ ЖЕ, где мы (06.08, вечер)

`WINDOWS-REFERENCE-FULL-20260805/ref-A-galaxy/player-full.log` — настоящая Windows 11 ARM —
заканчивается той же строкой `Loaded Objects now: 4274` и той же жалобой про `GameCameras`.
**Цель «47240 объектов» была ошибочной**: столько было в НАШЕМ прогоне 28.07, куда протолкнул
автомат языка, а не на Windows.

**Правило:** `player.log` пишет только то, что игра сама решила записать — про ожидание, мышь
и курсор там строк быть не может. Сравнивать поведение — по `observer.log` в том же каталоге,
он снят нашим наблюдателем на той же Windows.

Осталось ровно два расхождения:
1. `set_allowSceneActivation` — на Windows **4** события (две пары), у нас **2**; менеджеров
   (`GameManager`/`UIManager`/`GameCameras`) на Windows три, у нас **ноль**. Расхождение
   РАНЬШЕ языкового экрана, а не после него.
2. Скорость: одинаковые операции идут в **18–89 раз** дольше (`Loaded All Assemblies`
   0.278 с против 17.29 с). Разрыв ровный — это производительность, а не поломка.

## ★★★ ДИСТОВ ДВА — правку класть в ОБА (06.08, стоило полдня)

```
MACRUNNER_WINE_BIN  = engine/wine/dist/bin/wine       ← запуск
MACRUNNER_WINE_DIST = engine/wine/dist                ← отсюда грузится ntdll.so
                      engine/wine/dist-arm64ec-spike  ← сюда обычно разворачивают
```
Правка только в `dist-arm64ec-spike` в прогоне ОТСУТСТВУЕТ. Симптом обманчив: прибор
«не печатает», гейт «не срабатывает» — и это читается как «явления нет». 06.08 так
пропали два замера подряд (DFSC и гейт резервирования). Проверять маркер в ТОМ файле,
что реально грузится, беря путь из `final-child.json`.

## ★★★ Шторм отказов = ОТСУТСТВУЮЩАЯ СТРАНИЦА (DFSC=0x07), а не выравнивание

`macrunner-hb-fault-page` теперь печатает `esr`, `ec`, `dfsc`. Замер:
`faults=257 136 606  esr=92000007  ec=0x24  dfsc=0x07  busy_pct=35`.
`dfsc=0x07` — translation fault ур.3, страница НЕ отображена; `pc` в системном кеше macOS,
то есть бьёт НАШ `memmove` из сигнального ограждения. Лечится разметкой (хвост секции до
полного `VirtualSize`, 16 КБ), не транслятором. `si_code`/`BUS_ADRALN` причину не дают —
смотреть только `DFSC`.

## Резервирование адресного пространства давало exit=5

`MACRUNNER_HB_RESERVE_HOLES=0` отключает. С резервированием — `exit=5` на 201-й секунде
три раза подряд; без него — ноль за весь бюджет 1200 с. Конфликты Mono при этом 8 против 9,
то есть резервирование их почти не убавляло. Нужны повторы.
