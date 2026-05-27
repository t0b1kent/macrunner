# KIMI-TASK — DXMT Bring-Up: First Pixel → Game-Readiness MEGA Master Brief

**Date:** 2026-05-28
**Agent:** Kimi (read-only research/forensics; NO engine edits)
**Working copy:** DXMT worktree `/Volumes/MacOS 1/MacRunner-dxmt-truth-gate-20260527/`
(внешний golden). НЕ трогать PE32/WOW64/xtajit/HyperBridge активного Codex-лейна; x64
golden — только read-only.
**Predecessors:** `reports/research/DXMT-PRESENT-PATH-MAP-20260527.md` (present-path
размечен), `reports/DXMT-IMPLEMENTATION-MARATHON-20260527.md` (smoke-харнесс собран, упёрся
в краш до D3D11 init).

---

## Mission
Present-path размечен и DLL деплоятся, но truth-gate НЕ пройден: нативный aarch64
`dx11_clear_present` **крашится ДО инициализации D3D11** (Level 3→4, WINE_RUNTIME_CRASH).
Этот MEGA-бриф ведёт от текущего краша к **первому реальному пикселю на Metal** и дальше
к **карте готовности к играм**. Read-only research → даёт Codex implementation lane.

Принцип: корректный D3D→Metal слой (как стоковый GDI-рендер), НЕ подгонка. Verified =
реальный непустой кадр в CG-window capture, НЕ «DLL загрузились», НЕ «процесс не упал».

---

## PHASE A — Forensics: краш ДО D3D11 init (ИММЕДИАТНЫЙ блокер первого пикселя)
Цель: назвать ТОЧНУЮ причину, почему нативный aarch64 `dx11_clear_present` падает до
D3D11 init. Изолированно от HyperBridge (нативный aarch64 Wine, как и рекомендовал Kimi).
Исследовать (file:line + evidence):
1. CRT/startup нативного aarch64 PE: static vs dynamic CRT (марафон пробовал static CRT —
   результат?), entry point, какой первый вызов падает (lldb backtrace из run-dir
   `reports/phase-h/dx11_clear_present-20260527-17*`).
2. DLL dependency chain нативного dx11_tri: что грузится до D3D11CreateDevice; падает ли
   на загрузке/резолве (d3d11→dxgi→winemetal→winemac.so symbols) или в самом startup.
3. winemetal.so ↔ winemac.so symbol resolution (из present-path map 2.4-2.5): резолвятся
   ли символы; не тут ли краш.
4. Классифицировать: краш в (a) CRT/PE-startup, (b) DLL-load/resolve, (c) D3D11 device
   create, (d) swapchain/CreateMetalViewFromHWND. Дать точку + что Codex чинит/где.
Выход: точная локализация краша + гипотеза фикса в правильном слое (НЕ обход).

## PHASE B — D3D11 → Metal feature-coverage matrix
Карта: что DXMT реально реализует vs стаб vs отсутствует, от первого треугольника к игре.
По исходнику DXMT (engine/graphics/.../dxmt) + present-path map:
- Device/Context: D3D11CreateDevice, immediate/deferred context.
- Swapchain/Present: что покрыто (из present-path), флипы/режимы.
- Resources: buffers, textures (2D/3D/cube), views (SRV/RTV/DSV/UAV), mapping.
- **Shaders (ядро сложности):** DXBC→? путь (AIR/Metal/MSL), какой компилятор/транслятор,
  что покрыто (VS/PS/CS/GS/HS/DS), где дыры. Это главный риск для игр — оцени глубоко.
- Draw/State: draw/drawIndexed/instanced, blend/depth/raster/sampler state, input layout.
- Таблица: фича | DXMT статус (есть/стаб/нет) | file:line | риск для игр | reference.

## PHASE C — Game-readiness ladder + smoke corpus
Конкретный корпус проб (по лестнице Obsidian 111), что каждая проверяет:
1. clear_present (есть) → 2. triangle (есть) → 3. textured quad → 4. many-draws/instancing
→ 5. маленькая standalone DX11-игра БЕЗ launcher/DRM/anti-cheat.
Для каждой: что exercise-ит (какие фичи из Phase B), smoke-чеклист (device/swapchain/
present/shader/непустой кадр/crash), как захватить (cg_window_capture). Назови конкретных
кандидатов на ступень 5 (мелкие DX11-демки/инди без launcher).

## PHASE D — Reference backends (что заимствовать на дыры)
DXVK / DXMT upstream / MoltenVK / DXVK-native: для каждой дыры из Phase B — есть ли готовое
решение в reference (REFERENCE-BACKENDS.md уже есть в worktree — свериться, не дублировать),
что переносимо, что нет (лицензия/архитектура).

---

## Truth-gate дисциплина (анти-fake-PASS)
- ПЕРВЫЙ пиксель — нативно aarch64 под ARM64 Wine, БЕЗ HyperBridge (изоляция GPU-пути от
  x86-трансляции; x86-игры — позже, отдельный слой).
- Verified = непустой кадр в CG-window capture (tools/cg_window_capture.swift +
  analyze_capture.py), НЕ загрузка DLL, НЕ «процесс жив».
- Уровни: 1 DLL загружены → 2 device create → 3 swapchain → 4 present → 5 непустой кадр.
  Сейчас застряли 3→4 (краш до init). Не засчитывать уровень без доказательства.

## Output artifacts (write_to_file)
- `reports/research/DXMT-BRINGUP-FORENSICS-AND-COVERAGE-<date>.md` — Phases A-D, таблицы
  file:line, локализация краша, feature-matrix, game-ladder, reference-mapping.
- Раздел «Handoff to Codex».

## Handoff to Codex
1. Порядок Codex: (A) фикс краша до D3D11 init → первый пиксель (truth-gate L5), (B) закрыть
   приоритетные дыры feature-matrix под ступень ladder, (C) подниматься по лестнице с smoke.
2. Не смешивать с PE32-лейном на одном дереве (урок регрессии x64, Obsidian 114) — DXMT в
   своём worktree/golden.
3. Verified = реальный кадр; обновить ENGINE-CHANGE-JOURNAL + ACTIVE-INVESTIGATION
   (графический лейн отдельной секцией).

## Границы Kimi
READ-ONLY. Реальный fork-vs-stock по исходникам (engine/..., не wine-fork/). Не объявлять
«работает» без present-доказательства. write_to_file для отчёта, без heredoc/опасного shell.
Если объём велик — Phase A (краш, разблокировка) ПОЛНОСТЬЮ первой, B/C/D следом; частично
+ запись лучше зависа.
