# GAME-LIBRARY-MATRIX

Статический проход (только PE-заголовки, импорты DLL, `innoextract -l` для setup-ов), без запуска exe.

| Игра | exe | PE machine | Движок | API (по движку) | API (по импортам DLL) | Блокеры x64 пути | Блокеры i386 пути | Блокер D3D9-слоя | Готовность таргета | Очередь запуска |
|---|---|---|---|---|---|---|---|---|---|---|
| Hollow Knight | `game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe` | AMD64 | Unity | **D3D11** (Unity runtime) | Не детектируется через IAT (динамический рендер) | — | — | Не применяется | Распакован | **№1** |
| AI War 2 | `MacRunner/artifacts/ai-war2-unity-corpus/extracted/AIWar2.exe` | AMD64 | Unity | **D3D11** (Unity runtime) | `unityplayer.dll`; `kernel32.dll` | — | — | Не применяется | Распакован (lane D canonical corpus) | **№2** |
| Diablo/Hellfire | `Diablo.Hellfire-Rutracker/extracted/Diablo.exe` | I386 | Diablo legacy | **DirectDraw** | Не детектируется через IAT (`diabloui.dll`, `storm.dll` runtime) | — | x86-only: PE32 требуется | Не применяется | Распакован | **№3** |
| Diablo/Hellfire | `Diablo.Hellfire-Rutracker/extracted/hellfire/hellfire.exe` | I386 | Diablo legacy | **DirectDraw** | Не детектируется через IAT (`hellfrui.dll`, `storm.dll` runtime) | — | x86-only: PE32 требуется | Не применяется | Распакован | **—** |
| Terraria (GOG) | `Terraria_v1.4.5.6_(89298)_Win_[GOG]/extracted/Terraria.exe` | I386 | XNA/.NET | **D3D9** (managed/runtime path) | `mscoree.dll` | — | x86-only: PE32 требуется | **Ожидает (Lane D — D3D9 слой)** | Распакован | **№4** |
| Half-Life | `MacRunner/artifacts/games/half-life/Half-Life/hl.exe` | I386 | GoldSrc | **OpenGL + D3D8 (в зависимости от конфига/плагина)** | Не детектируется через IAT (`steam_api.dll`/`tier0.dll`/`vstdlib.dll` в ряде exe-веток) | — | x86-only: PE32/legacy-путь | Не применяется | Распакован | **№5** |
| GTA VC | `MacRunner/artifacts/games/gta-vc-disc/autorun.exe` | I386 | RenderWare (геймплейная часть, inferred) | **D3D8** | Не детектируется через IAT (setup/launcher) | — | x86-only ожидание интеграции пакета | **Не применимо** | Инсталлер (`GTA VICE CITY 1C` пока как `lolz`) | **№6** |
| Playdead: Limbo | `[dixen18] Playdead's Games/01. LIMBO (2011)/Setup.exe` | I386 | Installer (установка/обертка) | Н/Д | `oleaut32.dll`; `advapi32.dll`; `user32.dll`; `kernel32.dll`; `comctl32.dll` | — | x86-only (setup) | Н/Д | Инсталлер | — |
| Playdead: INSIDE | `[dixen18] Playdead's Games/02. INSIDE (2016)/Setup.exe` | I386 | Installer (установка/обертка) | Н/Д | `oleaut32.dll`; `advapi32.dll`; `user32.dll`; `kernel32.dll`; `comctl32.dll` | — | x86-only (setup) | Н/Д | Инсталлер | — |
| AI War 2 DLC/Installers | `game-ai.war.2-(91319)/setup_ai_war_2_*.exe` | I386 | Installer (InnoSetup, setup data) | Н/Д | (в основном `*.bin`-сегменты + ISI redist), игровые бинари в `extracted/AIWar2.exe` | — | Н/Д | Н/Д | Имеются setup-ы; готовый игровой бинарь — в `MacRunner/artifacts/ai-war2-unity-corpus/extracted` | — |

## utility/extras

| Тип | Путь | `innoextract -l` | Рекомендуемое состояние `extracted` |
|---|---|---|---|
| Inno setup (GOG) | `Terraria_v1.4.5.6_(89298)_Win_[GOG]/setup_terraria_v1.4.5.6_(89298).exe` | ✅ InnoSetup 5.6.2 (unicode) | Уже распакован рядом: `Terraria_v1.4.5.6_(89298)_Win_[GOG]/extracted/` |
| Inno setup (legacy) | `Diablo.Hellfire-Rutracker/setup_diablo_1.09_hellfire_v4_(78466).exe` | ✅ InnoSetup 5.6.2 (unicode) | Основной game-артефакт уже в `Diablo.Hellfire-Rutracker/extracted/` |
| Inno setup (legacy mod) | `Diablo.Hellfire-Rutracker/setup_diablo_1_hd_mod_(belzebub)_1.045_(83650).exe` | ✅ InnoSetup 5.6.2 (unicode) | Извлечен (в той же `Diablo.Hellfire-Rutracker/extracted/` ветке, которую уже распаковал основной инсталлер) | 
| Inno setup (AI War 2) | `game-ai.war.2-(91319)/setup_ai_war_2_5.812_(64bit)_(91319).exe` | ✅ InnoSetup 5.6.2 (unicode) | Игра уже распакована в lane D corpus (`MacRunner/artifacts/ai-war2-unity-corpus/extracted/AIWar2.exe`) |
| Inno setup (AI War 2 DLC) | `game-ai.war.2-(91319)/dlc/setup_ai_war_2_*.exe` | ✅ InnoSetup 5.6.2 (unicode) | Локально распакован в `game-ai.war.2-(91319)/dlc/extracted/` (по-игровому бинарю используем `MacRunner/artifacts/ai-war2-unity-corpus/extracted/AIWar2.exe`) |
| DXSETUP / redist | `Diablo.Hellfire-Rutracker/extracted/__redist/DirectX/DXSETUP.exe` | — | Имеется рядом с `extracted`-игрой |
| DXSETUP / redist | `MacRunner/artifacts/games/gta-vc-disc/DirectX/DXSETUP.exe` | — | Имеется рядом с диском `gta-vc-disc` |
| .NET redist | `Terraria_v1.4.5.6_(89298)_Win_[GOG]/extracted/__redist/dotNet4/dotNetFx40_Full_x86_x64.exe` | — | Имеется рядом с extracted |

Примечание:
- «API (по импорту)» в этой матрице намеренно полезна только как дополнение: многие движки грузят рендер-провайдеры через runtime и late-binding.
- «Очередь запуска» — приоритет после уже взятого HK, для текущей очереди после окна.
