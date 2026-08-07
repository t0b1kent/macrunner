# VERIFIED FIX BRIEF — Lane A: HK x64 dispatch-loop = ARM64EC RedirectionMetadata not applied
(workflow wxwshnds2, runtime-ground-truth, 2026-06-09. Supersedes LANE-A-THUNK-DISPATCH-FIX-BRIEF.md AND the
coordinator's "second x64 ntdll" lead — BOTH were wrong.)

## 0. Какая теория верна (runtime ground truth)
run.log:8507513: `View: 0x87fff910000 - 0x87fffffffff c-rWx (image)` — ОДИН image, span 0x6f0000.
Map: headers `0x87fff910000-0x87fff91ffff c-r--`, `.text` `0x87fff920000-0x87fffa2ffff c-r-x` (exec) — содержит trap PC.
pc=0x87fffa170c0, base 0x87fff910000 → RVA **0x1070c0**, внутри c-r-x .text. + ARM64EC CHPE metadata helpers
⇒ это **ARM64X/ARM64EC ГИБРИДНЫЙ ntdll**, НЕ отдельный x86_64 ntdll. RVA 0x507c0/0x2770c0 (прошлые) — против неверных
баз, бессмысленны. «.debug_info» — неверно (страница r-x). НЕ делать «не грузить 2-й x64-ntdll» — его нет.

## 1. КОРЕНЬ (подтверждён)
0x87fff910000 + 0x1070c0 = реальный **x64 entry-thunk в .text гибридного ntdll** — CHPE CodeMap type==2 (x64),
CodeRangesToEntryPoints диапазон [0x1070c0,0x1070e5), EntryPoint = сам 0x1070c0 (указывает на себя). Настоящая
ARM64-реализация в **RedirectionMetadata: x64 RVA 0x1070c0 → ARM64 RVA 0xe2a60**.
Почему exec-section гейт не режет (Phase F rejected=0): pc_in_executable_section и arm64x_pc_is_x64_code корректно
дают TRUE — это И ЕСТЬ exec x64-код. Гейт различает exec/не-exec, но не «x64 entry-thunk, который надо
перенаправить в ARM64». Не тот слой.
Почему луп при уже работающей нормализации: macrunner_hb_dispatch_x64_callback (19382) ВЫЗЫВАЕТ
macrunner_hb_normalize_arm64x_x64_callback_pc(target) на 19396. Но нормализатор (5824) смотрит только
CodeRangesToEntryPoints (x64→x64-entry). Для этого PC entry==pc → возвращает PC БЕЗ изменений → НЕ консультирует
RedirectionMetadata (x64→ARM64) → не доходит до 0xe2a60. HB JIT-исполняет x64-thunk → ре-фолт → бесконечный луп
(dispatch/sigill/0x87fffa170c0 счётчики в лок-степе, все 52403).

## 2. ФИКС — расширить нормализацию на ARM64EC RedirectionMetadata
Файл: engine/wine/dlls/ntdll/unix/macrunner_hb.c, macrunner_hb_normalize_arm64x_x64_callback_pc (с 5824).
redirect_arm64ec_rva объявлен extern в unix_private.h:342 (определён loader.c:2627) → вызываем из macrunner_hb.c.
macrunner_hb_get_arm64x_metadata (5651) и macrunner_hb_module_size — в этом же файле.
ЗАМЕНИТЬ существующий `return entry ? entry : pc;` (≈5846) на:
```c
/* CodeRangesToEntryPoints даёт x64 entry-thunk; для thunk'а, указывающего на себя,
 * надо следовать RedirectionMetadata к настоящей ARM64-реализации, иначе HB JIT
 * исполнит x64-thunk и ре-фолтнет. */
{
    IMAGE_ARM64EC_METADATA *md = macrunner_hb_get_arm64x_metadata( module );
    ULONG64 cand = entry ? entry : pc;
    if (md && md->RedirectionMetadata && md->RedirectionMetadataCount &&
        macrunner_hb_arm64x_pc_is_x64_code( module, (void *)(uintptr_t)cand ))
    {
        uintptr_t base = (uintptr_t)module;
        ULONG_PTR src_rva = (uintptr_t)cand - base;
        ULONG_PTR dst_rva = redirect_arm64ec_rva( module, src_rva, md );
        if (dst_rva != src_rva && dst_rva < macrunner_hb_module_size( module ))
            return base + dst_rva;   /* ARM64 implementation */
    }
    return cand;
}
```
Безопасность: срабатывает ТОЛЬКО для ARM64EC-модулей (RedirectionMetadata!=0 && count!=0). Обычный AMD64 guest
(сам HK.exe + его не-системные x64 DLL) НЕ ARM64EC → get_arm64x_metadata=NULL → поведение не меняется. redirect_arm64ec_rva
возвращает rva без изменений если нет entry (no-op для не-thunk x64). Гард dst_rva<module_size от битой таблицы.
После нормализации target становится ARM64 — убедись, что dispatch (19438/19476+, FF25 arm64_fn8 на 19490+) зовёт
ARM64-функцию напрямую, а не реджектит (если pc_is_x64_guest_code_no_lock=FALSE для ARM64-таргета).
ОТКАТИ мёртвые гейт-правки (теперь инертны, только путают): signal_arm64.c:997-998, :1044-1046, macrunner_hb.c:19458.
База pc_in_executable_section (5608) и классификатор (5728) — ОСТАВИТЬ (корректны, используются).

## 3. ПОЧЕМУ ОСТАНАВЛИВАЕТ ЛУП
Оба пути (SIGILL signal_arm64.c:2721 и SEGV/dispatch) сходятся на route_x64_callback_fault → трамплин (904) →
dispatch_x64_callback (19382), нормализация target на 19396. С фиксом normalize(0x87fffa170c0) следует
RedirectionMetadata 0x1070c0→0xe2a60 → возвращает ARM64-адрес. Диспетчер зовёт ARM64-рутину напрямую (без JIT
x64-thunk). Контроль НЕ исполняет x64 entry-thunk .text → не ре-фолтит на 0x87fffa170c0 — точка ре-входа устранена
на первой нормализации. Thread init проходит за entry-thunk в реальный ARM64 ntdll-код.

## 4. ПРОВЕРКА (WINEDEBUG=+virtual,+seh — НЕ +relay [флуд 600MB+], НЕ -all [прячет err:virtual])
- grep -ac 'dispatch-pe-fallback: target=0x87fffa170c0' → 0 (было 52403); sigill-pe-fallback ...0x87fffa170c0 → 0
- grep -ac '0x87fffa170c0' → ~0 (было 209624)
- (опц. MACRUNNER_HB_TRACE_CALLBACK_ROUTE=1) ждать 'arm64x-entrypoint-normalize ... entry_rva=0xe2a60' (редирект сработал)
- grep -ac 'NtUserCreateWindowEx' → не-ноль (окно достигнуто); Phase F rejected остаётся 0; размер лога рушится с ~9.2M строк

## ЕСЛИ ФИКС НЕПОЛНЫЙ (deciding trace, MACRUNNER_HB_TRACE_CALLBACK_ROUTE=1 + WINEDEBUG=+seh)
- normalize печатает entry=pc и нет redirect-строки → у RedirectionMetadata нет entry для 0x1070c0 (парс/таблица):
  дампни md->RedirectionMetadataCount + Source/Destination возле 0x1070c0 в живом образе.
- normalize вернул 0xe2a60, но луп на НОВОМ PC → ARM64-таргет сам ре-входит в x64-путь; пост-normalize ветка
  диспетчера (19438/19476) мис-роутит ARM64-таргет — чинить там (звать ARM64 напрямую, как FF25 arm64_fn8 на 19490+).

## ANCHORS
macrunner_hb.c: normalize 5824-5847 (правка), code_range_entry_point 5773, arm64x_pc_is_x64_code 5671,
get_arm64x_metadata 5651, dispatch 19382 (normalize call 19396), PE-fallback 19438-19474, gate 5608, classifier 5728.
loader.c: redirect_arm64ec_rva 2627.  unix_private.h: redirect_arm64ec_rva extern 342.
signal_arm64.c: route 940, trampoline 867-935, fault entrypoints 2512/2640/2721, dead-gate reverts 997-998/1044-1046.
Runtime proof: run.log:8507513 (image view) + dump_view 0x87fff910000 c-r-- / 0x87fff920000-0x87fffa2ffff c-r-x.
