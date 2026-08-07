# FIX BRIEF — Lane A: HK x64 бесконечный HB dispatch-fallback луп (workflow w4e8o7e56, 2026-06-09)

## 1. КОРЕНЬ (source-verified, НЕ логи)
Thread-start globals УЖЕ корректный ARM64: load_ntdll() (loader.c:2773) держит machine=current_machine=ARM64,
т.к. is_arm64ec()=FALSE для HK x64 guest → pLdrInitializeThunk/pRtlUserThreadStart резолвятся из
aarch64-windows/ntdll.dll (real ARM64 .text). signal_arm64.c:3532/3575 входит в ARM64 LdrInitializeThunk.
ЭТО НЕ БАГ — прошлые попытки Lane A целили сюда зря.
Реальный fault-surface: ВТОРОЙ полный x86_64 ntdll грузится как wow64-ntdll — get_machine_wow64_dir(AMD64)
возвращает system32 (не NULL) при macrunner_hb_x64_guest_process() (loader.c:2199-2202), и load_wow64_ntdll
(loader.c:3035) мапит весь x64-образ, включая DISCARDABLE .debug_* (Characteristics 0x42000040:
DISCARDABLE|INIT_DATA|READ, без EXECUTE) — DWARF-байты, замаплены readable. Когда шальной guest control-transfer
резолвится в этот DWARF VA (base + RVA 0x2770C0 = 0x87fffa170c0) → байты не-код → SIGILL.
Луп самоподдерживается: ДВА PE-header fallback'а промоутят битый PC обратно в «x64 guest code» ТОЛЬКО по
nt->FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64, ПРОПУСКАЯ exec-section гейт, который нормальный классификатор
macrunner_hb_module_pc_is_x64_guest_code() (macrunner_hb.c:5738 → macrunner_hb_pc_in_executable_section) уже
применяет. Диспетчер ре-резолвит тот же фикс. DWARF-таргет вечно (константный PC, 59K dispatch-pe-fallback).
Прошлый фикс не сел, т.к. трогал pLdrInitializeThunk/redirect_ntdll_functions_hb (уже ARM64 / уже скипнут на
loader.c:2796-2800) — не трогал недо-валидированные fallback'и, единственный путь к битому VA.

## 2. ФИКС (минимальный — добавить exec-section гейт в оба/три fallback'а)
Хелпер готов и корректен: macrunner_hb_pc_in_executable_section(void *module, uint64_t pc) @ macrunner_hb.c:5608
(режет любой RVA не в IMAGE_SCN_MEM_EXECUTE секции). Сейчас static.

Site A — macrunner_hb.c:19457 (macrunner_hb_dispatch_x64_callback, в том же TU):
    if (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        macrunner_hb_pc_in_executable_section( (void *)p, (uint64_t)target )) {
        if (!target_module) target_module = (void *)p;
        is_amd64_pe = TRUE;
    }
Провал гейта → существующий reject if(!is_amd64_pe) @:19464 (Phase F rejected, return 0).

Site B — signal_arm64.c:997-998 (macrunner_hb_route_x64_callback_fault, SIGILL fallback):
    if (mod && (nt = macrunner_hb_native_fault_nt_header( mod )) &&
        nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        macrunner_hb_pc_in_executable_section( mod, (uint64_t)raw_pc )) { x16_is_guest=TRUE; raw_is_guest=TRUE; ... }
Сними static с macrunner_hb_pc_in_executable_section (macrunner_hb.c:5608) + добавь
  extern int macrunner_hb_pc_in_executable_section(void *module, uint64_t pc);
в extern-блок signal_arm64.c:772-781. Провал → существующий return FALSE, SIGILL всплывает как обычное исключение.

Site C — signal_arm64.c:1044-1046 (SEGV-early-init fallback, латентный): тот же гейт
  macrunner_hb_pc_in_executable_section( mod, entry ).

Defense-in-depth (опц., ниже приоритет): early-return в load_wow64_ntdll для macrunner_hb_x64_guest_process()
чтобы сузить trap-surface. НЕ обязательно — section-гейт необходим и достаточен. Только если guest доказуемо
не нуждается в wow64-ntdll образе.

## 3. ПОЧЕМУ ОСТАНАВЛИВАЕТ ЛУП
RVA 0x2770C0 в .debug_* без IMAGE_SCN_MEM_EXECUTE → с гейтом pc_in_executable_section()=FALSE → НИ ОДИН fallback
не ставит is_amd64_pe/raw_is_guest=TRUE → путь к DWARF VA закрыт: dispatch_x64_callback → Phase F rejected→return 0;
route_x64_callback_fault → return FALSE. Фолт доставляется ОДИН раз как настоящий SIGILL, не ре-диспатчится в тот
же таргет. Константный-PC ре-резолв (59K луп) больше невозможен. Совпадает с section-aware классификатором (5738).

## 4. ПРОВЕРКА (WINEDEBUG=+virtual,+relay,+seh — НЕ -all)
- grep -c 'macrunner-hb-dispatch-pe-fallback' → 0 (было 59039)
- grep -c 'macrunner-hb-sigill-pe-fallback' → 0 (было ~118090)
- grep -c '0x87fffa170c0' → 0
- grep -E 'Phase F rejected|NtUserCreateWindowEx' → максимум один Phase F rejected, затем NtUserCreateWindowEx
  (HK дошёл до создания окна).

ФАЙЛЫ: macrunner_hb.c:19457 (+ гейт :5608 снять static); signal_arm64.c:997-998 и :1044-1046 (+ extern :772-781).
НЕ ТРОГАЙ: loader.c:2773, :2796-2800, signal_arm64.c:3532/3575 — уже корректный ARM64.
