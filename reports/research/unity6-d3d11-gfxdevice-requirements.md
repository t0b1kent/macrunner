# Unity 6000.x D3D11 GfxDevice: создание девайса + грабли translation-layer'ов

**Цель:** авторитетный ресёрч с цитированием первоисточников — что Unity 6000.x требует на этапе создания D3D11 GfxDevice, и почему Unity-D3D11-игры падают на translation-layer'ах (DXVK, wined3d, Proton, Apple GPTK/D3DMetal, CrossOver, DXMT).

**Контекст задачи:** x64 Unity 6000.0.61f1 (Hollow Knight) на Apple Silicon через ARM64 Wine + нативный ARM64X DXMT (D3D11→Metal). DXMT работает; Unity доходит до `GfxDevice: creating device client` и зовёт NULL-указатель функции рядом со строкой `force-d3`. 4 эксперимента опровергли vendor / API-selection / threading.

**Легенда тегов:**
- **[ФАКТ]** — прямо подтверждено процитированным первоисточником (Unity docs/staff, Microsoft docs, исходники DXVK/DXMT, баг-трекеры).
- **[ВЫВОД]** — логический вывод из процитированных фактов.
- **[?]** — правдоподобно, но не подтверждено первоисточником (UnityPlayer и Unity-бэкенд D3D11 закрыты, точные оффсеты/IAT не опубликованы).

**Важная оговорка:** UnityPlayer D3D11-бэкенд — closed source. Публичного IDA/Ghidra-разбора UnityPlayer 6000.0.61f1 с точными оффсетами функ-поинтеров рядом со строкой `force-d3d11` найти не удалось. Поэтому самые «внутренние» утверждения помечены [ВЫВОД]/[?], а не [ФАКТ].

---

## TL;DR — ранжированные гипотезы под твой кейс

1. **(Самый сильный лид) Unity 6 требует `ID3D11Fence` (`GpuFence::Create()` → `ID3D11Device5::CreateFence`).** Это регрессия именно Unity 6, ломающая запуск на Wine→Metal-стеках, где fence-интерфейс не реализован. У Unity есть официальный баг-трекер на это (`...failed-to-create-gpufence`), плюс воспроизведение на CrossOver/Whisky с дословным логом `GpuFence::Create(): Failed to create ID3D11Fence`. **[ФАКТ]** на существование требования; **[ВЫВОД]** на то, что это твоя причина.
2. **Архитектурный/loader-mismatch: x64 UnityPlayer ↔ нативный ARM64X DXMT.** Указатель из `GetProcAddress` указывает на ARM64-код без валидных x64-callable EC-thunk'ов → вызов через «битый» поинтер на первом же cross-domain вызове, ровно после `creating device client`. **[ВЫВОД, высокая уверенность]** — но частично противоречит твоему наблюдению «DXMT работает, NULL внутри Unity-логики», поэтому держим как №2.
3. **D3D11On12 / D3D11↔D3D12 interop.** Unity 6 по умолчанию тяготеет к D3D12; некоторые сборки создают D3D11-девайс поверх D3D12 (`D3D11On12CreateDevice`). Если слой отдаёт stub/`E_NOTIMPL`/NULL — креш на старте. **[ФАКТ]** на механизм; **[?]** какие подсистемы 6000.0.61f1 это триггерят.
4. **Отсутствующий экспорт у proxy/неполной d3d11.dll/dxgi.dll → `GetProcAddress`=NULL → NULL-вызов.** Классика, но DXVK/DXMT/wined3d сейчас экспортят весь нужный Unity набор, так что для них это менее вероятно, чем для самопального wrapper'а. **[ФАКТ]**.

---

## 1. Экспорты d3d11.dll / dxgi.dll, нужные Unity, и статический импорт vs GetProcAddress

### 1.1. Минимально необходимый набор экспортов

**[ФАКТ]** Из `d3d11.dll` для создания девайса Unity использует `D3D11CreateDevice` и/или `D3D11CreateDeviceAndSwapChain`; на D3D12-interop-пути — `D3D11On12CreateDevice`. Из `dxgi.dll` — `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2` (пробуются от новых к старым). Microsoft сами проектируют эти точки входа под динамическую линковку: для `D3D11CreateDevice` поставляется typedef `PFN_D3D11_CREATE_DEVICE` именно «чтобы использовать GetProcAddress вместо статической линковки».
(MS: https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-d3d11createdevice ; D3D11On12: https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-d3d11on12createdevice )

**[ФАКТ]** Эталон реального набора экспортов виден по DEF-файлам DXVK (они повторяют Windows-набор, который требуют игры):
- `d3d11.dll`: `D3D11CoreCreateDevice`, `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`, `D3D11On12CreateDevice`.
- `dxgi.dll`: `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2`, `DXGIDeclareAdapterRemovalSupport`, `DXGIGetDebugInterface1`.
(DXVK: https://raw.githubusercontent.com/doitsujin/dxvk/master/src/d3d11/d3d11.def ; https://raw.githubusercontent.com/doitsujin/dxvk/master/src/dxgi/dxgi.def )

**[ФАКТ]** Что Unity делает с фабрикой: пробует `CreateDXGIFactory2`, при отсутствии — fallback на `CreateDXGIFactory1` (видно в логах вида `CreateDXGIFactory2(...) not found, fallbacking to CreateDXGIFactory1(...)`).
(Unity staff thread: https://discussions.unity.com/t/d3d11-queryinterface-idxgifactory5-failed-80004002-windows-7/745636 )

### 1.2. Статический импорт vs динамический (GetProcAddress)

**[ФАКТ]** UnityPlayer.dll импортирует `GetProcAddress` из kernel32 и резолвит много рантайм-функций динамически (именно на это вешаются UnityDoorstop/BepInEx). (https://binary-machinery.github.io/2020/05/21/mods-1.html )

**[ВЫВОД]** D3D11/DXGI точки входа Unity грузит **динамически** (`LoadLibrary` + `GetProcAddress`), чтобы пережить отсутствие API и сделать fallback на другой бэкенд. Это и есть механизм твоего креша: proxy/неполный DLL удовлетворяет `LoadLibrary` (файл есть), но `GetProcAddress` для конкретного символа возвращает NULL, а слот в Unity вызывается без null-guard.

**[ФАКТ]** Реальный прецедент: drop-in `d3d11.dll`-wrapper, **не** экспортивший `D3D11On12CreateDevice`, блокировал запуск, и мейнтейнер 3Dmigoto прямо пишет, что «свежая Unity 2023 имеет ту же проблему» (Planet Crafter). Ошибка лоадера: `The procedure entry point D3D11On12CreateDevice could not be located in ... d3d11.dll`.
(https://github.com/bo3b/3Dmigoto/issues/183 )

**[?]** Точный список статического IAT UnityPlayer 6000.x по d3d11/dxgi (что именно статически, а что через GetProcAddress) первоисточником не подтверждён. Прямое использование `D3DKMT*`-thunk'ов UnityPlayer'ом тоже не подтверждено — **[?]**.

---

## 2. Путь Unity «creating device client» — что он делает и где NULL крэшит «ровно там»

### 2.1. Лог-последовательность вокруг создания девайса

**[ФАКТ]** Каноническая последовательность в Unity 6000.x (подтверждённый player.log, 6000.0.13f1):
```
Initialize engine version: 6000.0.13f1 (...)
[Subsystems] Discovering subsystems at path .../UnitySubsystems
GfxDevice: creating device client; kGfxThreadingModeSplitJobs
Direct3D:
    Version:  Direct3D 12 [level 12.1]
    Renderer: ...
    Vendor:   ...
    VRAM:     ...
Begin MonoManager ReloadAssembly
```
(https://discussions.unity.com/t/release-build-immediate-crash-in-unity-6000-0-13f1/1499899 )

**[ФАКТ]** Формулировка строки в Unity 6 изменилась: старые версии (2021.3) пишут `GfxDevice: creating device client; threaded=1; jobified=1`, Unity 6000.x — именованный enum: `kGfxThreadingModeThreaded` / `kGfxThreadingModeSplitJobs`.
(старая форма: https://discussions.unity.com/t/application-doesnt-start-on-some-pcs-stuck-after-gfxdevice-creating-device-client/902537 )

**[ВЫВОД]** Точка твоего креша — **между** `GfxDevice: creating device client; ...` и блоком `Direct3D: Version ...`. Именно тут Unity вызывает в d3d11/dxgi (создание фабрики → создание девайса → QI интерфейсов). NULL, разыменованный здесь, падает после `creating device client`, но до `Direct3D: Version` — ровно как ты описываешь. В подтверждающем 2021.3-треде на сломанном ПК выполнение виснет сразу после `creating device client` и до `Direct3D: Version` не доходит.

### 2.2. Что ждёт выставленным (функ-поинтеры/интерфейсы)

**[ВЫВОД]** Терминология «device client» отражает client/worker-split GfxDevice: при threaded/jobified Unity строит `GfxDeviceClient`, проксирующий команды на render-thread/worker'ы; реальный D3D11/D3D12-девайс создаётся на worker-стороне. Наиболее вероятный источник NULL здесь — **таблица динамически зарезолвленных DXGI/D3D11 точек входа** (результаты `GetProcAddress`), а не внутренний job-dispatch (тот статически слинкован внутри UnityPlayer и из-за отсутствующего системного экспорта NULL'ом не станет).

**[ФАКТ]** Unity делает `QueryInterface` за новыми интерфейсами и **толерантна к отказу** (логирует `E_NOINTERFACE`/`0x80004002`, не падает). Канон: `d3d11: QueryInterface(IDXGIFactory5) failed (80004002)` — staff подтверждает, что это «безвредная» проверка `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`.
(https://discussions.unity.com/t/d3d11-queryinterface-idxgifactory5-failed-80004002-windows-7/745636 )

**[ВЫВОД]** Опасность не в «честном» `E_NOINTERFACE`, а в слое, который **заявляет поддержку интерфейса/фичи, но возвращает NULL/мусор**, который Unity доверчиво разыменовывает (см. раздел 3 — это доминирующий паттырн D3DMetal).

### 2.3. Требование, которое при NULL крэшит «ровно там» — `ID3D11Fence`

**[ФАКТ]** В Unity 6 на этапе инициализации девайса вызывается `GpuFence::Create()`, который требует `ID3D11Fence` (через `ID3D11Device5::CreateFence`, интерфейс из d3d11_4.h / D3D11.3+). Есть **официальный баг Unity**: `min-spec ... Failed to create GpuFence` (DX11-регрессия в Unity 6.1), с ошибкой `GpuFence::Create(): Failed to create ID3D11Fence, error 0x80070057` (E_INVALIDARG).
(Unity Issue Tracker: https://issuetracker.unity3d.com/issues/min-spec-directx-11-regression-in-unity-6-dot-1-failed-to-create-gpufence )

**[ФАКТ]** Воспроизведение на Wine→Metal (Whisky + CrossOver) с дословным логом — даже при принудительном DX11 Unity 6 сперва пробует D3D12, затем падает на fence:
```
[D3D12 Device Filter] Feature Level: 12.2
Direct3D 11.0 [level 11.1] Renderer: AMD Compatibility Mode
GpuFence::Create(): Failed to create ID3D11Fence
Dedicated video D3D11 device multithread protection failed
```
По сообщению репортёра, не лечится `-force-d3d11` / `-force-d3d11-no-singlethreaded`; требование fence в Unity 6 обязательное.
(https://itch.io/post/15344879 )

**[ВЫВОД]** `ID3D11Fence` появился именно в линейке Unity 6 → это лучший Unity-6-специфичный кандидат на «креш ровно на creating device client» на translation-layer'е, который не реализует fence. Нюанс: «честный» путь возвращает HRESULT-ошибку и лог `Failed to create ID3D11Fence` (graceful), тогда как у тебя — NULL-вызов. Значит либо (а) у тебя не graceful-ветка, а более ранний NULL-резолв, либо (б) после неудачного fence Unity дальше разыменовывает NULL. **[?]** какая именно подветка у 6000.0.61f1.

---

## 3. Известные грабли Unity-D3D11 на translation-layer'ах + причины/фиксы

### 3.1. DXVK / vkd3d-proton

**[ФАКТ]** `D3D11On12CreateDevice` добавлен в DXVK только в **v2.2 (12.05.2023)** «чтобы включить D3D12-поддержку в свежих Unity-играх (Lego Builder's Journey)». До 2.2 экспорта не было; реализация требует vkd3d-proton ≥ коммита `26c4fed`, иначе не работает.
(https://github.com/doitsujin/dxvk/releases/tag/v2.2 )

**[ФАКТ]** Multi-GPU/PRIME-регрессия (Proton 6.3-3): все D3D11/D3D12-игры не создавали девайс, лог:
```
warn:  D3D11CoreCreateDevice: Adapter is not a DXVK adapter
info:  D3D11CoreCreateDevice: Probing 0
err:   D3D11CoreCreateDevice: Requested feature level not supported
```
Причина — Proton использовал Wine-DXGI вместо DXVK-DXGI; DXVK не узнавал адаптер. Фикс: `WINEDLLOVERRIDES=dxgi=n %command%`, либо Proton ≥ 6.3-5.
(https://github.com/ValveSoftware/Proton/issues/4835 )

**[ФАКТ]** Unity «Enemies» demo падала в DXVK внутри `CreateVertexShader` — демо «использует D3D12 в основном, но зачем-то прокидывает D3D12-шейдеры в D3D11» (D3D11↔D3D12 interop в HDRP). Это не дефект device-creation, а interop-edge.
(https://github.com/doitsujin/dxvk/issues/3086 )

**[ФАКТ]** Конфиг-рычаги DXVK под Unity: `d3d11.maxFeatureLevel` (по умолчанию `12_1`; «повышение может позволить создать девайс там, где иначе fail»), `dxgi.maxFrameLatency`, `DXVK_FILTER_DEVICE_NAME/UUID` (при неверной настройке отфильтрует все девайсы → device-creation невозможен).
(https://raw.githubusercontent.com/doitsujin/dxvk/master/dxvk.conf ; README: https://github.com/doitsujin/dxvk )

**[ФАКТ]** Debug-layer: DXVK **не** требует `D3D11SDKLayers.dll`. Флаг `D3D11_CREATE_DEVICE_DEBUG` пробрасывается насквозь, проверки на наличие debug-слоя нет → DXVK **не** падает с `DXGI_ERROR_SDK_COMPONENT_MISSING` (в отличие от настоящей Windows). То есть DEBUG-флаг сам по себе device-creation на DXVK не валит.
(https://raw.githubusercontent.com/doitsujin/dxvk/master/src/d3d11/d3d11_main.cpp ; MS: https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-layers )

### 3.2. wined3d

**[ФАКТ]** `D3D11On12CreateDevice` в индексе Wine API помечен «not documented» (исторически стаб/неполный) — главный высокоимпактный пробел для современных Unity (2023+/DX12-via-11on12).
(https://source.winehq.org/WineAPI/d3d11.html , корроборация: https://github.com/bo3b/3Dmigoto/issues/183 )

**[ФАКТ]** Новые device-интерфейсы (`ID3D11Device1..5`) добавлялись в Wine инкрементально через стабы; fence-поддержка (`ID3D11Fence` через `ID3D11Device5`) исторически отставала.
(история патчей: https://winehq.org/mailman3/hyperkitty/list/wine-devel@winehq.org/thread/QQLDV33U3QVWOEY4OEXFSKC4PXV3JIOV/ )

### 3.3. Apple GPTK / D3DMetal

**[ФАКТ]** Доминирующий паттырн D3DMetal — **«заявил поддержку → вернул NULL/невалид → игра разыменовала»**: access violation `0xC0000005` записью по адресу `0x0`, `RAX=0x0`. Apple подтвердила открытый фидбэк FB22285513.
(https://developer.apple.com/forums/thread/819513 )

**[ФАКТ]** Apple-инженер: строки `D3DMetal ... unsupported interface (...)` (напр. QI `ID3D10Multithread`) — это нормальный capability-probing через QueryInterface, **обычно не причина креша**. Не ведись на них.
(https://developer.apple.com/forums/thread/731843 )

**[ФАКТ]** Жёстко неподдержанное, что реально валит: `Unsupported API: IDXGISwapChain3::SetColorSpace1`, `Unsupported API: D3D11 timestamp query` (timestamp-queries Apple подтвердила как неподдержанные).
(https://developer.apple.com/forums/thread/757107 )

**[ФАКТ]** Шейдер-кэш D3DMetal может портиться → «не грузится»; фикс — удалить `$(getconf DARWIN_USER_CACHE_DIR)/d3dm/shaders.cache`. Логи D3DMetal в Console с префиксом `D3DM`.
(https://www.applegamingwiki.com/wiki/Game_Porting_Toolkit )

### 3.4. CrossOver / Whisky

**[ФАКТ]** В CrossOver 26 бэкенд выбирается per-bottle: **Auto / DXMT / D3DMetal / DXVK / Wine (wined3d)**. «Некоторые игры лучше на DXVK, другие на wined3d».
(https://support.codeweavers.com/en_US/advanced-settings-in-crossover-mac-26 )

**[ВЫВОД]** На macOS переключатель бэкенда — более мощный рычаг, чем Unity `-force-*` флаги. Для Unity-D3D11-тайтла, падающего на D3DMetal, **DXMT — самый перспективный альтернативный бэкенд** (он целевой для D3D11 и имеет Unity-aware код-пути).

### 3.5. DXMT (твой слой)

**[ФАКТ]** DXMT экспортит всё нужное Unity: `d3d11.dll` → `D3D11CoreCreateDevice`, `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`, `D3D11On12CreateDevice`; `dxgi.dll` → `CreateDXGIFactory`/`1`/`2`, `DXGIGetDebugInterface1`. То есть **отсутствующий экспорт — не твоя причина**.
(https://github.com/3Shain/dxmt/blob/main/src/d3d11/d3d11.def ; https://github.com/3Shain/dxmt/blob/main/src/dxgi/dxgi.def )

**[ФАКТ]** Нюансы значений:
- `D3D11On12CreateDevice` — экспортирован, но **stub `E_NOTIMPL`** (не NULL). Чистый D3D11-путь его не использует.
- `D3D11CreateDeviceAndSwapChain` — реально реализован (workhorse; `D3D11CreateDevice` форвардит в него).
- `D3D11CoreCreateDevice` пробует FL `11_1` (только на Apple7+/M1+), иначе `11_0`...`9_1`; при превышении min FL — `E_INVALIDARG`, при исключении — `E_FAIL`. NULL-девайс с S_OK не отдаёт; out-поинтеры инициализируются через `InitReturnPtr`.
(https://github.com/3Shain/dxmt/blob/main/src/d3d11/d3d11.cpp )

**[ФАКТ]** DXMT — это Wine **PE+Unix split builtin** (`d3d11.dll`/`dxgi.dll` → `winemetal.dll` → `__wine_unix_call` → `winemetal.so` → Metal), пост-обработанный `winebuild --builtin` и привязанный к конкретному Wine-дереву. Официальный install кладёт файлы **только** в `x86_64-windows` / `x86_64-unix`; **aarch64-пути в гайде нет**.
(https://github.com/3Shain/dxmt/wiki/DXMT-Installation-Guide-for-Geeks ; https://github.com/3Shain/dxmt/tree/main/src/winemetal ; DeepWiki: https://deepwiki.com/3Shain/dxmt )

**[ФАКТ]** DXMT уже чинил Unity-специфику (доказывает, что Unity device-creation на DXMT в норме работает): v0.74 — фикс зависания Unity при exclusive-fullscreen→borderless; v0.70 — видео в катсценах нескольких Unity-игр, зависания на выходе; фиксы Palworld/OMORI/Umamusume/Stellaris. Баги принимаются на **dxmt.report**/Discord, не в GitHub Issues.
(https://github.com/3Shain/dxmt/releases )

**[?]** Реализует ли текущий DXMT `ID3D11Device5::CreateFence`/`ID3D11Fence` — первоисточником не подтверждено (в release-notes явного упоминания fence нет). Это ключевой пункт для проверки под гипотезу №1.

---

## 4. Специфика Unity-D3D11, которую слой может не дать

**[ФАКТ] Feature level.** Unity пробует FL `11_1`, делает fallback на `11_0` и даже `10.1`; строка `D3D_FEATURE_LEVEL_11_1 not-recognized` — non-fatal. Эффективный минимум для рендера Unity 6 — `11_0`. Слой, отдающий валидный `11_0` с graceful-деградацией, проходит device-creation.
(https://discussions.unity.com/t/unity-using-direct3d-11-1-now-instead-of-11-0-windows-7-crash/695055 )

**[ФАКТ] Версия ID3D11Device.** Unity берёт базовый `ID3D11Device`/`Context` и **best-effort** QI'ит старшие версии (`ID3D11Device1+`, `ID3DUserDefinedAnnotation` через `Context1`, `IDXGIFactory2/5`). Отказ логируется `0x80004002`, не крэшит. Жёсткого требования `ID3D11Device3/5` на этапе init нет — **но** конкретный метод девайса-5 может быть нужен (см. fence). Прецедент failure-mode: Halo Infinite требовал `ID3D11Device5::CreateFence` до его реализации в DXVK.
(https://discussions.unity.com/t/failed-to-query-d3d11-context-for-id3duserdefinedannotation-interface-hr-0x80004002/638336 ; https://github.com/doitsujin/dxvk/issues/2409 )

**[ФАКТ] D3D11On12 / DX12-interop.** DX12-бэкенд Unity существует и в Unity 6.1+ становится дефолтом для новых Windows-проектов; D3D11 остаётся fallback/legacy. Свежие Unity-тайтлы могут гнать D3D11 поверх D3D12 → требуется `D3D11On12CreateDevice` + `ID3D11On12Device` + shared resources.
(https://unity.com/blog/directx-12-improvements-in-unity-6 ; https://docs.unity3d.com/6000.3/Documentation/Manual/WhatsNewUnity61.html ; https://github.com/doitsujin/dxvk/releases/tag/v2.2 )

**[ФАКТ] Device flags.** `-force-d3d11-singlethreaded` / `-force-d3d11-no-singlethreaded` тогглят `D3D11_CREATE_DEVICE_SINGLETHREADED` (под SINGLETHREADED `CreateDeferredContext` отдаёт `DXGI_ERROR_INVALID_CALL`). Дефолт флага зависит от threading-mode/версии. Debug-флаг (`D3D11_CREATE_DEVICE_DEBUG`) в обычных сборках Unity не ставит.
(https://discussions.unity.com/t/disable-d3d11_create_device_singlethreaded-without-command-line-arguments/764254 ; MS: https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createdeferredcontext )
**[?]** `BGRA_SUPPORT` (0x20) Unity почти наверняка ставит (для D2D-interop), но отдельным Unity-первоисточником не подтверждено.

**[ФАКТ] DXGI / swapchain.** Unity по умолчанию использует flip-model для D3D11 (`PlayerSettings.useFlipModelSwapchain` = true), всегда flip для D3D12, fallback на BitBlt на pre-Win8.1 / в ExclusiveFullScreen. Tearing пробуется через `IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)`; отсутствие `IDXGIFactory5` безвредно. Минимум — `IDXGIFactory1`, предпочтительно `IDXGIFactory2` (flip-model `CreateSwapChainForHwnd`).
(https://docs.unity3d.com/ScriptReference/PlayerSettings-useFlipModelSwapchain.html )

**[ФАКТ] `force-d3d11`-семейство (официальный CLI Unity 6000.4).** `-force-d3d11`, `-force-d3d11-bitblt-model`, `-force-d3d11-flip-model`, `-force-d3d11-singlethreaded`, `-force-d3d11-no-singlethreaded`, `-force-d3d12`, `-force-vulkan`, `-force-glcore`, `-force-driver-type-warp`, `-force-feature-level-11-0`, `-force-device-index`, `-force-gfx-direct` («Force single threaded rendering»). Строка `force-d3` в бинаре — это именно эта таблица аргументов, лежащая в `.rdata` рядом с кодом graphics-init.
(https://docs.unity3d.com/Manual/PlayerCommandLineArguments.html )
**[?]** `-force-gfx-st`, `-force-d3d11-debug-device` — community-known, в официальном CLI 6000.x отсутствуют.

**[ВЫВОД] Где слои реально ломают Unity (по убыванию риска для device-creation):** (1) D3D11On12 / DX12-interop для DX12-дефолтных тайтлов; (2) `ID3D11Fence`/device-5 метод, нужный Unity 6; (3) корректный flip-model present/resize; (4) компиляция DXBC-шейдеров (это уже post-init). Feature-level и версия интерфейса — НЕ типичная причина креша из-за QI-and-fallback дизайна Unity.

---

## 5. Применимость к твоему кейсу + что это значит

Твои наблюдения (DXMT работает, NULL внутри Unity-логики рядом с `force-d3`, опровергнуты vendor/API-selection/threading) сужают до:

- **[ВЫВОД]** Гипотеза №1 (`ID3D11Fence`) — лучший Unity-6-специфичный лид. 6000.0.61f1 входит в линейку, где `GpuFence::Create` обязателен. Если DXMT не реализует `ID3D11Device5::CreateFence`, Unity на этом и спотыкается. Расхождение: «честный» путь — это HRESULT+лог, а не NULL-вызов; значит у тебя либо ранний NULL-резолв символа, либо post-fence NULL-deref. **Проверь Player.log на `GpuFence` / `ID3D11Fence` / `[D3D12 Device Filter]`.**
- **[ВЫВОД, высокая уверенность но против твоего наблюдения]** Гипотеза №2 (ARM64X↔x64 EC-thunk mismatch): официально DXMT — это x86_64-windows/x86_64-unix builtin под x64-Wine+Rosetta; нативный ARM64X — вне опубликованных конфигураций, и корректность x64-callable EC-thunk'ов в нём первоисточником не подтверждается (**[?]**). Креш на первом cross-domain вызове ровно после `creating device client` укладывается в loader-mismatch. Ты говоришь «DXMT работает» — но «работает» мог быть проверен не на том самом первом вызове, который делает x64 UnityPlayer.
- **[ВЫВОД]** Гипотеза №3 (D3D11On12): если 6000.0.61f1-сборка Hollow Knight гонит D3D11-поверх-D3D12, она дойдёт до `D3D11On12CreateDevice`, который у DXMT — `E_NOTIMPL` stub. Это вернёт ошибку (не NULL-вызов напрямую), но Unity может разыменовать NULL-девайс дальше. **[?]** использует ли именно эта сборка D3D11On12.

**Замечание про Hollow Knight:** оригинальный Hollow Knight — legacy-Unity (не Unity 6) и имеет нативную Apple-Silicon-сборку, т.е. обычно «просто работает» и НЕ воспроизводит Unity-6-грабли. Раз у тебя build именно `6000.0.61f1`, это либо Silksong/ремастер-сборка, либо ремап версии — и тогда Unity-6-специфика (fence!) применима в полной мере. Стоит свериться, что версия движка в `*_Data/boot.config` / `Initialize engine version` действительно 6000.0.61f1.

---

## 6. Диагностические шаги (внешние, без правки кода)

1. **[ВЫВОД]** Открой `Player.log` и посмотри строки вокруг `GfxDevice: creating device client`. Маркеры:
   - `[D3D12 Device Filter] Feature Level: ...` + `Failed to create ID3D11Fence` → гипотеза №1 (fence).
   - ошибка лоадера про missing export (`D3D11On12CreateDevice` и т.п.) → гипотеза №4.
   - голый `0xC0000005` запись по `0x0` без Unity-ошибки → loader/arch-mismatch (№2).
2. **[ФАКТ]** Проверь, пишет ли DXMT свои `d3d11.log`/`dxgi.log`. Если они **пустые/отсутствуют** — PE-код DXMT не исполнялся → креш в loader/thunk-слое ДО DXMT (сильный довод за №2). (Logger в `src/d3d11/d3d11.cpp`.)
3. **[ВЫВОД]** Сверь архитектуру реально загруженного образа DXMT и UnityPlayer. Если UnityPlayer x64, а исполняемая половина DXMT — ARM64(EC) без валидных x64 EC-thunk'ов — это №2. Эталонная рабочая конфигурация DXMT — x86_64-артефакты под x86_64-Wine/CrossOver + Rosetta.
4. **[ФАКТ]** Проверь раскладку файлов и оверрайды: `winemetal.so` в `.../x86_64-unix/`, `winemetal.dll`/`d3d11.dll`/`dxgi.dll` в `.../x86_64-windows/` (или system32), `WINEDLLOVERRIDES="dxgi,d3d11,d3d10core=n,b"`, и что Wine экспонирует символы `winemac.drv`.
5. **[ВЫВОД]** Кросс-чек на другом бэкенде: запусти ту же сборку под **DXVK** (D3D11→Vulkan→MoltenVK) в CrossOver. Если на DXVK тоже падает на fence → причина Unity-side (№1/№3), а не DXMT-specific.
6. **[ФАКТ]** Спроси на dxmt.report / Discord про `ID3D11Device5::CreateFence`/`ID3D11Fence` под x64-Unity-6 — это не GitHub-Issues-проект.

---

## Источники

**Unity (docs / staff / issue tracker):**
- Player command line args (6000.4): https://docs.unity3d.com/Manual/PlayerCommandLineArguments.html
- «stuck after GfxDevice: creating device client» (лог-последовательность): https://discussions.unity.com/t/application-doesnt-start-on-some-pcs-stuck-after-gfxdevice-creating-device-client/902537
- «Release build immediate crash 6000.0.13f1» (6000.x лог + креш): https://discussions.unity.com/t/release-build-immediate-crash-in-unity-6000-0-13f1/1499899
- IDXGIFactory5 QI failed — harmless tearing-probe (staff): https://discussions.unity.com/t/d3d11-queryinterface-idxgifactory5-failed-80004002-windows-7/745636
- FL 11_1→11_0→10.1 fallback (staff): https://discussions.unity.com/t/unity-using-direct3d-11-1-now-instead-of-11-0-windows-7-crash/695055
- ID3DUserDefinedAnnotation QI 0x80004002: https://discussions.unity.com/t/failed-to-query-d3d11-context-for-id3duserdefinedannotation-interface-hr-0x80004002/638336
- D3D11_CREATE_DEVICE_SINGLETHREADED / deferred contexts: https://discussions.unity.com/t/disable-d3d11_create_device_singlethreaded-without-command-line-arguments/764254
- useFlipModelSwapchain: https://docs.unity3d.com/ScriptReference/PlayerSettings-useFlipModelSwapchain.html
- DX12 improvements in Unity 6: https://unity.com/blog/directx-12-improvements-in-unity-6
- What's new Unity 6.1 (DX12 default): https://docs.unity3d.com/6000.3/Documentation/Manual/WhatsNewUnity61.html
- **Issue Tracker — Failed to create GpuFence (DX11-регрессия Unity 6.1):** https://issuetracker.unity3d.com/issues/min-spec-directx-11-regression-in-unity-6-dot-1-failed-to-create-gpufence
- System requirements 6000.0: https://docs.unity3d.com/6000.0/Documentation/Manual/system-requirements.html
- DX11 features: https://docs.unity3d.com/Manual/UsingDX11GL3Features.html

**Wine→Metal / GpuFence воспроизведение:**
- itch.io (Unity 6 `Failed to create ID3D11Fence` на Whisky+CrossOver, дословный лог): https://itch.io/post/15344879

**DXVK:**
- v2.2 release (D3D11On12 для Unity): https://github.com/doitsujin/dxvk/releases/tag/v2.2
- d3d11.def: https://raw.githubusercontent.com/doitsujin/dxvk/master/src/d3d11/d3d11.def
- dxgi.def: https://raw.githubusercontent.com/doitsujin/dxvk/master/src/dxgi/dxgi.def
- d3d11_main.cpp (device-creation, FL-probe, DEBUG-flag): https://raw.githubusercontent.com/doitsujin/dxvk/master/src/d3d11/d3d11_main.cpp
- dxvk.conf (maxFeatureLevel и пр.): https://raw.githubusercontent.com/doitsujin/dxvk/master/dxvk.conf
- README: https://github.com/doitsujin/dxvk
- issue #3086 (Unity Enemies, D3D11↔D3D12): https://github.com/doitsujin/dxvk/issues/3086
- issue #2409 (ID3D11Device5::CreateFence, Halo Infinite — failure-mode): https://github.com/doitsujin/dxvk/issues/2409
- issue #1095 (Failed to create DXGI factory): https://github.com/doitsujin/dxvk/issues/1095

**Proton:**
- #4835 (multi-GPU/PRIME device-cap regression, `dxgi=n` фикс): https://github.com/ValveSoftware/Proton/issues/4835

**wined3d / Wine:**
- d3d11.dll API index (D3D11On12CreateDevice «not documented»): https://source.winehq.org/WineAPI/d3d11.html
- история d3d11 interface-стабов: https://winehq.org/mailman3/hyperkitty/list/wine-devel@winehq.org/thread/QQLDV33U3QVWOEY4OEXFSKC4PXV3JIOV/

**Apple GPTK / D3DMetal:**
- NULL-deref at pipeline creation (0xC0000005, FB22285513): https://developer.apple.com/forums/thread/819513
- «unsupported interface» обычно безвредны (Apple eng): https://developer.apple.com/forums/thread/731843
- timestamp query / SetColorSpace1 неподдержаны: https://developer.apple.com/forums/thread/757107
- AppleGamingWiki GPTK (D3DM-логи, shader-cache fix): https://www.applegamingwiki.com/wiki/Game_Porting_Toolkit

**CrossOver:**
- Advanced settings (бэкенды Auto/DXMT/D3DMetal/DXVK/Wine): https://support.codeweavers.com/en_US/advanced-settings-in-crossover-mac-26

**DXMT:**
- d3d11.def: https://github.com/3Shain/dxmt/blob/main/src/d3d11/d3d11.def
- dxgi.def: https://github.com/3Shain/dxmt/blob/main/src/dxgi/dxgi.def
- d3d11.cpp (device-create, FL-probe, D3D11On12=E_NOTIMPL): https://github.com/3Shain/dxmt/blob/main/src/d3d11/d3d11.cpp
- winemetal split: https://github.com/3Shain/dxmt/tree/main/src/winemetal
- Install guide (x86_64-only раскладка): https://github.com/3Shain/dxmt/wiki/DXMT-Installation-Guide-for-Geeks
- Runtime spec: https://github.com/3Shain/dxmt/wiki/Device-System-Runtime-Specifications
- Releases (Unity-фиксы, WoW64): https://github.com/3Shain/dxmt/releases
- DeepWiki (архитектура): https://deepwiki.com/3Shain/dxmt
- CrossOver DXMT blog: https://www.codeweavers.com/blog/mjohnson/2025/3/11/experience-next-level-gaming-on-mac-with-crossover-25

**Microsoft:**
- D3D11CreateDevice (PFN/GetProcAddress): https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-d3d11createdevice
- D3D11On12CreateDevice: https://learn.microsoft.com/en-us/windows/win32/api/d3d11on12/nf-d3d11on12-d3d11on12createdevice
- ID3D11Device5::CreateFence: https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-createfence
- D3D11 devices/layers (DEBUG → D3D11SDKLayers.dll): https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-devices-layers
- CreateDeferredContext (SINGLETHREADED): https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createdeferredcontext

**Прочее:**
- 3Dmigoto #183 (missing D3D11On12CreateDevice → Unity 2023 launch fail): https://github.com/bo3b/3Dmigoto/issues/183
- UnityPlayer GetProcAddress/Doorstop: https://binary-machinery.github.io/2020/05/21/mods-1.html
