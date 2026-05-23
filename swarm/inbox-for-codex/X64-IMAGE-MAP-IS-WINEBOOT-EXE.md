# Конкретный x64-image = wineboot.exe (helper EXE выбран как x64, таймаутит)

## Прямой ответ на твой вопрос «назвать конкретный x64 image»
Из `reports/phase-h/npp-x64-20260523-141734/wineboot-init.err`:
- `map_image_into_view mapping PE file ...\system32\wineboot.exe at 0x140000000`
  → wineboot.exe загружен как **x64** (база 0x140000000), НЕ нативный aarch64.
- `wineboot_init_timeout secs=30 pid=88435` → `wineboot_init_bounded_failure rc=124`
  → wineboot завис и убит по таймауту. `wine-process-start` так и не достигнут (0).

## Что это значит (две линии сходятся)
Твой native-redirect УЖЕ чинит DLL-зависимости — в логе imm32/ole32/combase/rpcrt4/
cryptbase/coml2/mpr/wininet корректно резолвятся в aarch64-windows. НО **сам helper-EXE
`wineboot.exe` всё ещё выбирается как x64** и гоняется через HyperBridge. Это и есть
оставшийся `x64-image-map`. x64 wineboot в раннем bootstrap бьёт в get_builtin_unix_funcs
→ dlopen под virtual_mutex (virtual.c:868) → dyld notify-hang → 30s timeout.

## Гипотеза фикса (твой слой)
Wine-инфраструктурные helper-EXE — wineboot.exe, и проверь services.exe / explorer.exe /
rpcss / svchost / start.exe — должны грузиться как НАТИВНЫЕ aarch64-windows, НЕ x64.
Они не гостевое приложение, им незачем идти через HyperBridge. Твой redirect покрывает
DLL-deps — расширь ту же policy на выбор арки самого helper-EXE (loader path выбора
builtin EXE), чтобы wineboot/services/explorer шли aarch64.
(Параллельно остаётся латентным dlopen-под-virtual_mutex hazard, virtual.c:868 — даже если
helper'ы станут native, стоит снять mutex вокруг dlopen, чтобы x64-guest app потом не
словил тот же hang. Но первично — helper EXE arch.)

## Verify
После фикса: wineboot.exe маппится из aarch64-windows (не 0x140000000 x64), wineboot_init
НЕ таймаутит, доходит до wine-process-start → CGWindow → toolbar crop. Icon ABI-фикс цел.
