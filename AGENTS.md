# AGENTS.md — onboarding для AI агентов (Codex, Claude, Cursor)

Этот файл — единый источник истины для путей, скриптов и known traps в MacRunner.
Любой агент, который потерял контекст (compaction, session restart, новая сессия),
должен прочитать его прежде чем запускать команды.

## Первое действие в каждой сессии

```bash
cd /Volumes/MacOS/MacRunner
. ./config/env.sh
pwd      # должно быть /Volumes/MacOS/MacRunner
```

Если `pwd` показывает `/Users/timurtoby/Documents/MacRunner` — это **Obsidian vault, не код**.
Никогда не запускай build/test/wine команды оттуда. Стратегические доки — да, код — нет.

## Канонические пути (не выдумывай альтернативы)

| Что | Путь |
|---|---|
| Repo root | `/Volumes/MacOS/MacRunner` |
| Wine dist (ARM64-native, HyperBridge lane) | `engine/wine/dist-pure-arm64/` |
| Wine build (ARM64-native) | `engine/wine/build-pure-arm64/` |
| Wine dist (legacy / non-HB) | `engine/wine/dist/` |
| HyperBridge source | `engine/hyperbridge/` |
| Notepad++ x64 payload | `artifacts/phase-h/npp-x64/notepad++.exe` |
| Notepad++ x64 cwd (mandatory) | `artifacts/phase-h/npp-x64/` |
| Reports / logs | `reports/phase-h/` |
| Obsidian vault (DOCS ONLY, NOT code) | `/Users/timurtoby/Documents/MacRunner/` |

Все доступны как exported env vars после `. ./config/env.sh`:
`$MACRUNNER_ROOT`, `$MACRUNNER_WINE_DIST_ARM64`, `$MACRUNNER_NPP_X64_APP`, `$MACRUNNER_NPP_X64_DIR`,
`$MACRUNNER_REPORTS_ROOT`, `$MACRUNNER_ARTIFACTS_ROOT`.

## Канонические скрипты (используй эти, не изобретай свои)

| Задача | Команда |
|---|---|
| Запуск Notepad++ x64 (bounded 18s) | `./scripts/run-notepad-x64.sh` |
| Notepad++ x64 (hold alive) | `./scripts/run-notepad-x64.sh --hold` |
| Notepad++ с trace | `./scripts/run-notepad-x64.sh --trace=fileinfo,abi` |
| Сборка HyperBridge | `./scripts/build-hyperbridge.sh` |
| Сборка Wine pure-arm64 | `./scripts/build-wine-pure-arm64-experiment.sh` |
| Relink только ntdll.so | `cd $MACRUNNER_WINE_BUILD_ARM64 && rm -f dlls/ntdll/ntdll.so && make dlls/ntdll/ntdll.so install && codesign --force --sign - $MACRUNNER_WINE_DIST_ARM64/lib/wine/aarch64-unix/ntdll.so` |
| Repair WindowMetrics | `./scripts/repair-wine-window-metrics.sh --prefix $PREFIX --fix` |
| Wine cleanup (mandatory before/after) | `./scripts/kill-wine-tree.sh` или см. ниже |
| HyperBridge tests | `./scripts/test-hyperbridge.sh` |

## Process hygiene (обязательно)

Перед каждым запуском Wine **и** после bounded run:

```bash
pkill -9 -f '[w]inetemp-|[w]ine-preloader|[w]inedbg|[n]otepad\+\+\.exe|[s]ervices\.exe|[r]pcss\.exe|[e]xplorer\.exe|[w]inedevice\.exe|[p]lugplay\.exe|[s]vchost\.exe|[w]ineserver'
sleep 1
# verify zero:
ps -axo comm= | awk '/^(wine-preloader|wine64-preloader|winedbg|wineserver)$/{c++} END{print c+0}'
```

Если не сделать — Dock зарастает старыми debugger/exec окнами, юзер ругается.

## Known traps (не повторяй)

- **`WINEDEBUG=+module` даёт 17MB логов** за секунды и съедает trace budget. Используй `+file`,
  или `-all` + наши собственные `MACRUNNER_HB_TRACE_*` env vars (gated fprintf в stderr).
- **`/Users/timurtoby/Documents/MacRunner/` — это Obsidian, не код**. Если запустил `pwd` там —
  немедленно `cd $MACRUNNER_ROOT`.
- **`python3` (Homebrew) НЕ имеет модуля Quartz**. Для CGWindowList probes используй `/usr/bin/python3`
  (system) или `swift tools/cg_window_probe.swift`.
- **`WINEDEBUG=-all` подавляет ERR_(...)**. Наши собственные трассы должны быть `fprintf(stderr, ...)`
  с env-gating (`if (getenv("MACRUNNER_HB_TRACE_X"))`), а не через Wine debug channels.
- **При изменении HyperBridge сорсов обязательно `rm -f dlls/ntdll/ntdll.so` перед `make`**.
  make не видит `libhyperbridge.a` как dep, не пересоберёт ntdll автоматически. Force relink.
- **`status` — reserved variable в zsh**. Не используй как имя локальной переменной в скриптах
  под zsh — поведение неочевидное.
- **Pre-launch sanity check бинарников** в начале сессии (один раз):
  ```bash
  file $MACRUNNER_WINE_DIST_ARM64/bin/wine
  file $MACRUNNER_WINE_DIST_ARM64/lib/wine/aarch64-unix/ntdll.so
  ```
  Оба должны быть `Mach-O 64-bit ... arm64`. Если выходит `ASCII text` — диагностические скрипты
  ранее перезаписали бинарь логом, recover из build dir.
- **Notepad++ требует cwd = payload dir**. Без `cd artifacts/phase-h/npp-x64` — silent exit
  (cwd-local resources: langs.xml, stylers.xml, doLocalConf.xml).
- **FreeType invisible to Wine without DYLD path**. Helper script уже ставит
  `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib:...`. Не убирай.
- **WindowMetrics corruption recurring** (CaptionHeight=2049 → 6x58 sliver window).
  win32u уже имеет clamp, но всё равно прогоняй `repair-wine-window-metrics.sh --fix` после wineboot.
  Порядок важен: boot → wait for user.reg → wineserver -w → repair → launch.
- **Background процесс с `&` и parent shell exit = SIGHUP**. Если запускаешь Wine в background
  и shell завершается до 20s sleep, процесс умирает silently. Держи shell живым или используй `nohup`.

## Architectural bugs reference

Найденные fundamental bugs (class-level, retroactively explain прежние симптомы):
см. [Obsidian / 90-architectural-discoveries-fundamental-bugs.md](file:///Users/timurtoby/Documents/MacRunner/90-architectural-discoveries-fundamental-bugs.md)

1. RIP-relative + trailing immediate (decoder semantics, ~12 opcode class)
2. x18 sigreturn restoration (macOS Apple ARM64 platform ABI)
3. Basic block PC fall-through (interpreter execution model)
4. REX.W ignored on 0F 7E — MOVD vs MOVQ (MSVC CRT argument pack via SSE)

При новых "strange register value" симптомах — first check эти. Особенно #4 для нулевых
аргументов через WinAPI.

## Methodology rules

1. **Validate premise before fix.** Если думаешь "X сломан" — сначала trace доказательство,
   потом patch. Patch-by-symptom = workaround, не fix.
2. **Class-level audit после finding class bug.** Нашёл `REX.W` ignored на 0F 7E → audit
   всю SSE↔GPR transfer family (0F 6E, 0F 7E, 66 0F D6, F3 0F 7E, и т.д.) на ту же ошибку.
   Один час audit'а спасает 10 часов реактивного debug.
3. **Real app testing > synthetic unit tests** для finding architectural gaps. Unit tests
   verify known correctness; real apps expose unknown gaps.
4. **Live-memory probe ("рентген-trace") вместо guessing.** Печатай память pack-struct,
   xmm0, GPR до/после подозрительной инструкции через `mach_vm_read_overwrite`.
   Mechanical bisection между правильным и неправильным состоянием изолирует bug точно.

## Когда обновлять этот файл

- Новый канонический путь добавлен → обнови табличку
- Новый класс trap встречен → добавь в "Known traps"
- Новый fundamental bug найден → добавь номер в "Architectural bugs reference"

Этот файл переживает context compaction. Stale entries хуже чем missing — поддерживай актуальность.
