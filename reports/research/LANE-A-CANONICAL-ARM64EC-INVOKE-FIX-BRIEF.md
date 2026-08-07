# FIX BRIEF — HK x64: native ARM64EC entry-thunk без канонического dispatch-ABI → ЗАВИСАНИЕ (не фолт)
(workflow wb38e0nve, 2026-06-10. Поправка: это НЕ per-address whack-a-mole, а HANG.)

## Поправка к посылке (verified по логу)
c0000005/SIGILL на 0x87fff9f0500 = ОЖИДАЕМЫЙ entry-signal, роутер ловит ОК (stage=after-abi result=OK). JIT гонит
5-инстр FFS-thunk (488bc448895820555de9…), его jmp садится на native-impl 0x87fff9c7418 (rva 0xe7418 = native тело
LdrInitializeThunk). Последняя строка: `stage=loop block=1 pc=0x87fff9c7418`, exit=143 (watchdog) — процесс ВИСИТ
там, НЕ фолтит. Симптом = hang, не fault.

## 1. КОРЕНЬ
Редиректнутый ARM64EC native-impl (0xe7418, native LdrInitializeThunk) входит БЕЗ канонического ARM64EC dispatch
ABI/CHPE-контекста, И через путь для ВОЗВРАЩАЮЩИХСЯ callback'ов, а не TAIL-transfer thread-entry. Две дыры:
- ABI: macrunner_hb_arm64_pe_call12 делает `blr x20` (macrunner_hb.c:6542), загрузив только x0-x7 + x18=TEB
  (:6500-6541). Не ставит x9 (callee), x10 (exit-thunk), x11 (dispatch) и не гонит EcCodeBitMap+FFS-декод. CHPE
  $ientry_thunk-пролог ждёт именно этот register-state → со stale x9/x11 дереф мусора / спин.
- Dispatch-shape: ветка macrunner_hb.c:20226-20238 гонит pc=0xe7418 в macrunner_hb_call_direct_native_target
  (:19325), который на :19345-19346 читает return-адрес с rsp и маршалит return-value = трактует LdrInitializeThunk
  как leaf-callback, что возвращается. А это thread-init трамплин: должен бежать в свой хвост (RtlUserThreadStart),
  не возвращается → бесконечный park, без фолта.

## 2. ФИКС — зеркалить arm64x_check_call, ОДНИМ местом
КАНОН-референс: arm64x_check_call — engine/wine/dlls/ntdll/signal_arm64ec.c:1965-2052. Контракт:
- caller ставит x9=callee, x10=exit-thunk ($iexit_thunk$…), x11=dispatch target, x0-x7=args, x18=TEB;
- тестит peb->EcCodeBitMap (PEB+0x368) по биту dest>>12 / слову dest>>18 (:1970-1976);
- если EC-код + FFS-последовательность (.Lffwd_seq :2033-2039 байт-в-байт = HK 488bc44889582055 5de9…) →
  x11 = entry + 14 + sxtw(rel32) (:1990-1992), возвращает native-impl адрес на blr;
- иначе → exit-thunk (:2026-2028).

Применить ОДНИМ сайтом, две согласованные правки:
(1) Gate (macrunner_hb.c:6500-6542, основной): перед `blr x20` прогнать arm64x_check_call-декод на ИСХОДНОМ
    x64 entry-thunk адресе (не пре-редиректнутом dest): загрузить peb->EcCodeBitMap, тест бита, на FFS-пути
    x11 = entry+14+sxtw(rel32) и `blr x11` с x0-x7 + x18=TEB (+ безусловно заполнять teb->ChpeV2CpuAreaInfo,
    обобщая per-thunk патч :18075). Предпочтительно — звать ntdll-диспетчер metadata->__os_arm64x_dispatch_icall
    (заполнен loader.c:2626-2677) с x9/x10/x11 — пусть in-module декодер сделает (ровно как RtlUserThreadStart
    signal_arm64ec.c:2155-2174 делает для BaseThreadInitThunk).
(2) Dispatch-shape для thread-entry (macrunner_hb.c:20226-20238 + :19345): когда label="x64-signal-callback" И
    resolved target = thread/loader entry (LdrInitializeThunk/RtlUserThreadStart native, т.е. FFS-редиректнутый
    0xe7418) — НЕ гнать через call_direct_native_target (он читает return-addr :19345-19346 и ждёт return).
    TAIL-TRANSFER в native-entry, чтобы он владел тредом и сам добежал до RtlUserThreadStart.

## 3. ПОЧЕМУ ЗАКРЫВАЕТ WHACK-A-MOLE
0x87fff9f0500, 0x87fff9ea0c0 и пропатченный 0x87fffa170c0 — все ОДНА FFS entry-thunk-форма по sibling-адресам.
EcCodeBitMap+FFS-декод АДРЕС-АГНОСТИЧЕН: считает верный native-impl + dispatch-регистры из битмапа и байтов thunk'а
для ЛЮБОГО — каждый sibling достигается с тем register-state, что ждёт его CHPE-пролог, без per-address x18/ChpeV2.
Плюс tail-transfer для thread-entry → native LdrInitializeThunk бежит на своём фрейме до конца → RtlUserThreadStart
→ BaseThreadInitThunk → app entry → создание окна, вместо park'а в returning-callback обёртке.

## 4. ПРОВЕРКА (WINEDEBUG=+seh — НЕ +relay/-all)
- c0000005/SIGILL на 0x87fff9f* (и 0x87fff9ea0c0) → 0 (entry-signal может мелькнуть раз, но без повторных фолтов
  и без hang на pc=0x87fff9c7418).
- RtlUserThreadStart достигнут (было 0).
- NtUserCreateWindowEx достигнут (было 0).
- loop проходит за stage=loop block=1 pc=0x87fff9c7418 (before-run block=2 / thread hand-off), нет exit=143.
- classify_run на новом прогоне.

## ANCHORS
macrunner_hb.c:6500-6542 (blr x20 @6542, gate-фикс); :20226-20238 + call_direct_native_target :19325
(return-addr read :19345-19346); direct-native alt :19571-19586; redirect resolver (оставить) :5751,
.hexpthk-байты :5941-5947; per-thunk ChpeV2 для обобщения :18075.
signal_arm64ec.c:1965-2052 arm64x_check_call (FFS .Lffwd_seq :2033-2039, EcCodeBitMap :1970-1976,
x11=entry+14+rel :1990-1992); RtlUserThreadStart caller-contract :2155-2174.
loader.c:2626-2677 (__os_arm64x_dispatch_icall slot).
Evidence: reports/phase4-hollow-knight/laneA-callback-run-probe-20260610-031552/run.log (last `stage=loop
block=1 pc=0x87fff9c7418`, exit=143 = hang).
