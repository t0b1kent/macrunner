# FIX BRIEF — Lane A: module-resolution so ARM64EC redirect FIRES (HK loop PC 0x87fffa170c0)
(workflow w2btx8nyq, 2026-06-09. SUPERSEDES LANE-A-ARM64X-REDIRECTION-FIX-BRIEF.md — тот хардкодил RVA против
НЕВЕРНОЙ базы [встроенного view]. Корень верный, но redirect не доходил из-за module_from_pc=NULL.)

## 1. MODULE TRUTH (решено, доказательство)
ОДИН образ, не два. Metadata-несущий ARM64EC гибридный ntdll зарегистрирован на базе **0x87fff7a0000**,
machine 0xaa64, **SizeOfImage=0x860000** (доказательство: postfix-clean6/run.log:147 `scan base=0000087FFF7A0000
machine=aa64 size=00860000`; самая высокая module=база в relocate-волнах).
- PC 0x87fffa170c0 = offset **0x2770c0** в этом образе (<0x860000) → СОДЕРЖИТСЯ.
- 0x87fff910000 — НЕ зарегистрированный модуль, а встроенный x64-PE view header на +0x170000 ВНУТРИ того же
  образа. В логе только как dispatch back-scan `mod=0x87fff910000` (×59039), никогда как `module=` база.

**Почему module_from_pc(0x87fffa170c0)=NULL (macrunner_hb.c:653):** LDR-lookup (:668→:778) и raw back-scan (:680-699)
требуют `addr < p + nt.SizeOfImage` (:694). Back-scan сперва натыкается на встроенный x64-header @0x87fff910000,
но его SizeOfImage не дотягивает до PC → тест падает → continue мимо настоящей базы → исчерпание → кэш-промах
permanent (:700) → NULL. Диспетчерский back-scan (:19449-19465) проходит ТОЛЬКО потому что у него НЕТ
SizeOfImage-гейта и НЕТ miss-cache. Это и есть асимметрия — рабочий резолв уже есть, normalize им не пользуется.

**База для redirect:** redirect_arm64ec_rva (loader.c:2627) / redirect_arm64ec_proc (loader.c:2654-2660) считают
Source RVA (addr-base) И Destination (module+Destination) относительно ЕДИНОЙ настоящей DllBase=0x87fff7a0000.
Существующий хелпер macrunner_hb_redirect_arm64x_thunk_to_native (macrunner_hb.c:5751) делает ровно это:
rva=ptr-module, возвращает module+Destination. macrunner_hb_get_arm64x_metadata (:5651) валидирует CHPEMetadataPointer
∈ [module, module+SizeOfImage) — проходит ТОЛЬКО для 0x87fff7a0000, не для встроенного 0x87fff910000.
**НЕ хардкодить RVA** (0x1070c0/0xe2a60 были view-relative; против верной базы это 0x2770c0→0x2520a0, но хелпер
считает сам).

## 2. ФИКС — минимальный, переиспользует существующие хелперы

### Edit A — macrunner_hb_normalize_arm64x_x64_callback_pc (macrunner_hb.c:5824)
Резолвить metadata-несущую гибридную базу, потом звать существующий redirect-хелпер. Перед существующим
code-range блоком вставить:
```c
    void *module = macrunner_hb_module_from_pc( (void *)(uintptr_t)pc );
    if (!module || !macrunner_hb_get_arm64x_metadata( module )) {
        void *hybrid = macrunner_hb_arm64x_hybrid_base_from_pc( (void *)(uintptr_t)pc );
        if (hybrid) module = hybrid;
    }
    if (!module) return pc;
    if (macrunner_hb_get_arm64x_metadata( module )) {
        void *redir = macrunner_hb_redirect_arm64x_thunk_to_native( module, (void *)(uintptr_t)pc );
        if (redir && (uintptr_t)redir != (uintptr_t)pc) {
            /* опц. trace MACRUNNER_HB_TRACE_CALLBACK_ROUTE: entry_rva = redir-module */
            return (ULONG64)(uintptr_t)redir;
        }
    }
    /* далее существующая CodeRangesToEntryPoints нормализация (без изменений) */
```

### Edit B — новый macrunner_hb_arm64x_hybrid_base_from_pc (перед module_from_pc, ~macrunner_hb.c:652)
Back-scan по образцу диспетчерского (:19449-19465) — БЕЗ SizeOfImage-гейта и БЕЗ miss-cache — идёт назад и берёт
первую базу, для которой get_arm64x_metadata УСПЕШЕН (т.е. гибридный образ @0x87fff7a0000, пропуская встроенный
view @0x87fff910000):
```c
static void *macrunner_hb_arm64x_hybrid_base_from_pc( void *pc ) {
    uintptr_t p = (uintptr_t)pc & ~(uintptr_t)0xfff; unsigned int i;
    for (i = 0; i < 0x100000 && p >= 0x1000; i++, p -= 0x1000) {
        IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS nt;
        if (!macrunner_hb_read_local_memory( p, &dos, sizeof(dos) )) continue;
        if (dos.e_magic != IMAGE_DOS_SIGNATURE) continue;
        if (dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) continue;
        if (!macrunner_hb_read_local_memory( p + dos.e_lfanew, &nt, sizeof(nt) )) continue;
        if (nt.Signature != IMAGE_NT_SIGNATURE) continue;
        if (macrunner_hb_get_arm64x_metadata( (void *)p )) return (void *)p; /* skip embedded x64 view */
    }
    return NULL;
}
```
Переиспользует диспетчерский back-scan + готовые redirect (:5751) и metadata-reader (:5651); НЕ трогает
module_from_pc-гейты. Неверная база = безопасный no-op (redirect-гарды: metadata!=NULL, Source==rva).

## 3. ПОЧЕМУ ТЕПЕРЬ СРАБОТАЕТ
hybrid_base_from_pc вернёт 0x87fff7a0000 (встроенный 0x87fff910000 пропущен — его CHPEMetadataPointer не проходит
bound :5666). redirect_arm64x_thunk_to_native(0x87fff7a0000, 0x87fffa170c0): rva=0x2770c0, бинарный поиск
RedirectionMetadata → возвращает 0x87fff7a0000+Destination = ARM64 native impl. Normalize вернёт native PC →
диспетчер идёт в реальный ARM64-код, не ре-входит в x64 entry-thunk → SIGILL-луп СТОП → доходит до NtUserCreateWindowEx.

## 4. ПРОВЕРКА (WINEDEBUG=+virtual,+seh MACRUNNER_HB_TRACE_CALLBACK_ROUTE=1 — НЕ +relay, НЕ -all; СВЕЖАЯ дир)
1. `dispatch-pe-fallback: target=0x87fffa170c0` → **0** (было 59039)
2. `macrunner-hb-arm64x-entrypoint-normalize:` для module=ntdll ПОЯВЛЯЮТСЯ (entry_rva виден)
3. **NtUserCreateWindowEx достигнут** (было 0)
4. 0x87fffa170c0 hit-count рушится со 118090 → ~1

## ANCHORS
macrunner_hb.c:5824 normalize (Edit A); ~652 новый hybrid_base_from_pc (Edit B); reuse 5751 redirect_arm64x_thunk_to_native,
5651 get_arm64x_metadata, 19449-19465 dispatch back-scan (образец). loader.c:2627/2654-2660 (RVA-конвенция).
Evidence: postfix-clean6/run.log:147 (base 0x87fff7a0000 size 0x860000).
