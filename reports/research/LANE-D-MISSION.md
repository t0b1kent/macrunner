# LANE D — STANDING MEGA-MISSION: Graphics / DXMT (D3D11 → Metal)

You are **Lane D**. This is a **months-long standing mission**, NOT a checklist you finish.
It is "done" ONLY when the COMPLETION CRITERIA below are ALL met. After you finish ANY item,
**immediately pick the next from the BACKLOG and keep going** — do NOT stop, do NOT declare the
mission complete, do NOT wait for a new prompt. Only the three STOP conditions at the end are valid
reasons to yield.

Read `reports/research/AGENT-TEAM-OWNERSHIP.md` (the law).

## OWNED FILES (edit ONLY these)
- `engine/dxmt/**`, `engine/vkd3d/**`, `engine/graphics/**`
**NEVER touch:** `engine/wine/dlls/ntdll/**` (Lane A/C), `engine/hyperbridge/**` (Lane A/B),
the golden x64 snapshot.

## YOUR OWN SMOKE ENVIRONMENT (authorized 2026-06-02 — DLL-binding is IN SCOPE, not a STOP)
You own and manage a **dedicated, isolated** DXMT smoke-test runtime so you can bind YOUR built
DLLs without depending on Lane A/C:
- Use a **separate WINEPREFIX** (e.g. `artifacts/dxmt-smoke-prefix/`), NEVER the shared
  `dist-arm64ec-spike` (Lane A's HK runs use it — leave it alone).
- INSTALL your freshly-built `d3d11.dll` / `dxgi.dll` / `winemetal.dll` into that prefix's
  system32 (and syswow64 if 32-bit) and/or a local dll dir, and force them with
  `WINEDLLOVERRIDES="d3d11,dxgi,winemetal=n"` (native). The earlier `c0000135` means the built
  DLLs weren't on the search path — fix that by installing them where Wine looks, in YOUR prefix.
- Your smoke runner script (`engine/graphics/scripts/run_dxmt_d3d11_headless_smoke.sh`) owns this
  setup end-to-end. Making Wine load YOUR DXMT DLLs in YOUR prefix is YOUR job — **do NOT stop for
  it; solve it.** Only escalate if it genuinely requires editing ntdll/loader (Lane A/C) — then
  record the precise need in `reports/research/LANE-D-NEEDS.md` and KEEP WORKING other backlog
  items meanwhile (don't idle).
- If copying built artifacts into a prefix/install dir, that's fine (artifacts/own-prefix only);
  never modify the shared `engine/wine/dist*` tree Lane A uses.

## COMPLETION CRITERIA (NOT done until ALL hold)
1. A real Unity D3D11 game renders a live window through DXMT→Metal (device→swapchain→present→
   frames), at interactive frame rates, stable over minutes.
2. DXMT covers the D3D11 feature surface real games use: feature levels 10_0–11_1, all common DXGI
   formats, all shader stages (VS/PS/GS/HS/DS/CS), buffers/textures/views, all state objects,
   draw/dispatch/instancing/indirect, queries, multi-RT, MSAA, mips, compute. Coverage matrix shows
   no user-mode gaps.
3. A headless D3D11 smoke-test suite (yours) passes end-to-end through DXMT→Metal.
4. `reports/research/DXMT-D3D11-COVERAGE.md` is a living matrix: every D3D11/DXGI entry point +
   format + cap marked implemented / partial / gap, updated as you go.
Treat "done" as asymptotic (the D3D11 surface is large) — keep closing gaps.

## BACKLOG (work top-to-bottom; always have a next item; loop until COMPLETION)
### Phase 1 — Inventory + smoke harness (build-ahead; do NOT wait on Lane A)
- Inventory DXMT's current D3D11/DXGI coverage → publish `DXMT-D3D11-COVERAGE.md` matrix.
- Stand up a headless D3D11 smoke test you control: `D3D11CreateDevice` → feature level →
  swapchain or offscreen RTV → clear to color → present/readback. Drive it GREEN through
  DXMT→Metal. (Wine ships d3d11 tests; or a minimal harness.) This validates the path with NO game.
### Phase 2 — Device-create + the Unity probe set (the window's last rungs)
- Make the exact create path Unity uses succeed: feature level 11_0 (fallback 10_x), BGRA support,
  `CheckFormatSupport`/`CheckFeatureSupport` for the formats Unity probes, `CreateDXGIFactory`,
  adapter/output enumeration, `CreateSwapChain(ForHwnd)` (flip + bitblt). A failed format/feature
  probe makes Unity refuse to start — close every one.
### Phase 3 — Resource & pipeline coverage (bulk)
- Buffers (vertex/index/constant/structured/UAV), textures 1D/2D/3D/cube + arrays, all views
  (SRV/RTV/DSV/UAV), samplers, all state objects (blend/depth-stencil/rasterizer), input layouts,
  map/unmap + usage/CPU-access. Cover the finite DXGI format table as a matrix.
### Phase 4 — Shaders & draw/dispatch
- DXBC → Metal (or via existing path): VS/PS first, then GS/HS/DS, then CS (compute). Draw,
  DrawIndexed, instanced, indirect; dispatch; multi-RT; viewport/scissor; queries/predication.
### Phase 5 — Presentation & correctness
- Swapchain present modes, vsync, resize, fullscreen↔windowed, MSAA resolve, sRGB, gamma, mip
  generation. Validate pixel-correctness vs reference for a set of render cases.
### Phase 6 — Performance & stability
- Frame pacing, resource residency, command-buffer batching, multithreaded-rendering (Unity render
  thread) correctness, long-run stability (no leaks/crashes over minutes).
### Phase 7 — D3D8/9 path note (for older games later, e.g. GTA Vice City)
- Document what a D3D8/9→Metal (or →DXMT-11 translation) path needs; do NOT build yet — flag for a
  future lane.

## DISCIPLINE
- Build via the project graphics build; run tests under `timeout`; NEVER global `pkill wine`
  (scoped `wineserver -k`). Avoid running heavy graphics tests at the same instant as Lane A's full
  HK boot (CPU/GPU contention). `./scripts/disk-guard.sh` before long cycles.
- Evidence over status: paste device-create result / feature level / pixel readback / frame counts.
- NEVER `git add -A`; commit only your files as `checkpoint(Lane D): ...` / `feat(Lane D): ...`.

## STOP conditions (ONLY these — otherwise keep looping the backlog for MONTHS)
1. ALL completion criteria met (report with a screenshot/frame evidence).
2. Hard external blocker that genuinely requires editing another lane's files (ntdll/loader = A/C)
   AND you've tried 3 distinct in-scope workarounds — record it in `LANE-D-NEEDS.md` and KEEP
   WORKING other backlog items (do NOT idle; the backlog is huge — Phases 1–7).
3. Operator decision on a real graphics trade-off (e.g. "Metal lacks feature X — stub or emulate?").

**NOT stop conditions (solve these yourself, keep going):** DLL-binding / WINEDLLOVERRIDES / your
own smoke-prefix install / build-path issues / a single failing smoke case. These are in-scope.
A blocker on ONE item never stops the mission — move to the next backlog item and come back.
