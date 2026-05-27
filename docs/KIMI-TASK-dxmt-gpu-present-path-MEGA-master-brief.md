# KIMI-TASK — DXMT GPU Present-Path Integration MEGA Master Brief

**Date:** 2026-05-27
**Agent:** Kimi (read-only research/discovery; NO engine edits)
**Working copy:** golden snapshot (внешний диск) `/Volumes/MacOS/MacRunner` ИЛИ
синхронный `/Users/timurtoby/Documents/MacRunner/Main/MacRunner` — ОБА только read-only
для графики; PE32/WOW64/xtajit/HyperBridge активного Codex-лейна НЕ ЧИТАТЬ-НЕ-ТРОГАТЬ.
**Predecessor:** `reports/KIMI-DXMT-FIRST-IMPLEMENTATION-LANE-20260527.md` (readiness audit).

---

## Mission
Довести DXMT (DirectX→Metal) от «DLL присутствуют» до **«пиксель на экране»**. Readiness
уже доказан Kimi (router верный, безопасно начинать). Этот бриф картирует ПОЛНЫЙ
present-путь + deploy-фикс + truth-gate, чтобы Codex имплементировал первый кадр на Metal,
а не получил fake-PASS «DLL загрузились».

Принцип: корректный D3D→Metal слой (как стоковый GDI-рендер), НЕ подгонка под одну игру.
Verified = реальный пиксель в окне (CG-window capture), НЕ наличие DLL/факт загрузки.

---

## Phase 1 — Deploy-фикс (это #1 рантайм-блокер)
Из readiness-аудита: `scripts/sync-prefix-from-dist.sh` копирует core Wine DLL, но НЕ
DXMT DLL → в prefix их нет → D3D11 app падает на загрузке.
Задача (read-only спека для Codex):
- Какие именно DXMT-артефакты лежат в `engine/graphics/dist/dxmt/aarch64-windows/`
  (d3d11.dll, dxgi.dll, d3d10core.dll, winemetal.dll, + .so unix-часть если есть) — точный
  список с путями/размерами.
- Куда в prefix их класть (system32 + arch-аналоги), и как это добавить в
  sync-prefix-from-dist.sh (спека diff, НЕ правка — Codex применит).
- Совместимость с verify-build-freshness: добавить ли DXMT в arch_pair/prefix_synced
  проверки (чтобы DXMT-дрейф ловился как core DLL — урок из 64-битного build-whack-a-mole).

---

## Phase 2 — Present-path integration map (file:line)
Цепочка «D3D11 draw → пиксель на Mac-экране». Дай точные file:line вставки/связки:
1. **DXMT swapchain/present** наружу: где DXMT (d3d11/dxgi) отдаёт готовый кадр
   (IDXGISwapChain::Present → Metal texture/drawable). Источники в engine/graphics/dist/dxmt
   или его исходник, если доступен.
2. **Wine surface boundary:** `engine/wine/dlls/winemac.drv/surface.c` (surface struct,
   flush, create/destroy), `cocoa_window.m` (создание окна, layer setup), `macdrv.h`
   (surface/window структуры), `d3dmetal.c` + `d3dmetal_objc.m` (существующие Metal-interop
   стабы) — ГДЕ DXMT-выход подключается к окну.
3. **IOSurface / CAMetalLayer:** точки, где Wine GDI-поверхность окна должна стать
   CAMetalLayer-backed (или IOSurface-shared) для GPU present, НЕ ломая GDI-семантику
   (toolbar/menu остаются GDI). CALayer vs NSView backing tradeoffs.
4. **Entitlements:** `com.apple.security.cs.allow-jit` (Metal/IOSurface могут требовать),
   что уже в wine.entitlements, чего не хватает.
Для каждого звена: file:line, что делает, fork-vs-stock (DXMT/winemac трогали или сток),
риск, и КУДА Codex поставит фактический код/трассу.

---

## Phase 3 — Truth-gate + первая GPU-проба (анти-fake-PASS)
- Выбери ПЕРВУЮ лёгкую GPU-цель: standalone **DX11** (или DX9) **без launcher/DRM/
  anti-cheat** (по лестнице из Obsidian 111: не AAA/DX12). Кандидаты — маленькие
  DX11-сэмплы/демки, не Steam-тайтлы.
- Smoke-чеклист (как доказать present РЕАЛЬНО на экране):
  device create → swapchain create → present → shader compile → есть ли кадр (CG-window
  pixels не чёрные) → crash/нет. Логи: D3D device/swapchain/present/shader compile /
  unsupported opcode / native faults.
- Явно: «DLL загрузились» ≠ present. Truth-gate = непустой кадр в CG-window capture.

---

## Output artifacts (write_to_file)
- `reports/research/DXMT-PRESENT-PATH-MAP-<date>.md` — Phases 1-3, таблицы file:line,
  fork-vs-stock, deploy-спека, truth-gate план, smoke-чеклист, рекомендованная первая
  GPU-проба.
- Раздел «Handoff to Codex» (см. ниже).

## Handoff to Codex (что делать с результатом)
1. Codex берёт карту ТОЛЬКО когда не занят активным PE32-лейном (или на golden-копии
   отдельной сессией) — НЕ смешивать с 32-бит на одном дереве (урок регрессии x64 → 114).
2. Порядок Codex: (a) deploy-фикс sync-prefix (Phase 1), (b) present-path по карте (Phase 2),
   (c) truth-gate на первой GPU-пробе (Phase 3).
3. Verified = реальный кадр на экране (CG-pixels), обновить ENGINE-CHANGE-JOURNAL +
   ACTIVE-INVESTIGATION (графический лейн отдельной секцией, не мешать PE32).

## Границы Kimi
READ-ONLY. Не трогать PE32/WOW64/xtajit/HyperBridge файлы (активный Codex-лейн). Реальный
fork-vs-stock (как делал для wake/drain — engine/wine/dlls/, не wine-fork/). Не объявлять
«работает» без present-доказательства. write_to_file для отчёта, без heredoc/опасного shell.
