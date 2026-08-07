# FIX BRIEF — Lane A: рекурсивный stack-overflow на границе ARM64↔x64 HyperBridge
(сгенерён ultracode-workflow, консолидирует ракурсы Lane A + Lane D + PE32. 2026-06-08)

## 1. КОРЕНЬ (единый, три лица)
**ОДИН корень, три проявления.** Exception-setup путь **двигает стек, но не убирает причину** — превращая
один восстановимый фолт в неограниченную ре-входную петлю. Когда нативный фолт берётся при SP на bridge-стеке
HyperBridge (`0x87fff…`, `__thread` диапазон в `macrunner_hb.c:285-290`, заводится в TEB на
`macrunner_hb.c:19694-19701`), оба redirect-сайта — `setup_raise_exception` (`signal_arm64.c:1198-1214`) и
last-page путь `virtual_setup_exception` (`virtual.c:5872-5923`, `return base - size`) — ставят свежий **SP**
(`context->Sp`/`stack_ptr`), но оставляют **`context->Pc` на том же HB-helper'е, что рекурсит**, и НЕ
разматывают нативную ARM64/x64 цепочку. KiUserExceptionDispatcher бежит на перемещённом StackBase, не находит
guest-обработчика для НАТИВНОГО overflow, падает обратно в helper `…070c0`, снова переполняет → снова входит
в `setup_raise_exception` → снова редиректит.

**Доказано логом:** HK редиректит на ВАЛИДНЫЙ StackBase (`0x110570000`) **84 раза**, fault PC константа
(`0x87fffa170c0`), байты растут на кадр за проход (592→816) до `exit=143`; экспериментальный sine_440-билд с
лимитером `[redirect N/3]` чисто абортит на "redirect 4" — **цель редиректа НЕ фикс, петля — вот баг.**
Три других ракурса (неверная битность цели, нулевой запас StackBase, TEB-swap гонка) — реальные **вторичные**
дефекты (первый редирект садится плохо), но НЕ драйвер петли. В текущем `virtual.c` лимитера петли **НЕТ**
(`macrunner_bt_done` на 5878 гейтит только fp-walk диагностику, не редирект).

## 2. ФИКС (конкретно, файлы Lane A) — по приоритету
(A) — loop-breaker, ОБЯЗАТЕЛЕН; (B)+(C) делают единственную доставку корректной.

### (A) Re-entrancy gate — не пере-диспатчить фолт, случившийся ВО ВРЕМЯ диспатча (ломает петлю)
В **`signal_arm64.c setup_raise_exception` (верх, ~1187)** добавь per-thread guard:
```c
static __thread int macrunner_exc_dispatch_active;
```
- На входе: если `macrunner_exc_dispatch_active` уже взведён **И** faulting PC (`rec->ExceptionAddress`) в
  HB-native диапазоне — это фолт ВО ВРЕМЯ доставки → НЕ relocate-and-retry. Доставь `STATUS_STACK_OVERFLOW`
  **один раз** на оригинальный guest-стек (см. B), либо если уже пытались — `abort_thread(1)` с отдельным
  сообщением. Перенеси счётчик `[redirect N/3]` из sine-билда как минимальную страховку (худший случай —
  чистый abort, не `exit=143`).
- Взводи флаг перед построением dispatcher-фрейма; снимай на `DBG_CONTINUE`/`restore_context` early-return (1226).

Зеркаль тот же `__thread` счётчик в **`virtual.c virtual_setup_exception`** вокруг `return base - size`
(5913-5921): кап редиректов (напр. 3) на поток; при превышении — в существующий `abort_thread(1)` (5924), не в петлю.

### (B) Редирект на ОРИГИНАЛЬНЫЙ guest-стек (не bridge-top) + битность
Цель редиректа — закоммиченная guest-память с реальным запасом, не переполняющийся bridge-стек.
В **`setup_raise_exception` 1203-1211** и **`virtual.c` 5913-5921**, когда SP в bridge-диапазоне, замени
live-TEB чтение на сохранённый guest-стек:
```c
extern __thread void *macrunner_hb_original_stack_base;   // macrunner_hb.c:290
extern __thread void *macrunner_hb_original_stack_limit;  // macrunner_hb.c:289
```
- Цель = `macrunner_hb_original_stack_base - RESERVE`, `RESERVE >= 0x100000` (как `min_guaranteed` в
  `is_inside_thread_stack`), 16-выровнено — **НЕ** голый `StackBase` (это эксклюзивный top, ~0 запаса →
  регрессия "exits=1 до CreateWindowEx").
- **i386/WOW64:** гейт `is_wow64() && main_image_info.Machine == IMAGE_FILE_MACHINE_I386` (как
  `macrunner_hb_wow64_i386_execute_fault` ~1038-1040). Редирект на `get_wow_teb(teb)->Tib.StackBase - RESERVE`
  и ставь **i386 Esp** через `get_cpu_area(IMAGE_FILE_MACHINE_I386)` — ставить ARM64 `context->Sp` на 64-битный
  base бессмысленно для guest'а, оттого `@7bdc6da8` и рефолтит.
- Добавь TEB-независимый bridge-range тест (`SP ∈ [macrunner_hb_bridge_stack_limit, base+size)`) как
  авторитетный триггер — чтобы корректность не зависела от того, отработал ли TEB-swap restore
  (`macrunner_hb.c:20392-20402`). Зеркаль в `is_inside_thread_stack` (`virtual.c:5635-5660`): bridge SP НЕ
  должен классифицироваться как guest thread-стек.

### (C) Не релоцировать при истинно нативно-рекурсивном фолте
В обоих сайтах: если `rec->ExceptionAddress` в HB-native И gate (A) показывает что для этого PC уже редиректили
— это реальный native-recursion баг, релокация не лечит → терминируй с оригинальным исключением, не крути.

## 3. ПОЧЕМУ ЧИНИТ ВСЕ ТРИ
- **HK window-callback (x64):** петлю на 84 редиректа убивает (A); (B) даёт единственной доставке реальный
  запас на оригинальном guest-стеке → in-flight callback-фрейм на bridge-стеке цел, callback доходит за
  atom 8001/c01d к `CreateWindowEx`.
- **sine_440 (x64, минимальный, без callback):** тот же путь без вложенности. (A) ограничивает; (B)-запас
  делает единственную доставку успешной. Быстрый regression-гейт (падает до winmm).
- **i386 @7bdc6da8 (WoW64):** нужна именно битность-ветка (B) — сейчас редирект шлёт i386-диспетчер на 64-битный
  StackBase → его handler рефолтит; маршрут на 32-битный WOW StackBase + i386 Esp, плюс (A) капает republish-петлю
  guest32-диспетчера (ESP −0x340/итер) → терминирует.

Всё сводится к «фолт на bridge SP → relocate SP, keep PC, re-fault». (A) ломает петлю всем; (B) делает
выжившую доставку корректной по битности.

## 4. RECURSION-GUARD
1. **`__thread int macrunner_exc_dispatch_active` в `setup_raise_exception`** — фолт во время построения
   exception-фрейма не может ре-войти; второй вход → одна reserved-slab доставка, третий → чистый abort.
2. **Per-thread callback nesting depth вокруг `macrunner_hb_run_x64`** (`macrunner_hb.c:19512` + callback-трамплин
   `macrunner_hb_route_x64_callback_fault` `signal_arm64.c:1027-1030`). Инкремент на входе / декремент на `done:`
   restore (20392-20402). Если фолт пришёл при depth > ~4 уровней bridge-dispatch — доставляй overflow на
   `macrunner_hb_original_stack_base`, не маршрутизируй ещё один вложенный WndProc/handler. Капает вложенный
   WndProc Lane D и handler-рефолт PE32 даже при кратко неверной цели.

## 5. VERIFICATION (grep-маркеры)
- **sine_440 (x64) — overflow = 0:** прогон audio smoke; `grep -c "stack overflow" reports/audio-test/<ts>/stderr.log`
  → **0**, нет `redirect 4`/`loop detected` abort. Pre-fix baseline: `reports/audio-test/20260608-144253/stderr.log:72753-72756`.
- **HK — дойти до CreateWindowEx:** `grep -c "0x87fffa170c0" reports/phase4-hollow-knight/latest/run.log` →
  малое/ограниченное (не 84+), `exit`≠143, маркер window-creation/`CreateWindowEx` ПОСЛЕ atom `8001/c01d`.
  Pre-fix baseline: `run.log:1336-1383`.
- **i386 — handler не рефолтит:** `grep -c "7bdc6da8" reports/pe32/latest/run.log` → ограничено (pre-fix 379×),
  guest32 ESP не шагает вниз по `0x340`/итер.

## ANCHORS
- `signal_arm64.c:1187` setup_raise_exception; redirect 1198-1214 (ставит SP, **не PC**); post-fix safe_sp
  1235-1240; callback route 1027-1030.
- `virtual.c:5800` virtual_setup_exception; unbounded redirect `return base - size` 5913-5921; лимитера НЕТ;
  `macrunner_bt_done` 5878 (только fp-walk); `is_inside_thread_stack` 5635-5660; early unmapped `return stack - size` 5815.
- `macrunner_hb.c`: globals 285-290; run_x64 swap 19694-19701 / restore 20392-20402; WoW64 simulate swap
  17972-17976 (`bridge_base = EmulatorStackLimit` @17973) / restore 18084-18088; bridge alloc no-guard-page ~6338-6372.
