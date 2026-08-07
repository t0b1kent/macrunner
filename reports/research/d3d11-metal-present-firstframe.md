# D3D11-презентация → Metal на macOS под Wine: путь до пикселя и почему нет первого кадра

**Дата:** 2026-06-15. **Метки достоверности:** `[ФАКТ]` — подтверждено первоисточником с цитатой; `[ВЫВОД]` — логическое следствие из фактов; `[?]` — не подтверждено первоисточником / неуверенность.

Все цитаты кода — из публичных репозиториев (DXMT `3Shain/dxmt`, DXVK `doitsujin/dxvk`, MoltenVK `KhronosGroup/MoltenVK`, зеркало Wine `wine-mirror/wine`) и developer.apple.com на дату отчёта.

---

## 1. Презентационный путь D3D11 → Metal на macOS

### 1.1. DXMT (D3D11 → Metal напрямую)

`[ФАКТ]` Swapchain — это класс `MTLD3D11SwapChain` (реализует `IDXGISwapChain4`). Он **не рендерит игру прямо в drawable**: создаётся приватный D3D11-backbuffer (`backbuffer_`, `D3D11_BIND_RENDER_TARGET`), который `GetBuffer(0)` отдаёт игре; при презентации backbuffer блитится в drawable слоя полноэкранным треугольником (с gamma-LUT/HDR-метаданными, опционально MetalFX-апскейл). Источник: `src/d3d11/d3d11_swapchain.cpp`.

`[ФАКТ]` Презентационная поверхность — это **`CAMetalLayer`**, полученный от Wine (см. §2). Unix-мост кастует хэндл прямо в `CAMetalLayer` и вызывает на нём `nextDrawable`:
```c
// src/winemetal/unix/winemetal_unix.c
params->ret = (obj_handle_t)[(CAMetalLayer *)params->handle nextDrawable];
```
Свойства слоя (`device`, `framebufferOnly`, `contentsScale`, `displaySyncEnabled`, `drawableSize`, `pixelFormat`) выставляются на главном потоке в `_MetalLayer_setProps`. Источник: `winemetal_unix.c`.

`[ФАКТ]` Порядок вызовов. `Present` → `Present1`: флашит контекст, считает `vsync_duration`, **кладёт present-колбэк в командный чанк**, коммитит чанк и вызывает `PresentBoundary()`. Реальный `nextDrawable` и блит происходят **асинхронно, на отдельном encode-потоке** (`#define ASYNC_ENCODING 1`, потоки `dxmt-encode-thread`/`dxmt-finish-thread`), а не в вызове `Present` приложения:
```cpp
// src/dxmt/dxmt_presenter.cpp  (Presenter::encodeCommands)
auto drawable = layer_.nextDrawable();
info.colors[0].texture = drawable.texture();
... encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);  // блит backbuffer→drawable
return drawable;
```
```cpp
// src/dxmt/dxmt_context.cpp  (EncoderType::Present)
if (data->after > 0) cmdbuf.presentDrawableAfterMinimumDuration(drawable, data->after);
else                 cmdbuf.presentDrawable(drawable);
```
`commit()` командного буфера делается в очереди (`CommitChunkInternal`), не в `Present`. Источники: `dxmt_presenter.cpp`, `dxmt_context.cpp`, `dxmt_command_queue.cpp`.

`[ФАКТ]` Фрейм-пейсинг отдан Metal/CoreAnimation, а не симулируется на CPU (через `presentDrawableAfterMinimumDuration`). По умолчанию `display_sync_enabled = false`; глубина латентности `kSwapchainLatency = 1`. Источники: `dxmt.conf`, `dxmt_presenter.cpp`, `d3d11_swapchain.cpp`.

`[ФАКТ]` Если окно свёрнуто или размер 0 (и swap-effect ≤ SEQUENTIAL) — `Present1` возвращает `DXGI_STATUS_OCCLUDED` и **флашит команды без презентации**. Источник: `d3d11_swapchain.cpp`.

### 1.2. DXVK → MoltenVK (Vulkan `VK_KHR_swapchain` → CAMetalLayer)

`[ФАКТ]` DXVK реализует `Present` в классе `dxvk::Presenter` (`src/dxvk/dxvk_presenter.cpp`; старого `src/vulkan/vulkan_presenter.cpp` больше нет). Получение кадра — `vkAcquireNextImageKHR` (таймаут UINT64_MAX, семафор без fence), показ — `vkQueuePresentKHR` на графической очереди. После успешного present DXVK **сразу пред-захватывает следующий image**. Поверхность создаётся через платформенный колбэк `m_surfaceProc` (слой `src/wsi/`), сам presenter платформонезависим и видит только `VkSurfaceKHR`. Источник: `dxvk_presenter.cpp`.

`[ФАКТ]` MoltenVK: для вывода на экран **обязателен `VK_EXT_metal_surface`**, а поверхность **обязана быть на базе `CAMetalLayer`**, иначе `VK_ERROR_SURFACE_LOST_KHR`:
> «In order to visibly display your content … you must enable the `VK_EXT_metal_surface` extension» (User Guide).
```cpp
// MVKSurface.mm
if ( !_mtlCAMetalLayer && !isHeadless )
  reportError(VK_ERROR_SURFACE_LOST_KHR,
    "On-screen rendering requires a layer of type CAMetalLayer.");
```
`MVKSwapchain` настраивает слой: `drawableSize`, `device`, `pixelFormat`, `maximumDrawableCount`, `displaySyncEnabledMVK` (FIFO → vsync ON, IMMEDIATE → OFF). Источники: `MoltenVK_Runtime_UserGuide.md`, `MVKSurface.mm`, `MVKSwapchain.mm`.

`[ФАКТ]` Ключевой нюанс: `vkAcquireNextImageKHR` **не** вызывает `nextDrawable`. Реальный Metal-drawable берётся **лениво**, в момент рендера/презентации, в `MVKPresentableSwapchainImage::getCAMetalDrawable` (`MVKImage.mm`), с ретраями; nil/невалидный формат → `VK_ERROR_OUT_OF_DATE_KHR`/`VK_ERROR_OUT_OF_POOL_MEMORY` → пересоздание swapchain. Презентация: `vkQueuePresentKHR` → `presentCAMetalDrawable` → `[drawable present]`/`presentAtTime:` **внутри scheduled-handler командного буфера** (по рекомендации Apple), плюс `addPresentedHandler` для тайминга. Источник: `MVKImage.mm`.

**Итоговая цепочка:** игра рендерит в backbuffer → `Present` → (DXMT: encode-поток `nextDrawable`+блит+`presentDrawable`+`commit`; DXVK: `vkQueuePresentKHR` → MoltenVK `presentCAMetalDrawable` → `[drawable present]`) → CAMetalLayer отдаёт кадр WindowServer → композиция на экран.

---

## 2. Мост HWND → NSView → CAMetalLayer в winemac.drv

`[ФАКТ]` HWND ↔ Cocoa хранится в `struct macdrv_win_data` (ключ — HWND в `CFMutableDictionary`): поля `cocoa_window` (NSWindow) и `client_view` (NSView). Источник: `dlls/winemac.drv/macdrv.h`, `window.c`.

`[ФАКТ]` Классы Cocoa (все в `cocoa_window.m`):
- `WineWindow : NSPanel <NSWindowDelegate>` — окно, само себе делегат (`[window setDelegate:window]`).
- `WineContentView : WineBaseView` — content-view, **layer-backed** (`setWantsLayer:YES`), создаётся в `createWindowWithFeatures:` и ставится через `setContentView:`.
- `WineMetalView : WineBaseView` — отдельный subview под Metal; в `makeBackingLayer` создаёт `CAMetalLayer`:
```objc
- (CALayer*) makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = _device;
    layer.framebufferOnly = YES;
    layer.contentsScale = retina_on ? 2.0 : 1.0;
    return layer;
}
```
Metal-view добавляется как subview content-view позицией `NSWindowBelow`. Источник: `cocoa_window.m`.

`[ФАКТ]` C-мост (именно эти символы дёргает DXMT через `dlsym`): `macdrv_create_metal_device` (обёртка `MTLCreateSystemDefaultDevice()`), `macdrv_view_create_metal_view` (создаёт view **на главном потоке**, `OnMainThread`), `macdrv_view_get_metal_layer` (возвращает `(CAMetalLayer*)view.layer`). Источники: `macdrv_cocoa.h`, `cocoa_window.m`; со стороны DXMT — `winemetal_unix.c` (`macdrv_view_create_metal_view` → `macdrv_view_get_metal_layer`), `d3d11_swapchain.cpp` (`CreateMetalViewFromHWND`, при провале — `abort()`).

`[ФАКТ]` Vulkan-путь (для DXVK/MoltenVK): `dlls/winemac.drv/vulkan.c` отдаёт `CAMetalLayer` в `vkCreateMetalSurfaceEXT` (или fallback `vkCreateMacOSSurfaceMVK`). Тот же client-view-механизм; для дочерних кросс-процессных окон — `FIXME("Cross-process child window Metal swapchains are not implemented")`. Источник: `vulkan.c`, `window.c`.

`[ФАКТ]` **Гейт WindowServer/GUI-сессии.** `macdrv_init` отказывается грузиться без графического доступа сессии:
```c
// dlls/winemac.drv/macdrv_main.c
SessionGetInfo(callerSecuritySession, NULL, &attributes);
if (status != noErr || !(attributes & sessionHasGraphicAccess))
    return STATUS_UNSUCCESSFUL;
```
`[ФАКТ]` Cocoa-приложение поднимается `macdrv_start_cocoa_app` → `WineApplication : NSApplication`, `[NSApp run]`. Политика активации **стартует как фоновая/accessory и повышается до `NSApplicationActivationPolicyRegular` лениво** — при первом показе окна, в `transformProcessToForeground:` (вызывается из order-in пути окна). Явного `Prohibited` драйвер не ставит. Источники: `cocoa_main.m`, `cocoa_app.m`, `cocoa_window.m`.

`[ФАКТ]` Порядок/видимость окна: `macdrv_order_cocoa_window` → `-[WineWindow orderBelow:orAbove:activate:]` (промоут процесса в foreground → опц. активация → `orderFront:`). `makeKeyAndOrderFront:` Wine сам **не** вызывает. Off-screen — `doOrderOut` (`orderOut:`/`close`), бит `on_screen` в `macdrv_win_data`. Источник: `cocoa_window.m`, `window.c`.

`[?]` Литерального токена `WINDOW_SERVER` в исходниках winemac.drv **нет** — функциональный эквивалент это проверка `sessionHasGraphicAccess` выше.

---

## 3. ЧЕКЛИСТ: почему нет первого кадра + как диагностировать

> Для удобства разделено на два класса (на практике дают один симптом «чёрный экран»): **(A) окно/слой так и не вышло на экран** и **(B) окно есть, но кадр чёрный/не флашится**.

**A1. Процесс не имеет GUI-сессии (нет соединения с WindowServer).** `[ФАКТ]` Запуск по SSH без активного логина на экране, из LaunchDaemon, или иначе вне Aqua-сессии → `sessionHasGraphicAccess == NO` → `macdrv_init` возвращает `STATUS_UNSUCCESSFUL`, окна не создаются (Wine `macdrv_main.c`; Apple TN2083/DTS: «SSH session is not an Aqua session type… `SessionGetInfo()` reports `NO` for `sessionHasGraphicAccess`»). **Диагностика:** проверить, есть ли логин в Aqua; искать в логах `_CGSDefaultConnection() is NULL`, `kCGErrorRangeCheck`, `FAILED to establish the default connection to the WindowServer`. **Фикс:** запускать из GUI-сессии / LaunchAgent в домене `gui/<UID>`, не из daemon/SSH.

**A2. Политика активации не GUI.** `[ФАКТ]` `.prohibited` — «may not create windows»; `.accessory` — окна можно, но без Dock/меню; `.regular` — обычное GUI-приложение (Apple, `NSApplication.ActivationPolicy`). `[ВЫВОД]` Если процесс остался в `prohibited`/не дошёл до `transformProcessToForeground:`, окно не покажется даже при корректном слое. **Диагностика:** залогировать `[NSApp activationPolicy]` и факт вызова `transformProcessToForeground:`. **Фикс:** убедиться, что путь order-in окна выполняется (см. A3).

**A3. Окно не выведено на экран / off-screen / не orderFront.** `[ФАКТ]` winemac.drv показывает окно только через `orderBelow:orAbove:activate:`; свёрнутое/0-размерное окно → `DXGI_STATUS_OCCLUDED` без презентации (DXMT). **Диагностика:** проверить бит `on_screen`, размер окна, `WINEDEBUG=+macdrv`. **Фикс:** дождаться реального показа окна до первого `Present`; не презентовать в occluded-состоянии.

**A4. Неактивированное приложение (Sonoma cooperative activation).** `[ФАКТ]` Apple DTS: возможна «incomplete activation» — Dock-иконка есть, окно не разворачивается; на Sonoma+ активация стала кооперативной, такие сбои чаще. **Диагностика:** видно иконку, но окна нет. **Фикс:** корректный foreground-промоут/активация.

**B1. `nextDrawable` вернул nil.** `[ФАКТ]` Apple: nil если (а) все drawables заняты — слой ждёт ~1 c и возвращает nil, либо (б) «pixelFormat or other properties are invalid». `[ВЫВОД]` сюда же попадают `device == nil`, `drawableSize == 0`, неподдерживаемый `pixelFormat`. MoltenVK на nil выдаёт `VK_ERROR_OUT_OF_DATE_KHR`/`OUT_OF_POOL_MEMORY` и крутит пересоздание swapchain (вечный чёрный). **Диагностика:** счётчик nil-drawable; в DXMT — HUD `drawable_blocking_interval`; в MoltenVK — лог пересоздания. **Фикс:** валидные `device`/`pixelFormat`/ненулевой `drawableSize`; не удерживать drawables; рендер-цикл в `@autoreleasepool`.

**B2. `drawableSize`/`contentsScale` == 0.** `[ФАКТ]` Apple: `drawableSize` = `bounds` × `contentsScale` (по умолчанию); `contentsScale` для **самоуправляемого** слоя надо ставить вручную (default 1.0, но 0 при кривой инициализации даёт 0×0). `[ВЫВОД]` 0×0 → nil-drawable (см. B1). **Диагностика:** залогировать `layer.bounds`, `contentsScale`, `drawableSize`. **Фикс:** выставить `contentsScale` (2.0 retina), ненулевой размер до первого present (winemac.drv ставит `contentsScale` в `makeBackingLayer`).

**B3. Слой не привязан к видимому view.** `[ФАКТ]` CAMetalLayer композитится WindowServer только в layer-дереве **on-screen** окна (Apple: «typically displayed onscreen»; MoltenVK рекомендует делегатом слоя сделать сам view). `[ВЫВОД]` Слой, не вставленный в видимый view (или view заменил слой → MoltenVK `VK_ERROR_SURFACE_LOST_KHR`), не даёт кадра. **Диагностика:** проверить, что Metal-view добавлен в content-view (winemac.drv `addSubview:positioned:NSWindowBelow`) и окно on-screen. **Фикс:** привязать слой к видимому view на главном потоке.

**B4. present/commit не флашится.** `[ФАКТ]` Apple: `commandBuffer.present(drawable)` нужно вызвать **до** `commit()`; при `presentsWithTransaction = YES` нельзя пользоваться `present(_:)`. `[ВЫВОД]` Пропущенный `commit()`/`present` или рассинхрон с CA-транзакцией → кадр не доходит. **Диагностика:** трейс `present*`+`commit` (DXMT — `EncoderType::Present` → `presentDrawable*` → `CommitChunkInternal`). **Фикс:** соблюсти порядок `nextDrawable → encode → present → commit`.

**B5. Слой/view созданы не на главном потоке.** `[ФАКТ]` Apple/практика: инициализация CAMetalLayer/view вне main thread → drawables начинают падать в nil. winemac.drv намеренно делает это `OnMainThread`. **Диагностика:** проверить поток создания слоя. **Фикс:** создание/привязка слоя только на главном потоке.

**B6. vsync/тайминг.** `[ФАКТ]` На уровне слоя vsync — `displaySyncEnabled`; `presentsWithTransaction` меняет модель синхронизации; тайм-варианты `present(_:atTime:)`/`afterMinimumDuration:`. `[?]` Прямой связи «CADisplayLink обязателен для первого кадра» в reference-доках Apple не нашёл — это таймер цикла, не условие показа. **Фикс:** для первого кадра достаточно одного `present`+`commit`, display-link не обязателен.

**B7. Регрессия трансляции (НЕ window-server).** `[ФАКТ]` Множество «чёрных экранов» в Whisky/CrossOver/GPTK лечатся сменой версии GPTK (Whisky disc. #968, issue #849); часть — баги шейдеров/LUT (MoltenVK #1741: UE4 CombineLUTs даёт чёрный кадр). **Диагностика:** окно видно, рендер чёрный → это класс B, а не A. **Фикс:** откат/смена версии GPTK/транслятора.

---

## 4. Про «WINDOW_SERVER_ERROR» и «тихий спин без present»

`[ФАКТ]` Точная строка `WINDOW_SERVER_ERROR` **не встречается** ни в одном публичном исходнике/трекере macOS, Wine, MoltenVK, DXVK, DXMT, GPTK (поиск по вебу и GitHub даёт только посторонний Windows-шум). `[ВЫВОД]` Это, почти наверняка, **собственный enum/лейбл вызывающего кода (вашего лаунчера)**, обозначающий реальный класс сбоя — **невозможность соединиться с macOS WindowServer**. Настоящие симптомы этого класса: `_CGSDefaultConnection() is NULL`, `kCGErrorRangeCheck`/`kCGErrorFailure`, `FAILED to establish the default connection to the WindowServer`, и для Wine — отказ `macdrv_init` по `sessionHasGraphicAccess` (см. A1). `[ВЫВОД]` «Тихий спин без present» — закономерен при B1: `nextDrawable` отдаёт nil → MoltenVK гоняет пересоздание swapchain / DXMT блокируется в `nextDrawable`, кадр не выходит, ошибки нет.

`[?]` Утверждения «первый кадр специально отбрасывается» / «окно обязано быть в фокусе до первого present» первоисточником **не подтверждены**; реальные смежные механизмы — nil-drawable (B1) и политика активации/видимость (A2–A4).

---

## 5. Минимум, чтобы Metal-кадр стал виден (сводка)

`[ФАКТ/ВЫВОД]` Процесс: GUI/Aqua-сессия (не SSH/daemon) → `sessionHasGraphicAccess == YES`; политика активации доведена до `.regular` (или хотя бы `.accessory`); приложение реально показало окно (`orderFront`). Окно/слой: `NSWindow` on-screen, `WineContentView` layer-backed, `CAMetalLayer` привязан к видимому view на главном потоке, с непустым `device`, валидным `pixelFormat`, `contentsScale > 0` и `drawableSize > 0`. Порядок кадра: `nextDrawable` (≠ nil) → encode render-pass в `drawable.texture()` → `commandBuffer.present(drawable)` **до** `commit()` → `commit()`. После завершения буфера WindowServer композитит кадр на экран.

---

## Источники (первоисточники)

**DXMT:** `d3d11_swapchain.cpp`, `dxmt_presenter.cpp`, `dxmt_context.cpp`, `dxmt_command_queue.cpp`, `winemetal/unix/winemetal_unix.c`, `dxmt.conf` — https://github.com/3Shain/dxmt (raw: `raw.githubusercontent.com/3Shain/dxmt/main/...`).
**DXVK:** `src/dxvk/dxvk_presenter.cpp` — https://github.com/doitsujin/dxvk.
**MoltenVK:** `Docs/MoltenVK_Runtime_UserGuide.md`, `MoltenVK_Configuration_Parameters.md`, `MVKSurface.mm`, `MVKSwapchain.mm`, `MVKImage.mm` — https://github.com/KhronosGroup/MoltenVK; `VK_EXT_metal_surface` — https://registry.khronos.org/vulkan/specs/latest/man/html/VK_EXT_metal_surface.html.
**Wine winemac.drv:** `macdrv.h`, `macdrv_cocoa.h`, `window.c`, `cocoa_window.m`, `cocoa_main.m`, `cocoa_app.m`, `cocoa_opengl.m`, `opengl.c`, `vulkan.c`, `surface.c`, `macdrv_main.c` — https://gitlab.winehq.org/wine/wine/-/tree/master/dlls/winemac.drv (зеркало raw: `raw.githubusercontent.com/wine-mirror/wine/master/dlls/winemac.drv/...`).
**Apple:** CAMetalLayer — https://developer.apple.com/documentation/quartzcore/cametallayer ; nextDrawable() — https://developer.apple.com/documentation/quartzcore/cametallayer/nextdrawable() ; drawableSize — .../drawablesize ; device — .../device ; framebufferOnly — .../framebufferonly ; maximumDrawableCount — .../maximumdrawablecount ; contentsScale — https://developer.apple.com/documentation/quartzcore/calayer/contentsscale ; presentsWithTransaction — .../presentswithtransaction ; MTLCommandBuffer present(_:) — https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:) ; NSApplication.ActivationPolicy — https://developer.apple.com/documentation/appkit/nsapplication/activationpolicy-swift.enum ; Managing your game window for Metal — https://developer.apple.com/documentation/Metal/managing-your-game-window-for-metal-in-macos.
**WindowServer/сессии:** Apple DTS/TN2083 thread — https://developer.apple.com/forums/thread/765060 ; incomplete activation — https://developer.apple.com/forums/thread/756322 ; nextDrawable nil — https://developer.apple.com/forums/thread/15102 , https://github.com/gfx-rs/gfx/issues/2460.
**Чёрный экран (практика):** MoltenVK #1665 — https://github.com/KhronosGroup/MoltenVK/issues/1665 ; MoltenVK #1741 — https://github.com/KhronosGroup/MoltenVK/issues/1741 ; Whisky disc. #968 — https://github.com/orgs/Whisky-App/discussions/968 ; Whisky #849 — https://github.com/Whisky-App/Whisky/issues/849 ; Wine bug 50790 — https://bugs.winehq.org/show_bug.cgi?id=50790.
