# wineboot hang = HB module_from_pc spin (VERIFIED из стека, НЕ service-event)

## Источник: проверенный факт из твоего сэмпла
`reports/phase-h/MILESTONE/sample-wineboot-services-hang-20260523-145032/wineboot-92320.sample.txt`

- **Thread 1 (main):** `__wine_main → CFRunLoopRun → mach_msg` — просто стоит в рунлупе.
- **Thread 2 (виновник, 570+ сэмплов в одной точке):**
```
0x140013f84  (x64 guest code, база 0x140000000)
  → _sigtramp (доставлен СИГНАЛ/fault)
    → macrunner_hb_primary_signal_handler
      → macrunner_hb_route_x64_callback_fault
        → macrunner_hb_pc_is_x64_guest_code
          → macrunner_hb_module_from_pc
            → mach_vm_read_overwrite   ← застрял/крутится здесь (570 сэмплов)
```
- services.exe: `NtWaitForSingleObject → server_wait/server_select → read` = ШТАТНЫЙ idle
  сервиса, НЕ дедлок. Значит service-handshake — это симптом, не корень.

## Что это значит (verified, без догадок о «почему»)
wineboot --init не завершается, потому что worker-поток **крутится в
`macrunner_hb_module_from_pc` → `mach_vm_read_overwrite`** внутри обработчика x64-fault'а.
Это похоже на fault-storm / патологический memory-scan: x64-код на `0x140013f84`
фолтит, каждый fault входит в HB signal handler, module_from_pc сканирует память через
mach_vm_read_overwrite и не выходит (или крайне медленно).

ПРОТИВОРЕЧИЕ для проверки: wineboot должен быть native aarch64, но в стеке исполняется
x64-код на 0x140013f84 → либо native-helper redirect НЕ полностью покрыл этот wineboot
(остался x64-регион), либо fault на x64-адрес мис-роутится в HB вместо нативной обработки.

## Куда смотреть (твой слой, твоё решение)
1. Почему в native-aarch64 wineboot исполняется x64-код на 0x140013f84 (redirect неполный?).
2. `macrunner_hb_module_from_pc` — почему спин в mach_vm_read_overwrite: линейный скан без
   терминации? повторный re-entry на каждый fault (storm)? Добавь bound/кэш/ранний выход.
3. НЕ start from service/__wineboot_event — стек показывает, что services штатно idle.

## Verify
После фикса: thread 2 не висит в module_from_pc/mach_vm_read_overwrite, wineboot --init
завершается → wine-process-start → CGWindow → toolbar crop. Icon ABI-фикс цел.

(Это VERIFIED из сэмпла — стек, не гипотеза механизма. Если twой свежий прогон уже
показал иную картину — доверяй свежему evidence.)
