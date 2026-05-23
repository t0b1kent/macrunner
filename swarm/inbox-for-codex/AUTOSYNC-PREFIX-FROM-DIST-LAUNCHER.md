# Авто-синк prefix из dist в лаунчере (убить build-error whack-a-mole навсегда)

## Корень всей серии (подтверждено твоей же сессией 11:19→11:27)
Приложение бежит из PREFIX (`artifacts/phase-h/prefix-npp-x64-current/drive_c/windows/
system32/` + winsxs), а `make install` пишет ТОЛЬКО в dist. Лаунчер
(`scripts/run-notepad-x64.sh`, `run-notepad-x64-ui-smoke.sh`) НЕ синкает prefix из dist
→ app грузит старую DLL → missing export / c000007b. Это и был дрейф, не «ошибка сборки».

## Уже сделано (Claude, detection)
`verify-build-freshness.sh` теперь проверяет `prefix_synced_*` (prefix system32 x64 DLL
не старше dist x86_64-windows) для user32/gdi32/shell32/kernelbase/ntdll/win32u, плюс
`arch_pair_*`. Гейт ловит дрейф автоматически. Сейчас PASS (ты синкнул).

## ЗАДАЧА (твой домен — harness): сделать sync АВТОМАТИЧЕСКИМ
1. Канонический идемпотентный шаг sync dist→prefix: копировать свежие PE DLL
   (x86_64-windows → prefix system32; + winsxs arm64 common-controls v6 для comctl32_v6;
   + i386/syswow64 если используется) одним batch'ом. Вынеси в один скрипт-источник
   истины (например scripts/sync-prefix-from-dist.sh), не разбросанный copy.
2. Вызывать его в лаунчере ПЕРЕД запуском Notepad++ (и в ui-smoke), чтобы prefix ВСЕГДА
   соответствовал dist. Тогда ручной sync больше не нужен, и whack-a-mole не вернётся.
3. Лаунчер после sync (опц.) гонит verify-build-freshness и стопает с понятной ошибкой,
   если prefix_synced_* FAIL — fail fast вместо мутного no-window.
4. Проверь, что sync покрывает ВСЕ места, где prefix держит копии (system32, syswow64,
   winsxs hash-dir для comctl32_v6) — частичный sync = тот же дрейф.

## Verify
После авто-sync: один bounded прогон → окно поднимается без missing-export/c000007b →
on-screen toolbar crop (иконки цветные?). verify-build-freshness=PASS включая prefix_synced_*.

Принцип: один источник истины для деплоя (build→dist→sync prefix→run), без piecewise copy.
Icon ABI-фикс не трогать. Обнови ACTIVE-INVESTIGATION результатом окна.
