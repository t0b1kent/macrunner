# MEGA-PROGRAM — i386/PE32: от «CPU исполняет» до «32-битные Windows-игры с окном»

**Лайн:** PE32/i386 (весь 32-битный фронт). **Горизонт:** МЕСЯЦЫ. Это ПРОГРАММА, не задача.
**Дата:** 2026-06-08. **Режим:** автономный — находи И СРАЗУ чини в корне, пересобирай, перепрогоняй,
чейнь к следующему блокеру. НЕ останавливайся по-блокеру. Делай МАКСИМУМ за заход.

## ⚡ АВТОНОМИЯ (читать первым)
- Нашёл блокер → ПОЧИНИ в корне → пересобери → перепрогони → СЛЕДУЮЩИЙ. Не «нашёл-отчитался-стоп».
- Решай, не документируй: для СВОИХ файлов чини сам, не пиши NEEDS. Кросс-лайн запись — только если
  корень реально в чужом файле (x64/ARM64X signal_arm64.c pdata — это HK-фронт, не трогай).
- Остановись ТОЛЬКО по 3 STOP-условиям в конце. На «продолжать?» ответ всегда ДА.

## 🟣 ВЫЖИВАНИЕ (чтобы идти месяцами, не умирая на контексте)
- ctx (ctx_execute javascript) на логи — НЕ сырые cat/grep/tail на 30МБ-логах (умрёшь). ctx_batch_execute
  тут падает (/bin/zsh ENOENT) — не используй.
- Heartbeat КАЖДЫЙ шаг ОДНОЙ строкой в reports/research/PE32-PROGRESS.md (resume-anchor).
- Прогоны через `MACRUNNER_RUN_DIR=$RUNDIR scripts/mr-run.sh …` (авто-триаж + flight в run-dir).
- Коммить ИМЕНОВАННЫЕ файлы (не -A). Код коммить, доки — нет.

## ГДЕ МЫ СЕЙЧАС (verified 2026-06-08)
- i386 CPU **исполняет**: Wow64LdrpInitialize → BTCpuProcessInit/ThreadInit → BTCpuSimulate → guest EIP.
- **heap.c size=0 ПОЧИНЕН** (guard: never pass 0 to kernel) → NtAllocateVirtualMemory SUCCESS → петля
  0x6c8000 ушла.
- **mr-run.sh services-start ОБОБЩЁН** (`MACRUNNER_MR_RUN_START_SERVICES=1` для любого прогона).
- **ТЕКУЩИЙ узел:** Diablo упирается в `0x6ba` RPC_S_SERVER_UNAVAILABLE. Caveat (от тебя же): голый
  `mktemp -d` префикс БЕЗ реестра → services.exe не знает rpcss → 0x6ba persist. Нужен реестр.

## SCOPE / ВЛАДЕНИЕ
- Твоё: `engine/wine/dlls/xtajit/**` (32-бит, НЕ xtajit64), `wow64`/`wow64cpu`/`wow64win`, i386 ntdll
  (`dlls/ntdll/heap.c`, i386 loader пути), deploy-скрипты, mr-run.sh config для i386.
- НЕ трогай: `signal_arm64.c` x64/ARM64X pdata (HK-фронт, отдельный терминал), `macrunner_hb.c`/
  `signal_arm64.c` SEH x64 (Lane A x64), `engine/dxmt|graphics` (Lane D).
- Verdict = реальное окно на экране (CG-capture), не «process alive».

## 🛑 BULK-FIRST (самый большой рычаг скорости)
i386-опкоды — конечный набор + есть референс. Когда EIP наткнётся на неверный/непокрытый опкод —
НЕ чини по одному: возьми матрицу Gemini `reports/research/HB-X86-32-ISA-COVERAGE-matrix.md` (+ vendored
capstone, golden oracle) и закрой семью разом. Атлас-приоритеты: MOVZX/MOVSX/MUL/DIV/ADC/SBB/rotates,
полный integer, x87, SSE.

## ФАЗЫ (сверху вниз; всегда имей следующий пункт; крути МЕСЯЦАМИ)

### Phase 0 — services/rpcss реально стартуют (текущий узел)
Голый префикс не имеет реестра → services.exe не поднимает rpcss → 0x6ba. ФИКС (выбери самый чистый):
(a) перед запуском — warm/инициализированный префикс (`bottles/generic-x86` или `wineboot -u`), чтобы
rpcss зарегистрировался; (b) или прогрей префикс один раз и кэшируй (warm-prefix template, как dxmt-путь
делает). Gate: в логе `[mr-run] services.exe pid=… started` И `0x6ba` ушёл.

### Phase 1 — Diablo проходит init до главного цикла
За RPC: ожидаемые `c0000005 @0x7bd6151b`, DLL-load стены, Win32-API surface (kernel32/user32/gdi32 i386).
Чини каждый в корне (i386 ntdll/xtajit/Win32-impl), чейнь. Gate: Diablo доходит до своей game-логики
(не падает в init).

### Phase 2 — Diablo graphics (DirectDraw) → кадр/окно
Diablo = PE32 + DirectDraw (8-bit indexed palette, SetEntries/Lock/Blit). Путь DDraw → поверхность →
окно. Палитра-oracle 8-bit → текстура. Это ОТДЕЛЬНО от DXMT (см. GEMINI-lanef-diablo-ddraw.md).
**Gate: Diablo рисует кадр/меню на экране (CG-capture).**

### Phase 3 — i386 ISA bulk coverage (find AND fix весь класс)
По мере исполнения — diff hb_decode_x86 vs capstone по большому корпусу; имплементь весь missing decode +
lift + interp + JIT, diff семантику (regs+flags+memory) vs golden oracle. Публикуй матрицу. Gate: декодер
матчит capstone (0 необъяснённых), oracle-diff зелёный.

### Phase 4 — Notepad++ x86 (не-игровой i386 GUI) → окно
Параллельно докажи чистый i386 GUI-путь: Notepad++ x86 → CreateWindow/NtUser/NtGdi → видимое окно.
Gate: окно Notepad++ x86 на экране.

### Phase 5 — Инсталляторы (Inno/NSIS/FreeArc/unarc/lolz) — дифференциатор
3GB LAA, async named pipes (декомпрессоры), CreateProcess+WaitForSingleObject, temp dirs. Валидируй на
in-tree инсталляторах + FreeArc-репаке (tools/repack гейт). Gate: инсталлятор доходит до конца,
поставленное приложение запускается.

### Phase 6 — Больше i386 игр + CI
GTA Vice City (D3D8), другие из ладдера; capstone-diff + oracle-diff фуз в CI (постоянный гейт).

## STOP-условия (ТОЛЬКО эти — иначе крути фазы МЕСЯЦАМИ)
1. Diablo i386 загрузился до кадра/окна на экране (доказательство-кадр) И Notepad++ x86 окно И ISA-матрица
   зелёная в CI И инсталлятор проходит — программа выполнена (асимптотически).
2. Жёсткий блокер реально в ЧУЖОМ файле (x64 signal/dxmt) — запиши одну строку в PE32-NEEDS.md и
   ПРОДОЛЖАЙ другие фазы (не простаивай; бэклог огромный).
3. Решение оператора по реальному компромиссу (платный ассет, подпись).

В КОНЦЕ КАЖДОГО захода — строка с колонки 0 в PE32-PROGRESS.md: `LOOP-STATUS: GOAL|BLOCKED <причина>|CONTINUE`.
