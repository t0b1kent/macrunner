# UNITY-TARGETS-INTEL

Статический анализ HK + AI War 2 (без запусков). Источник данных: `*_Data/globalgamemanagers`, `*_Data/boot.config`, и наличие `MonoBleedingEdge` / `il2cpp_data`.

| Игра | Путь к `_Data` | Сценерий скриптинга | Версия Unity (`globalgamemanagers`) | boot.config / gfx-флаги | Рекомендуемый первый launch-аргумент |
|---|---|---|---|---|---|
| Hollow Knight | `/Users/timurtoby/Documents/MacRunner/Main/game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight_Data` | `MonoBleedingEdge` (Mono JIT — стандартный путь) | `6000.0.61f1` | `gfx-threading-mode=6`<br/>`wait-for-native-debugger=0`<br/>`hdr-display-enabled=0`<br/>`single-instance=`<br/>`build-guid=...` | `"<wine-runner> Hollow Knight.exe -force-d3d11 -screen-fullscreen 0 -popupwindow"` |
| AI War 2 | `/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/ai-war2-unity-corpus/extracted/AIWar2_Data` | `MonoBleedingEdge` (Mono JIT — критично, IL2CPP не используется) | `2021.3.45f2` | `gfx-enable-gfx-jobs=1`<br/>`gfx-enable-native-gfx-jobs=1`<br/>`wait-for-native-debugger=0`<br/>`hdr-display-enabled=0`<br/>`gc-max-time-slice=3` | `"<wine-runner> AIWar2.exe -force-d3d11 -screen-fullscreen 0 -popupwindow"` |

Примечание:
- `MonoBleedingEdge` присутствует в `.../MonoBleedingEdge` для обеих игр. `il2cpp_data` не обнаружен.
- `-force-d3d11 -screen-fullscreen 0 -popupwindow` не задано в `boot.config`, поэтому рекомендуется добавить явно в первый запуск для устойчивого получения окна и упрощения трекинга `WINDOW_VERDICT`.
