# KIMI-TASK — DXMT Bring-Up: First Pixel → MacRunner Game Runtime MEGA Master Brief

**Date:** 2026-05-28 (v2 — исправлено: цель = x86_64 PE через HyperBridge, НЕ aarch64-native)
**Agent:** Kimi (read-only research/forensics; NO engine edits)
**Working copy:** DXMT worktree `/Volumes/MacOS 1/MacRunner-dxmt-truth-gate-20260527/`.
НЕ трогать PE32/WOW64/xtajit/HyperBridge активного Codex-лейна; x64 golden — read-only.
**Predecessors:** `reports/research/DXMT-PRESENT-PATH-MAP-20260527.md`,
`reports/DXMT-IMPLEMENTATION-MARATHON-20260527.md`.

---

## ЦЕЛЕВАЯ ЦЕПОЧКА (north star — как можно нативнее)
```
Windows x64 game.exe
 → ARM64 Wine host
 → HyperBridge x64 (исполняет CPU-код игры)
 → x86_64-windows D3D frontend DLLs = DXMT (НЕ WineD3D, НЕ OpenGL fallback)
 → (WINE_UNIX_CALL) → aarch64-unix winemetal.so (нативный ARM64 Metal backend)
 → Apple GPU
```
Игра остаётся Windows x64; горячая графика уходит в нативный ARM64 D3D→Metal, НЕ в Rosetta
и НЕ в WineD3D/OpenGL. Verified = реальный непустой кадр в CG-window capture + метрики.

## ПОПРАВКА к v1 (важно — НЕ повторять)
- aarch64-native PE smoke = ТУПИК: тестирует не ту конфигурацию (игры — x86_64 PE, не
  aarch64 PE) и упёрся в посторонний `WINE_UCRT_ARM64_BUG` (`vsnscanf_l`). НЕ идти туда.
- Правильный smoke = **x86_64 `dx11_*_nocrt.exe` через HyperBridge** с НАСТОЯЩИМ DXMT.
- Verified-факт (Claude): x86_64-windows DXMT DLL РЕАЛЬНО собраны и лежат в
  `engine/graphics/dist/dxmt/x86_64-windows/` (d3d11 5.28MB, dxgi, d3d10core, winemetal —
  символы `dxmt::`, не WineD3D). НО в рантайме грузился WineD3D → wined3d.dll → opengl32
  → non-application-target. Причина: DXMT не задеплоен/не оверрайднут + НЕТ `x86_64-unix/`.

---

## PHASE A — Реальный DXMT x86_64 путь загружается (ИММЕДИАТНЫЙ блокер MVP-1)
Цель: чтобы x86_64 `d3d11.dll`/`dxgi.dll` в рантайме был DXMT, а НЕ WineD3D, и unixlib-
граница до ARM64 winemetal.so работала. Исследовать (read-only, file:line, спека для Codex):
1. **Deploy x86_64 DXMT в prefix:** скопировать `dxmt/x86_64-windows/{d3d11,dxgi,d3d10core,
   winemetal}.dll` в prefix system32; добавить в sync-prefix-from-dist.sh. + DLL overrides
   (`d3d11,dxgi,d3d10core,winemetal = native,builtin`), чтобы DXMT выигрывал у встроенного
   WineD3D. Проверить, что загруженный d3d11 = DXMT (символы dxmt::), не WineD3D (нет
   импорта wined3d/opengl32).
2. **`x86_64-unix/winemetal.so` — СОЗДАТЬ как symlink → `aarch64-unix/winemetal.so`**
   (loader-путь для WINE_UNIX_CALL; это НЕ Rosetta, это ARM64 .so по x86_64-unix имени).
   Сейчас `x86_64-unix/` ОТСУТСТВУЕТ. Проверь по исходнику Wine loader, что он принимает
   ARM64 .so по этому пути для x86_64 PE unixlib (как именно резолвится __wine_unix_call).
3. **x86_64 winemetal.dll → winemetal.so boundary:** из present-path map (2.4-2.5) —
   как PE-thunk x86_64 winemetal.dll вызывает unix winemetal.so через HyperBridge/WINE_UNIX_CALL;
   что должно быть на месте, чтобы вызов прошёл, а не упал в non-application-target.
4. Классифицировать оставшийся краш `dx11_clear_present` (марафон, L3→4) ИМЕННО на
   x86_64-через-HyperBridge пути с НАСТОЯЩИМ DXMT (не WineD3D, не aarch64): где падает —
   CRT/startup, DLL-load, D3D11CreateDevice, swapchain/CreateMetalViewFromHWND.
Выход: точная спека «что собрать/слинковать/задеплоить + DLL overrides» + локализация
оставшегося краха. Это разблокирует MVP-1.

## MVP-1 (ближайшая цель truth-gate)
x86_64 `dx11_tri_nocrt.exe` (или dx11_clear_present) через HyperBridge с НАСТОЯЩИМ DXMT:
загрузился exe → DXMT d3d11/dxgi (не WineD3D) → D3D11CreateDevice → swapchain →
Draw/Present → окно видно через CoreGraphics (непустой кадр). Уровни L1..L5, не засчитывать
без доказательства. Корпус: dx11_clear_present → dx11_triangle → dx11_texture_quad →
dx11_many_draws.

## PHASE B — D3D11 → Metal feature-coverage matrix
Что DXMT реально реализует vs стаб vs нет (по исходнику dxmt): Device/Context, Swapchain/
Present, Resources (buffers/textures/SRV/RTV/DSV/UAV/map), **Shaders DXBC→AIR/Metal — ядро
сложности** (VS/PS/CS/GS/HS/DS, какой транслятор, дыры), Draw/State. Таблица: фича | статус
| file:line | риск для игр | reference.

## PHASE C — Game-readiness ladder (x64 D3D11 first)
После MVP-1 PASS, по лестнице (Obsidian 111). Первые ИГРОВЫЕ цели — x64 D3D11 Windows-only:
1. Risk of Rain 2  2. Skyrim Special Edition  3. Fallout 4  4. Dark Souls III.
(GTA Vice City classic = 32-bit → ПОСЛЕ PE32/WOW64 лейна, не сюда.)
Для каждой: что exercise-ит из Phase B, smoke-чеклист, что мешает (launcher/CRT/TLS/audio/
input/services — почему сразу игру не берём, сначала smoke). Без launcher/DRM/anti-cheat
для первой настоящей.

## PHASE D — Reference backends (на дыры)
DXVK / DXMT upstream / MoltenVK: для каждой дыры Phase B — что заимствовать (свериться с
REFERENCE-BACKENDS.md в worktree, не дублировать).

## PHASE E — MacRunner Graphics Core (north star, после первого пикселя)
Не просто DXMT, а Game Runtime с проверяемыми метриками. Картировать, ГДЕ это внедрять:
- **Telemetry:** draw calls, Present count, shader compile ms, pipeline miss, CPU bridge
  calls, выбранный backend, bottleneck (CPU/GPU/shader/texture-upload/bridge).
- **Caches:** shader cache + pipeline cache (DXMT уже имеет ShaderCache — где, как
  персистить/prewarm).
- **Game profiles:** game_id/exe-hash → best backend, DLL overrides, shader prewarm list,
  pipeline hints, known-broken paths, fastpath flags, MetalFX/render scale. При повторном
  запуске MacRunner узнаёт игру → правильный backend + прогретые кэши + меньше фризов.
- HyperBridge-aware fast thunks для горячих CPU↔GPU границ.
Это research-карта (где хуки телеметрии/кэша/профилей), не имплементация.

---

## Output artifacts (write_to_file)
`reports/research/DXMT-X64-PATH-AND-GRAPHICS-CORE-<date>.md` — Phases A-E, таблицы file:line,
deploy/override/symlink спека, локализация краха на x86_64-DXMT пути, feature-matrix,
game-ladder, graphics-core hook map. + раздел «Handoff to Codex».

## Handoff to Codex
1. Порядок: (A) реальный DXMT x86_64 грузится + x86_64-unix symlink + override WineD3D →
   (MVP-1) первый пиксель через HyperBridge → (B) дыры feature-matrix под ladder → (C)
   подъём по лестнице → (E) Graphics Core слой.
2. Не смешивать с PE32-лейном на одном дереве (Obsidian 114) — DXMT в своём worktree.
3. Verified = реальный кадр + метрики; обновить ENGINE-CHANGE-JOURNAL + ACTIVE-INVESTIGATION
   (графический лейн отдельной секцией).

## Границы Kimi
READ-ONLY. Реальный fork-vs-stock по исходникам (engine/..., не wine-fork/). Не объявлять
«работает» без present-доказательства (непустой кадр, не «DLL есть»). write_to_file, без
heredoc/опасного shell. Если велико — Phase A (разблокировка MVP-1) ПОЛНОСТЬЮ первой,
B/C/D/E следом; частично + запись лучше зависа.
