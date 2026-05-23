# РАЗВОРОТ: хватит дорабатывать build-гейт — вернись к engine-багу (P0)

## Координатор признаёт: тебя увели в инфраструктуру
Серия build-директивов утянула фокус в build-freshness гейт. Build-дрейф (prefix↔dist,
arch-skew) — РЕАЛЬНО был корнем whack-a-mole и его надо было закрыть. НО сейчас идёт
gold-plating (syswow64 sync, winsxs arch-detection) — это сверх необходимого и НЕ блокер.
Реальный блокер окна/иконок — engine: HyperBridge `non-application-target`.

## Build — «достаточно», заверши МИНИМУМ и стоп
Достаточный минимум, чтобы дрейф не вернулся (и больше НЕ расширять гейт):
- Лаунчер (`run-notepad-x64.sh` + ui-smoke) авто-синкает prefix из dist: system32 x64 +
  winsxs comctl32_v6. Один идемпотентный вызов перед запуском. ВСЁ.
- syswow64/winsxs-arch проверки в verify-build-freshness — оставь как есть, но БОЛЬШЕ не
  расширяй. Гейт уже ловит arch_pair + prefix_synced — этого хватает.
Не вкладывай больше depth-циклов в build-инфраструктуру.

## P0 — РЕАЛЬНЫЙ блокер (твой core domain)
HyperBridge отверг guest-поток как `non-application-target`:
`pc=0x87fff9ba310  entry=0x1403E5C20  status=c000007b` — во время загрузки comctl32/shell32.
Это политика трансляции: какой код HyperBridge считает application-target (транслирует)
vs нет. Поток исполнения попал в регион, который движок отказался исполнять.

Диагностика (follow the fault):
1. Что по pc=0x87fff9ba310? Какой модуль/регион (mapping), это PE-thunk / loader-stub /
   native-ARM код builtin-DLL, в который зашёл x64-гость? Какой call-site (entry 0x1403E5C20)?
2. Почему политика «non-application-target» исключает этот регион — корректно ли это
   для данного пути загрузки? (тот же класс cross-arch boundary, что KUSER/NtMapView/
   syscall-ABI — guest x64 ↔ host ARM граница исполнения).
3. Фикс в правильном слое HyperBridge (policy/боundary исполнения), не костыль.
4. Verify: окно поднимается → on-screen toolbar crop → иконки цветные (icon ABI-фикс цел).

## Принцип
Build-инфра = средство, не цель. Verified = реальное окно. Обнови ACTIVE-INVESTIGATION:
build-дрейф закрыт (detection+autosync), текущий P0 = non-application-target engine boundary.
