# KIMI MANDATE — Graphics Core Implementer: SHIP PIXELS, DON'T REPORT

**Date:** 2026-05-28
**Role change:** Kimi больше НЕ read-only research-агент по графике. Kimi теперь
**ВЛАДЕЛЕЦ и ИМПЛЕМЕНТАТОР MacRunner Graphics Core** — пишет код, собирает, запускает,
чинит блокеры, итерирует ДО реального результата на экране. Это долгий мандат (месяцы),
не разовая задача.
**Working copy:** DXMT worktree `/Volumes/MacOS 1/MacRunner-dxmt-truth-gate-20260527/`.

## ГЛАВНОЕ ПРАВИЛО (господин сказал прямо)
ХВАТИТ ПИСАТЬ ОТЧЁТЫ. Deliverable = **работающий код + реальные пиксели/игры**, НЕ markdown.
- НЕ создавай новые research-报告/`*-MAP-*.md`/`*-AUDIT-*.md`. Анализ → сразу в КОД.
- Единственная допустимая запись: ОДИН живой `docs/GRAPHICS-CORE-STATUS.md` (как
  ACTIVE-INVESTIGATION: что сделано / текущий блокер / next) + нормальные git-коммиты.
- Если поймал блокер — РЕШАЙ его (собери/слинкуй/задеплой/поправь/перезапусти), а не
  описывай. «Закончил» = пиксель на экране или игра запустилась, не «отчёт готов».

## ГРАНИЦЫ ВЛАДЕНИЯ (чтобы не было merge-hell с Codex)
- Kimi РЕДАКТИРУЕТ: `engine/graphics/**` (DXMT, winemetal, dist, build), графический smoke-
  харнесс (`tools/smoke/**`, `dx11_*`), deploy/sync для графики, game-runtime
  (`app/game_runtime/**`), профили игр. + свой worktree.
- Kimi READ-ONLY: `engine/hyperbridge/**`, `engine/wine/dlls/**` (CPU-движок — территория
  Codex). Нужна правка там — НЕ делай сам, опиши Claude'у одной строкой, передам Codex.
- x64 golden — read-only эталон. PE32-лейн Codex — не трогать.
- Работай в worktree на внешнем golden; не сливай в локальный Codex-репо без согласования.

## ЦЕЛЬ (north star)
```
x64 Windows game.exe → ARM64 Wine → HyperBridge x64 → x86_64-windows DXMT frontend
 → WINE_UNIX_CALL → aarch64-unix winemetal.so → Metal → Apple GPU
```
Нативно. Не Rosetta, не WineD3D/OpenGL fallback.

## МАЙЛСТОУНЫ (делай по порядку, каждый = РЕАЛЬНЫЙ результат, не отчёт)
**M1 — Первый пиксель (разблокировать прямо сейчас):**
- Почему грузился WineD3D, а не DXMT: задеплой x86_64 DXMT DLL в prefix + DLL overrides
  (`d3d11,dxgi,d3d10core,winemetal=native,builtin`); СОЗДАЙ `engine/graphics/dist/dxmt/
  x86_64-unix/winemetal.so` symlink → `aarch64-unix/winemetal.so` (loader-путь для
  WINE_UNIX_CALL, НЕ Rosetta); почини unixlib-границу x86_64 winemetal.dll → ARM64 .so.
- Доведи x86_64 `dx11_clear_present`/`dx11_tri_nocrt.exe` через HyperBridge до НАСТОЯЩЕГО
  DXMT (не WineD3D) → device → swapchain → Present → **непустой кадр в CG-window capture**.
- Это MVP-1. Чини все блокеры по пути сам (краш до D3D11 init и т.д.).

**M2 — Smoke-корпус зелёный:** dx11_clear_present → triangle → texture_quad → many_draws,
все дают реальный кадр через HyperBridge+DXMT+Metal.

**M3 — Первая настоящая x64 D3D11-игра рисует кадр:** Risk of Rain 2 → Skyrim SE →
Fallout 4 → Dark Souls III (без launcher/DRM/anti-cheat для первой). Доведи до играбельного
кадра, чини feature-дыры по ходу.

**M4 — Feature coverage:** закрой шейдеры (DXBC→AIR/Metal: VS/PS/CS/GS/HS/DS), ресурсы,
draw/state — столько, сколько нужно корпусу игр M3. Заимствуй из DXVK/MoltenVK где можно.

**M5 — MacRunner Graphics Core:** telemetry (FPS, Present, draw calls, shader compile ms,
pipeline miss, CPU-bridge calls, bottleneck CPU/GPU/shader/upload/bridge), shader+pipeline
cache (персист/prewarm), game-profiles (exe-hash → backend/overrides/prewarm/fastpaths/
MetalFX/render-scale), HyperBridge-aware fast thunks. Цель: не просто FPS, а меньше фризов.

## ДИСЦИПЛИНА
- Verified = РЕАЛЬНЫЙ непустой кадр (cg_window_capture + analyze_capture), НЕ «DLL есть»,
  НЕ «процесс жив», НЕ mock-PASS. Уровни L1..L5, не засчитывай без пикселя.
- Корректный D3D→Metal слой, не подгонка под одну игру.
- Работай АВТОНОМНО и долго: уперся в блокер → реши и иди дальше; не останавливайся на
  «отчёт написан». Обновляй GRAPHICS-CORE-STATUS.md кратко по ходу.
- .clinerules для shell (без heredoc/опасного квотинга). Исходники Wine для read-only
  сверки — `engine/wine/dlls/**`, не `wine-fork/`.

## ESCALATION к Claude (координатор)
Только если: нужна правка в CPU-движке Codex (hyperbridge/wine) ИЛИ конфликт worktree/
golden ИЛИ архитектурное решение. Одной строкой — передам/разрулю. Иначе — РЕШАЙ САМ.
