# GEMINI 3.5 FLASH — ARM64EC ntdll Research MASTER BRIEF

**Date:** 2026-05-28
**Agent:** Gemini 3.5 Flash (terminal CLI, READ-ONLY research, ОДИН write_to_file)
**Why Flash:** структурированное web/code research; **НЕ** инженерные правки движка.
**Working copy (read-only):** `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.
НЕ трогать `engine/**` (правки делает Codex/stronger model по твоему отчёту).

---

## Контекст и эскалация
MacRunner запускает x64 Windows-приложения на Apple ARM64 через свой собственный
HyperBridge + pure-ARM64 Wine fork. Графический агент (Kimi) уперся в инженерный блокер:

```
0024:err:module:load_ntdll_functions __wine_unix_call_dispatcher_arm64ec not found
0024:err:module:load_ntdll_functions KiUserEmulationDispatcher not found
0024:err:virtual:virtual_setup_exception stack overflow
```

Wine loader (`engine/wine/dlls/ntdll/unix/loader.c:1693, 1909-1933`) ЖЁСТКО требует
`__wine_unix_call_dispatcher_arm64ec` от `x86_64-windows/ntdll.dll` когда исполняется
x86_64 PE на ARM64 host. Наш текущий
`engine/wine/dist-pure-arm64/lib/wine/x86_64-windows/ntdll.dll` этот символ
**НЕ экспортирует** → x86_64 PE падает на запуске → DXMT даже не грузится.

То же отсутствие в snapshot'е `MacRunner-x64-snapshots/.../dist-x64only/lib/wine/
x86_64-windows/ntdll.dll`. То есть наш Wine собран **без ARM64EC support**.

## ЦЕЛЬ research (north star)
Понять и расписать с **file:line + ссылками + конкретным планом**, что нужно сделать
чтобы наш `x86_64-windows/ntdll.dll` экспортировал `__wine_unix_call_dispatcher_arm64ec`
+ `KiUserEmulationDispatcher` и работал в схеме:
`x64 Windows.exe → ARM64 Wine host → HyperBridge x64 → x86_64-windows DLLs → Metal`.

## Scope research (bounded — НЕ имплементировать, только разложить)

### 1. ARM64EC: что это и как Wine это собирает
- Что такое ARM64EC ABI (Microsoft) — кратко: эмулируемый x64 на ARM64 Windows,
  hybrid PE, fast-forward thunks. Ссылки на Microsoft docs.
- Где в Wine upstream появилась ARM64EC поддержка: год, версия, branch, релевантные
  commits/PR (через git log Wine upstream / wine-mirror github).
- Какие WINE BUILD конфиги/флаги включают ARM64EC: `--enable-archs=arm64ec`,
  configure-флаги, mingw-target требования. file:line в Wine configure/Makefile где это.
- Какие PE-файлы должны быть hybrid PE (arm64ec + x86_64) и как `x86_64-windows/ntdll.dll`
  получает `__wine_unix_call_dispatcher_arm64ec`. file:line в исходнике Wine
  (`dlls/ntdll/`).

### 2. Toolchain требования
- Какой llvm-mingw (или clang/lld) version умеет ARM64EC target.
- Наш `engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/` — поддерживает ли
  arm64ec target? (проверить наличие `--target=arm64ec-w64-windows-gnu` или похожего).
- Если нет — какой toolchain нужен и как его получить.

### 3. Существующие источники готового Wine ARM64EC dist
- Wine upstream (winehq.org) предоставляет ли готовый ARM64EC build для macOS/Linux?
- CrossOver (Codeweavers) — публиковали ли инженерные заметки про ARM64EC?
- Mythic/Whisky/GPTK — используют ли они ARM64EC или другой подход (Rosetta+wine64)?
- Microsoft `xtajit64.dll` (для PE32+) — что-то применимо к x64-on-ARM64?

### 4. Альтернативы (если ARM64EC build слишком тяжёл)
- Запуск x86_64 PE через wine-x86_64 + Rosetta (Apple Rosetta 2 как CPU-эмулятор).
  Что меняется в схеме? winemetal.so путь сохраняется?
- Имеет ли смысл «гибридный режим»: CPU через Rosetta, графика через DXMT/winemetal?
- Что теряем по сравнению с HyperBridge x64 (производительность, контроль, fallback
  после исчезновения Rosetta 2027-2028).

### 5. Конкретный план для Codex/stronger model (когда токены вернутся)
Дай **stepwise implementation plan** в виде списка задач с приоритетом, file:line, и
ожидаемым результатом:
- Шаги для добавления ARM64EC сборки в наш Wine build (configure, Makefile, dist
  layout).
- Альтернативный план «Rosetta-fallback» (быстрый, на случай если ARM64EC build
  заблокирован toolchain'ом).
- Список рисков и неизвестных (open questions).

---

## ВЫХОД (ОДИН write_to_file)
`reports/research/ARM64EC-NTDLL-RESEARCH-<date>.md` (~5-10KB):
- Краткий вывод (3-5 строк): что нужно, насколько реалистично, какой путь рекомендуешь.
- Секции 1-5 с конкретикой (file:line, ссылки, версии, commit ID).
- Раздел «Open questions» — то, что Flash не смог однозначно установить.
- Раздел «Handoff» — что именно делает Codex/stronger model по этому отчёту.

## ГРАНИЦЫ
- **READ-ONLY** по `/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/**`.
- Можно ходить в web (Wine source github, Microsoft docs, mailing lists).
- ОДИН write_to_file. БЕЗ engine-правок. БЕЗ запусков Wine/сборок.
- Если что-то не нашёл — честно пиши «open question», не выдумывай.
- Цель research, не имплементации: даёшь Codex'у точную карту, он чинит.
