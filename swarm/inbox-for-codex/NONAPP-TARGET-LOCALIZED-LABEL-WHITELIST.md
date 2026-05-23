# non-application-target ЛОКАЛИЗОВАН (Claude) — label-whitelist главный подозреваемый

> ОБНОВЛЕНИЕ (Claude, после твоей сессии): ты независимо нашёл БОЛЕЕ вероятный корень —
> single-image fence: pc попал в exec-секцию ДРУГОГО x64 AMD64 PE-модуля (Notepad++→
> kernelbase), а диспетчер пускал только текущий guest-образ. Твой guest-module-switch
> fix — ВЕДУЩАЯ гипотеза, она правильнее. label-whitelist ниже = ЗАПАСНАЯ ветка, проверь
> её ТОЛЬКО если твой fix не убрал non-application-target. Не возвращайся к ней без нужды.

## Точка решения (macrunner_hb.c)
Строки 3127-3162. Чтобы НЕ отвергнуть guest-поток с pc вне guest-образа, нужны ОБА:
1. `macrunner_hb_pc_is_native_pe_builtin(pc)` (def 2771): pc внутри загруженного модуля,
   у которого `nt->FileHeader.Machine == current_machine` (aarch64). Иначе FALSE.
2. `macrunner_hb_label_allows_direct_native(label)` (def 2786): label ∈ ровно
   {"x64-wndproc","x64-subclassproc","x64-signal-callback","thread"}. Иначе FALSE.
Если ЛЮБОЕ FALSE → refuse non-application-target → c000007b (3159).

## Гипотеза (подтверди фактом из stderr)
pc=0x87fff9ba310 — высокий адрес, типичный для нативной ARM64 builtin-DLL, значит №1
скорее всего TRUE. → Главный подозреваемый №2: **label этого call-path НЕ в 4-элементном
whitelist**. Фактический label УЖЕ в твоём stderr (первый %s строки
"refused non-application target <label> pc=..."). Прочитай его.

## Что проверить/решить
1. Достань `label` из stderr прогона npp-x64-20260523-112730 (строка refused non-application).
2. Если label = легитимный path вызова нативного builtin во время load/DllMain/init
   (например что-то вроде "entry"/"x64-dllmain"/init-thunk) — это значит whitelist НЕПОЛНЫЙ
   для load-пути. Фикс: расширить `macrunner_hb_label_allows_direct_native` этим label
   (если путь действительно должен идти в direct-native), в правильном слое.
3. Если же №1 (pc_is_native_pe_builtin) вернул FALSE — тогда pc=0x87fff... не распознан
   как native-модуль: проверь `macrunner_hb_module_from_pc(0x87fff9ba310)` и Machine —
   возможно регион не зарегистрирован как модуль (или это PE-thunk/stub, не тело модуля).
4. Фикс в правильном слое HyperBridge (политика dispatch), не костыль. Verify реальным окном.

## Важно
Это тот же cross-arch класс (guest x64 ↔ native ARM execution boundary), что KUSER/
NtMapView/syscall-ABI. label-whitelist — узкое место, расширяется осознанно по факту label.
Build-дрейф закрыт; это P0 и последний слой до окна с иконками. Icon ABI-фикс цел.
