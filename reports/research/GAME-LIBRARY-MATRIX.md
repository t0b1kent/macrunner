# GAME-LIBRARY-MATRIX

Статический проход (только PE-заголовки, импорты DLL, `innoextract -l` для setup-ов), без запуска exe.

Текущее состояние по lane-срезу:
- HK: x64 (Unity 6000) уже на DXGI + пути окна; окно-критерий готовится, для старт-пайплайна включён DXMT `Present`.
- `hl.exe` и `Diablo` i386 уже в L6-trace: ожидаем PE32/WOW64/BTCpu путь до окна.
- Инициализация `ai-war2-unity-corpus` подтверждена через `_Data`: Mono JIT (не IL2CPP).
- Блокер `audio-native-ARM64` по аудио-цепочке закрыт.

| Игра | exe | PE machine | Движок | API (по движку) | API (по импортам DLL) | Блокеры x64 пути | Блокеры i386 пути | Блокер D3D9-слоя | Готовность таргета | Очередь запуска |
|---|---|---|---|---|---|---|---|---|---|---|
| Hollow Knight | `game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe` | AMD64 | Unity | **D3D11** (Unity runtime) | Не детектируется через IAT (динамический рендер) | — | — | Не применяется | Распакован, **R0: DXGI+окно** | **№1** |
| AI War 2 | `MacRunner/artifacts/ai-war2-unity-corpus/extracted/AIWar2.exe` (`game-ai.war.2-(91319)/extracted/AIWar2.exe`) | AMD64 | Unity | **D3D11** (Unity runtime) | `unityplayer.dll`; `kernel32.dll` | — | — | Не применяется | Результат статички: Mono JIT, версия `2021.3.45f2`, queue-ready после HK | **№2** |
| Diablo/Hellfire | `Diablo.Hellfire-Rutracker/extracted/Diablo.exe` | I386 | Diablo legacy | **DirectDraw** | Не детектируется через IAT (`diabloui.dll`, `storm.dll` runtime) | — | x86-only: PE32/WOW64 требуется | Не применяется | Распакован, **R1: L6-окно (WOW64+BTCpu)** | **№3** |
| Diablo/Hellfire | `Diablo.Hellfire-Rutracker/extracted/hellfire/hellfire.exe` | I386 | Diablo legacy | **DirectDraw** | Не детектируется через IAT (`hellfrui.dll`, `storm.dll` runtime) | — | x86-only: PE32/WOW64 требуется | Не применяется | Распакован (variant) | **—** |
| Half-Life | `MacRunner/artifacts/games/half-life/Half-Life/hl.exe` | I386 | GoldSrc | **OpenGL + D3D8 (по конфигу/плагинам)** | Не детектируется через IAT (`steam_api.dll`/`tier0.dll`/`vstdlib.dll` в ряде exe-веток) | — | x86-only: PE32/legacy-путь | Не применяется | Распакован, L6 window-окно в пределах одного lane с Diablo | **№4** |
| Terraria (GOG) | `Terraria_v1.4.5.6_(89298)_Win_[GOG]/extracted/Terraria.exe` | I386 | XNA/.NET | **D3D9** (managed/runtime path) | `mscoree.dll` | — | x86-only: PE32 требуется | **Ожидает (Lane D — D3D9 слой)** | Распакован, redist есть, ждёт слой D3D9 | **№5** |
| GTA VC | `MacRunner/artifacts/games/gta-vc-disc/autorun.exe` | I386 | RenderWare (геймплейная часть, inferred) | **D3D8** | Не детектируется через IAT (setup/launcher) | — | x86-only: legacy-пакет | **Не применимо** | Инсталлер/launcher, авторун и DirectX redist присутствуют; распаковка/прогон ещё не прогнан | **№6 (после hl/D3D9-коридора)** |
| Playdead: Limbo | `[dixen18] Playdead's Games/01. LIMBO (2011)/Setup.exe` | I386 | Installer (установка/обертка) | Н/Д | `oleaut32.dll`; `advapi32.dll`; `user32.dll`; `kernel32.dll`; `comctl32.dll` | — | x86-only (setup) | Н/Д | Инсталлер | — |
| Playdead: INSIDE | `[dixen18] Playdead's Games/02. INSIDE (2016)/Setup.exe` | I386 | Installer (установка/обертка) | Н/Д | `oleaut32.dll`; `advapi32.dll`; `user32.dll`; `kernel32.dll`; `comctl32.dll` | — | x86-only (setup) | Н/Д | Инсталлер | — |
| AI War 2 DLC/Installers | `game-ai.war.2-(91319)/setup_ai_war_2_*.exe` | I386 | Installer (InnoSetup, setup data) | Н/Д | (в основном `*.bin`-сегменты + ISI redist), игровые бинари в `extracted/AIWar2.exe` | — | Н/Д | Н/Д | Имеются setup-ы; канонический бинарь берём из `MacRunner/artifacts/ai-war2-unity-corpus/extracted` | — |

## Ближайшая очередь после HK/hl (по близости к уже закрытым lane)

| № | Тайтл | Логика близости |
|---|---|---|
| 2 | AI War 2 | `Unity + D3D11 + x64` — тот же графический трек, что у HK |
| 3 | Diablo/Hellfire | `i386 + PE32/WOW64+Btcpu + окно` — тот же lane-чекпоинт, что у hl.exe |
| 4 | GTA VC | `RenderWare + D3D8 + setup/launcher` — ближайший по legacy i386/редистам к hl |

## Run-обвязки + ожидаемые marker-ladders (pipeline-ready, без запусков)

> Формат маркеров — как в PE32/D3D harness: `after-loader`, wow64/BTCpu, D3D-маркерный набор и окно.

### 1) AI War 2 (Unity x64 + D3D11)

```sh
AIWAR2_EXE="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/ai-war2-unity-corpus/extracted/AIWar2.exe"
RUNDIR="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/queue/ai-war2"
MACRUNNER_GRAPHICS_BACKEND=dxmt \
MACRUNNER_HB_TRACE_D3D=1 \
MACRUNNER_HB_TRACE_CREATEWINDOW=1 \
WINEDEBUG=fixme+all,seh+all,virtual+trace,loaddll+warn \
MACRUNNER_MR_RUN_START_SERVICES=1 \
scripts/run-windows-app.sh "$AIWAR2_EXE" --timeout 180 --lane arm64-hyperbridge --d3d-backend metal --d3d-trace --json "$RUNDIR/run.json" -- -force-d3d11 -screen-fullscreen 0 -popupwindow
```

Ожидаемая лестница:
1. `macrunner-hb-ldr-init.*phase=after-loader`
2. `dxgi.dll`/`d3d11.dll` module_map + `CreateDXGIFactory` + `D3D11CreateDevice`
3. `CreateSwapChain`/`Present`
4. `create_window` + `macdrv_create_win_data win`
5. `window-verdict: PASS` после capture/triage

### 2) Diablo/Hellfire (i386 PE32/WOW64 lane)

```sh
MACRUNNER_DIABLO_EXE="/Users/timurtoby/Documents/MacRunner/Main/Diablo.Hellfire-Rutracker/extracted/Diablo.exe"
MACRUNNER_LANE_C_MATRIX_TARGETS=diablo_i386_native \
scripts/lane-c-real-software-matrix.sh
```

Ожидаемая лестница:
1. `macrunner-hb-ldr-init` + `phase=after-loader`
2. `Wow64`/`macrunner-wow64` + `BTCpu`
3. `loader` markers
4. окно: `NtUserCreateWindowEx` / `create_window` / `create_window_call`

### 3) GTA VC (i386 RenderWare + D3D8 installer path)

```sh
DXSETUP="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/games/gta-vc-disc/DirectX/DXSETUP.exe"
AUTORUN="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/games/gta-vc-disc/autorun.exe"
RUNDIR="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/queue/gta-vc"

MACRUNNER_GRAPHICS_BACKEND=dxmt \
MACRUNNER_HB_TRACE_D3D=1 \
WINEDEBUG=fixme+all,seh+all,virtual+trace,loaddll+warn \
scripts/run-windows-app.sh "$DXSETUP" --timeout 180 --lane arm64-hyperbridge --json "$RUNDIR/dxsetup.json"

MACRUNNER_GRAPHICS_BACKEND=dxmt \
MACRUNNER_HB_TRACE_D3D=1 \
MACRUNNER_HB_TRACE_CREATEWINDOW=1 \
WINEDEBUG=fixme+all,seh+all,virtual+trace,loaddll+warn \
scripts/run-windows-app.sh "$AUTORUN" --timeout 240 --lane arm64-hyperbridge --json "$RUNDIR/autorun.json"
```

Ожидаемая лестница:
1. `macrunner-hb-ldr-init: after-loader`
2. `Wow64`/`BTCpu`
3. D3D8/D3D/`wined3d`-маркер после install/runtime-init
4. окно: `macdrv_create_win_data win`

### Проверка фактической доступности таргетов (перед пробегом)

- Hollow Knight: `.../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe` — есть  
- AI War 2: `.../MacRunner/artifacts/ai-war2-unity-corpus/extracted/AIWar2.exe` — есть  
- Diablo: `.../Diablo.Hellfire-Rutracker/extracted/Diablo.exe` — есть  
- Hellfire: `.../Diablo.Hellfire-Rutracker/extracted/hellfire/hellfire.exe` — есть  
- Half-Life: `.../MacRunner/artifacts/games/half-life/Half-Life/hl.exe` — есть  
- Terraria: `.../Terraria_v1.4.5.6_(89298)_Win_[GOG]/extracted/Terraria.exe` — есть  
- GTA VC: `.../MacRunner/artifacts/games/gta-vc-disc/autorun.exe` — есть  
- GTA VC DXSETUP: `.../MacRunner/artifacts/games/gta-vc-disc/DirectX/DXSETUP.exe` — есть

## utility/extras

| Тип | Путь | `innoextract -l` | Рекомендуемое состояние `extracted` |
|---|---|---|---|
| Inno setup (GOG) | `Terraria_v1.4.5.6_(89298)_Win_[GOG]/setup_terraria_v1.4.5.6_(89298).exe` | ✅ InnoSetup 5.6.2 (unicode) | Уже распакован рядом: `Terraria_v1.4.5.6_(89298)_Win_[GOG]/extracted/` |
| Inno setup (legacy) | `Diablo.Hellfire-Rutracker/setup_diablo_1.09_hellfire_v4_(78466).exe` | ✅ InnoSetup 5.6.2 (unicode) | Основной game-артефакт уже в `Diablo.Hellfire-Rutracker/extracted/` |
| Inno setup (legacy mod) | `Diablo.Hellfire-Rutracker/setup_diablo_1_hd_mod_(belzebub)_1.045_(83650).exe` | ✅ InnoSetup 5.6.2 (unicode) | Извлечен (в `Diablo.Hellfire-Rutracker/extracted/` с общим артефактом) |
| Inno setup (AI War 2) | `game-ai.war.2-(91319)/setup_ai_war_2_5.812_(64bit)_(91319).exe` | ✅ InnoSetup 5.6.2 (unicode) | Игра уже доступна в canonical lane-D corpus: `MacRunner/artifacts/ai-war2-unity-corpus/extracted/AIWar2.exe` |
| Inno setup (AI War 2 DLC) | `game-ai.war.2-(91319)/dlc/setup_ai_war_2_*.exe` | ✅ InnoSetup 5.6.2 (unicode) | Локально распакован в `game-ai.war.2-(91319)/dlc/extracted/`; gameplay-бинарь берём из `MacRunner/artifacts/ai-war2-unity-corpus/extracted` |
| DXSETUP / redist | `Diablo.Hellfire-Rutracker/extracted/__redist/DirectX/DXSETUP.exe` | — | Имеется рядом с `extracted`-игрой |
| DXSETUP / redist | `MacRunner/artifacts/games/gta-vc-disc/DirectX/DXSETUP.exe` | — | Имеется рядом с диском `gta-vc-disc` |
| .NET redist | `Terraria_v1.4.5.6_(89298)_Win_[GOG]/extracted/__redist/dotNet4/dotNetFx40_Full_x86_x64.exe` | — | Имеется рядом с extracted |

Примечание:
- `API (по импорту)` — это дополнение к `API (по движку)`: многие рендер-пути late-binding.
- Оконные/маркеры в этой матрице нужны именно как лестница готовности к авто-прогону после закрытия текущих блокеров.
