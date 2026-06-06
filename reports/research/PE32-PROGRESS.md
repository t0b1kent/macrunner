# PE32 Progress Log

## 2026-06-06 — wow64cpu/xtajit/i386-ntdll handoff checkpoint

- `engine/wine/dlls/wow64/syscall.c: process_init()` is the handoff entry for BTCpu init:
  - calls `get_cpu_dll_name()`
  - `module = load_64bit_module( get_cpu_dll_name() )`
  - resolves all `BTCpu*` exports via `RtlFindExportedRoutineByName`
  - logs `before BTCpuProcessInit` -> `pBTCpuProcessInit()`
- `get_cpu_dll_name()` selects:
  - `xtajit.dll` for `current_machine == IMAGE_FILE_MACHINE_I386` on `IMAGE_FILE_MACHINE_ARM64` native.
  - `wow64cpu.dll` otherwise for x86\_64 path.
- In run `reports/phase-h/npp-x86-diagnostic-20260606-150550`, triage shows required i386 DLLs are missing:
  - `i386 wow64cpu.dll` in `bottles/generic-x86/drive_c/windows/syswow64` is missing.
  - `i386 xtajit.dll` in `bottles/generic-x86/drive_c/windows/syswow64` is missing.
- `summary.txt`/`triage-summary.txt` classify `PE32_WOW64CPU_NOT_LOADED` and `LOADER_STALL_BEFORE_BTCPUSIMULATE` with `BTCpuSimulate_status_count=0`.
- `stdout.log`/`stderr.log` contain no `BTCpuProcessInit` or `BTCpu*` markers, while the prefix contains valid `i386 ntdll.dll` and `i386 kernel32.dll`.
- Finding: handoff chain is blocked before execution reaches `BTCpuProcessInit`; missing i386 `syswow64` CPU runtime DLL in the prefix prevents `load_64bit_module(get_cpu_dll_name())` from producing a module with BTCpu exports.

Next step: populate `bottles/generic-x86/drive_c/windows/syswow64/{wow64cpu.dll,xtajit.dll}` (or registry override via `Wow64\\x86`) and re-run with WOW64 handoff markers enabled.

## 2026-06-07 — clean handoff trace classification

Run dirs:
- `reports/phase-h/npp-x86-handoff-manual-20260607-031851`
- `reports/phase-h/npp-x86-handoff-clean-20260607-032035`

Command shape:
- Manual equivalent of `reports/phase-h/run_window_clean.sh`, because that script forces `WINEDEBUG=-all`.
- `WINEPREFIX=$PWD/bottles/generic-x86`
- `WINEDLLPATH=$PWD/engine/wine/dist-pure-arm64/lib/wine`
- `WINEDEBUG=+loaddll,+module`
- `MACRUNNER_XTAJIT_TRACE_SYSCALLS=1`
- `MACRUNNER_XTAJIT_TRACE_STACK=1`
- Scoped cleanup only: `WINEPREFIX=$PWD/bottles/generic-x86 engine/wine/dist-pure-arm64/bin/wineserver -k`

Observed loader evidence from `reports/phase-h/npp-x86-handoff-clean-20260607-032035/stderr.log`:
- `notepad++.exe` is mapped as a PE32/i386 executable:
  - `trace:module:get_load_order ... artifacts\\phase-h\\npp-x86\\notepad++.exe`
  - `trace:module:map_image_into_view mapping PE file ... notepad++.exe`
- `C:\\windows\\system32\\start.exe` and `C:\\windows\\system32\\ntdll.dll` load.
- Counts from the processed log:
  - `notepad++`: 9
  - `wow64.dll`: 0
  - `wow64cpu.dll`: 0
  - `wow64win.dll`: 0
  - `xtajit.dll`: 0
  - `BTCpu`: 0
  - `macrunner-wow64`: 0
  - `macrunner-xtajit`: 0
  - loader/errors: 0

Prefix DLL presence captured in `env.log`:
- `syswow64/wow64.dll exists=0`
- `syswow64/wow64cpu.dll exists=0`
- `syswow64/wow64win.dll exists=0`
- `syswow64/xtajit.dll exists=0`
- `syswow64/ntdll.dll exists=1`
- `syswow64/kernel32.dll exists=1`
- `system32/wow64.dll exists=1`
- `system32/wow64cpu.dll exists=1`
- `system32/wow64win.dll exists=1`
- `system32/xtajit.dll exists=1`

Post-run classifier:
- `python3 tools/triage/classify_run.py reports/phase-h/npp-x86-handoff-clean-20260607-032035`
- Output class: `WAIT_TRACE_INSUFFICIENT` with secondary `PE32_HANDOFF_TRACE_INSUFFICIENT`.
- This generic classifier does not override the handoff-specific evidence above.

Handoff classification:
- `wow64cpu` load: no evidence of load.
- `xtajit.dll` lookup/load: no evidence of lookup/load.
- `BTCpu*` export resolution: not reached.
- `BTCpuProcessInit`: not called.
- Root class for this narrow gate: backend not loaded before BTCpu resolution, not export mismatch and not `BTCpuProcessInit` failure.

Current conclusion:
- The i386 handoff never reaches `wow64/syscall.c:process_init()` markers. The PE32 image maps, but the WOW64 CPU-provider layer is not entered. The prefix has i386 `ntdll.dll`/`kernel32.dll`, but no i386 `syswow64` copies of `wow64.dll`, `wow64cpu.dll`, `wow64win.dll`, or `xtajit.dll`; only `system32` has those CPU-provider DLLs.

## 2026-06-07 — phase 0 deploy+trace checkpoint

2026-06-07 03:48 · Phase 0 · sync-prefix fix + trace retry ·
Что сделано: `scripts/sync-prefix-from-dist.sh` теперь деплоит `wow64cpu.dll`, `wow64.dll`, `wow64win.dll`, `xtajit.dll` в `syswow64`; ручной redeploy для `bottles/generic-x86` подтвердил все файлы в `syswow64`.
Результат/классификация: `macrunner_hb_x64_main_requested` логи есть, но `wow64/xtajit/BTCpuProcessInit` маркеры отсутствуют, `PE32-` трасса до сих пор `WAIT_TRACE_INSUFFICIENT`; первичная причина текущего узла сейчас — `PE32_WOW64CPU_NOT_LOADED` / backend не грузится до handoff.
Next: keep Phase 0 active, identify why 32-bit path still stays on 64-bit bootstrap path before wow64 handoff and retry with explicit process-init-only wow64 tracing.
03:58 · диагностика каналов · ctx_batch_execute через shell не работает (ENOENT /bin/zsh), переключаюсь на exec_command для rg/чтения точечных файлов · продолжить локализацию причины syswow64-path
03:58 · локальный поиск bootstrap ·  без wildcards, т.к. shell прерывал из‑за пустых матчей путей · детектировать функцию выбора WOW64 CPU DLL в 
04:00 · Поиск причин выбора bootstrap и WOW64 path · context-mode запускался с ENOENT/запрос в zsh, анализ не получен · Проверю целевые файлы через прямые команды
04:01 · Продолжение диагностики bootstrap WOW64 для i386 · Проверка текущего состояния и трасс PE32 · Далее прогон с трассой bootstrap
04:04 · локальная трасса конфликта bootstrap · найден источник x64-bootstrap env override для PE32 (конфиг/скрипты) -> следующий шаг: безопасно отключить x64 bootstrap для machine=x86 в скриптах запуска/обертках; затем немедленный rerun run_window_clean
04:04 · правка деплой-раннеров в процессе · MACRUNNER_HB_X64_LOADER=0 для machine=x86 в run-windows-app + run_window_clean; next run_window_clean TRACE check (BTCpu/wow64 markers)
04:08 · heartbeat start · прочитал требования и стартовал поиск в wow64/x86 chain · next: найти функцию выбора machine/bootstrap
04:22 · локальная диагностика -> точечный правка в run-notepad-x64.sh · обнаружена hardcoded MACRUNNER_HB_X64_LOADER=1 для x86-кейсов · следующий шаг: изменить на machine-aware env и rerun handoff trace
04:25 · start · inspect progress and handoff context for PE32 root-cause path
04:25 · step1 · reviewed PE32-PROGRESS checkpoints and last findings about syswow64 path/handoff markers
04:28 · диагностика · найден глобальный дефолт X64-loader в mr-run + run-notepad для i386 проверки · переходим к чтению runner скриптов и правке локального scope
04:29 · диагностика · сверка wow64 handoff функции на предмет x64/arm64ec/bootstrap флага и current_machine · план: оставить логику в script/runner без правок ядра
04:30 · Инициация фикс-прохода PE32 · подтверждена трасса: x64 bootstrap включается для i386 через MACRUNNER_HB_X64_LOADER · правка скриптов запуска и повторный сбор/прогон
04:36 · Анализ скриптов — найдено несколько точек конфигурации x64 loader (run-notepad*, run-notepad-x64-ui-smoke, возможно run-windows-app) с потенциалом перетирания значения для x86
04:41 · Выявил детерминированный источник включения x64-загрузчика: в run-notepad* используется set-if-unset для MACRUNNER_HB_X64_LOADER
04:45 · Патч: run-notepad-x64.sh теперь всегда ставит x64-loader=0 для x86 PE, иначе не трогает если x64 и уже задано вручную
04:48 · Патч: run-notepad-x64-ui-smoke.sh синхронизирован с тем же правилом x86=0, иначе только когда env не задан
04:52 · Подтверждена доступность PE32 артефакта npp-x86/notepad++.exe для проверки после правки
05:00 · Проверочный прогон выполнен с принудительно заданным MACRUNNER_HB_X64_LOADER=1 для x86 app; запуск завершился неуспешно (rc=1), собираю артефакты
05:10 · run-windows-app запуск 30s на npp-x86 завершился по таймауту инструмента/внутренне; логи есть в /tmp/pe32-rwa.log для пост-анализа
05:15 · run-windows-app дал timeout и записал stdout/stderr в reports/run-windows-app/run-20260607-043106.*; продолжаю чтение только через ctx_* по сути запуска
05:23 · Для проверки переключения bootstrap запускаю системный поиск по логу run-windows-app на строки x64-loader и BTCpuProcessInit
05:33 · Второй прогон run-notepad-x64.sh завершён (без свежести-превью); логи в /tmp/pe32x86-run2.log для анализа
05:43 · Прогон с MACRUNNER_NPP_X64_DIR=npp-x86 завершился; собираю run-лог и проверяю machine/loader признаки
05:46 · Лог теперь указывает APP=/artifacts/phase-h/npp-x86; запускается и сразу kill после 20s, проверяю трассу в RUN-dir
05:55 · Прогон синтакс-проверки: run-notepad-x64.sh и run-notepad-x64-ui-smoke.sh OK после правок
LOOP-STATUS: CONTINUE — верификация bootstrap частичная: два фронтовых скрипта переведены на принудительный 0 для x86; следующий шаг — подтверждение через run-windows-app/run-notepad x86 с трассой macrunner_hb_x64_main_requested и BTCpuProcessInit в live-запусках

08:01 · старт PE32 loop · прочитал checkpoint: syswow64 deploy уже закрыт, текущий blocker — PE32 уходит в x64/arm64ec bootstrap до WOW64/i386 · следующий шаг: статически найти machine/bootstrap выбор и проверить, не требуется ли Lane C loader.c
08:01 · dirty-tree audit · обнаружены многочисленные чужие изменения, включая loader.c/x64/graphics; свои действия ограничиваю PE32/deploy и reports/research · следующий шаг: ctx-поиск routing-пунктов bootstrap
08:02 · ctx_batch limitation · ctx_batch_execute в этом окружении всё ещё падает на spawn /bin/zsh ENOENT · переключаюсь на ctx_execute javascript с execFile/FS без сырого вывода
08:03 · mega-program сверка · актуальный файл найден в reports/research/CODEX-MEGA-PROGRAM-pe32-32bit-to-real-apps.md; gate Phase 0 = BTCpuProcessInit/BTCpuThreadInit в логе · следующий шаг: проверить live routing после предыдущих runner fixes
08:06 · loader read-only audit · loader.c показывает x64 bootstrap только если main image AMD64 + env enabled; PE32 может обходиться через 64-bit wrapper/start.exe или bootstrap host-module remap · следующий шаг: доказательный NPP x86 trace с machine/env markers
08:04 · proof run · NPP x86 trace собран в reports/phase-h/pe32-handoff-proof-20260607-080428, процесс остановлен scoped timeout через wineserver -k · следующий шаг: ctx-классификация stderr/stdout на BTCpu/wow64/bootstrap markers
08:08 · proof classification · первый trace показал только prefix wineboot main=aa64 при loader env=0, до notepad++.exe не дошёл; wow64/BTCpu=0 · следующий шаг: повторить с MACRUNNER_HB_SKIP_WINEBOOT/MACRUNNER_SKIP_WINEBOOT=1
08:06 · skipboot proof run · trace собран в reports/phase-h/pe32-handoff-proof-skipboot-20260607-080556, scoped timeout остановил процесс · следующий шаг: классифицировать notepad++/WOW64 markers
08:10 · root evidence · notepad++.exe PE32 мапится, но build_main_module выбирает C:\\windows\\system32\\start.exe machine=aa64; wow64/xtajit/BTCpu=0 · следующий шаг: найти source fallback на start.exe для non-native PE32
08:13 · source trace · fallback source: ntdll/unix/env.c load_main_exe() treats non-success PE32 map as invalid and calls load_start_exe(); Unix loader PE32 routing is gated by macrunner_hb_x64_loader · следующий шаг: compare env=1 vs env=0 before writing PE32-NEEDS
08:17 · compare env=1 · MACRUNNER_HB_X64_LOADER=1 still maps notepad++.exe then falls back to start.exe machine=aa64; no PE32 i386-lane/wow64/BTCpu markers · следующий шаг: identify virtual_map_module mismatch path and write Lane C need if outside PE32 ownership
08:20 · Lane C handoff written · reports/research/PE32-NEEDS.md создан с evidence по start.exe fallback и mapping.c/env.c/loader.c точками · следующий шаг: укрепить PE32-owned runner/deploy без правки ntdll/server
08:14 · deploy readiness · sync-prefix-from-dist rerun OK for bottles/generic-x86, log reports/phase-h/pe32-sync-prefix-20260607-081418.log (1234 lines) · следующий шаг: проверить машинные типы и BTCpu exports в деплое
08:22 · PE32 deploy/export readiness · syswow64/start.exe+kernel32.dll are x86; xtajit.dll is arm64 and exports BTCpuProcessInit/BTCpuThreadInit/BTCpuSimulate; wow64cpu exports BTCpuProcessInit too · следующий шаг: run classifier and finalize blocked/continue status
LOOP-STATUS: BLOCKED Lane C loader/server handoff: PE32 notepad++.exe maps, then ntdll/unix env.c load_main_exe/load_start_exe replaces it with ARM64 C:\windows\system32\start.exe before WOW64, so BTCpuProcessInit cannot be reached; exact evidence and requested fix recorded in reports/research/PE32-NEEDS.md.
