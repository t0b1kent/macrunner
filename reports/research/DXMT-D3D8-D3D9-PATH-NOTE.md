# DXMT D3D8/D3D9 Path Note

Date: 2026-06-05
Scope: static research/design note only. No headless smoke, game run, or GPU validation was executed because Lane A is holding Hollow Knight GPU work.

Target: old 32-bit D3D8/D3D9 games, with GTA Vice City classic / RenderWare as the concrete D3D8 first target.

## Short Answer

Current MacRunner DXMT is a D3D11/DXGI to Metal backend, not a D3D8/D3D9 backend. The packaged DXMT path currently contains `d3d11.dll`, `d3d10core.dll`, `dxgi.dll`, `winemetal.dll`, and `winemetal.so`; it does not expose `d3d8.dll` or `d3d9.dll`.

For product correctness, the best native route is:

1. Keep the current router default for legacy games short-term: D3D8/D3D9 through DXVK-MoltenVK or WineD3D fallback.
2. Build a D3D9 frontend over the existing DXMT core, not a separate renderer.
3. Put D3D8 on top of D3D9 using the proven DXVK upstream pattern.
4. Treat D3D8/D3D9 to D3D11 as a possible prototype route, but not the clean long-term architecture.

Reason: D3D8/9 differ from D3D11 primarily in frontend semantics - fixed-function state, mutable global device state, lost/reset behavior, resource pools, lock/update rules, old shader models, caps, and state blocks. The Metal command/resource/present backend can be reused, but a substantial D3D9 compatibility frontend is still required.

## Current Tree Facts

- `engine/dxmt/src/d3d11`, `engine/dxmt/src/dxgi`, `engine/dxmt/src/dxmt`, `engine/dxmt/src/winemetal`, and `engine/dxmt/src/airconv` are the native DXMT path.
- `engine/graphics/dist/dxmt/{aarch64-windows,x86_64-windows}` packages D3D11/DXGI/Winemetal artifacts.
- `docs/GRAPHICS-ROUTER.md` currently routes DX7/8 to WineD3D and DX9 to DXVK, with DXMT as the primary D3D11 path.
- `reports/research/DXMT-D3D11-COVERAGE.md` tracks D3D11/DXGI coverage; it is not a D3D9 coverage matrix.
- `engine/dxvk-upstream/src/d3d8` already implements D3D8 as a wrapper over D3D9 concepts. This is the right design reference for D3D8.
- `engine/dxvk-upstream/src/d3d9` contains the broad D3D9 compatibility frontend: fixed-function, state blocks, shader handling, resources, swapchain/reset, queries, cursor, and multithread support.

## D3D8/D3D9 vs D3D11 Differences

| Area | D3D8/D3D9 | D3D11/DXGI | Impact |
|---|---|---|---|
| Device creation | `Direct3DCreate8/9`, adapter caps, `CreateDevice`, `D3DPRESENT_PARAMETERS` | `D3D11CreateDevice`, DXGI factory/adapter/swapchain | Need new DLL exports, COM objects, caps, adapter mode enumeration, and present-parameter translation. |
| Device lifetime | Device lost, `TestCooperativeLevel`, `Reset`, fullscreen mode changes | DXGI device removal, swapchain resize, fewer classic lost-device semantics | D3D9 reset/lost-device emulation is mandatory for old games. |
| State model | Mutable device state: render states, texture-stage states, transforms, lights, materials, fog, clip planes, samplers | Mostly explicit immutable state objects and shader resource views | Need a D3D9 state tracker and pipeline-key cache. |
| Fixed function | FVF, T&L, lighting, texture combiner stages, alpha test, fog, point sprites | No fixed-function pipeline | Need generated shaders or direct lowering to DXMT/Metal pipeline state. |
| Shaders | D3D8/9 shader models 1.x, 2.x, 3.0 token streams | DXBC SM4/SM5 style D3D11 shaders | Existing DXMT `airconv` is useful after lowering, but old shader bytecode needs a translator. |
| Resources | `D3DPOOL_DEFAULT/MANAGED/SYSTEMMEM/SCRATCH`, lock/unlock, dirty regions, `UpdateTexture`, `StretchRect` | usage/bind flags, map/update, views | Need pool manager, lock staging, dirty tracking, and managed-resource upload rules. |
| Views | Mostly implicit surfaces/textures/render targets | explicit RTV/DSV/SRV/UAV | D3D9 frontend must synthesize internal views and cache them. |
| Draw input | FVF/declarations, streams, index buffers, `DrawPrimitive`, `DrawIndexedPrimitive`, user-memory variants | input layouts, IA bindings, draw calls | Need FVF and declaration conversion to internal input layouts. |
| Presentation | `Present`, additional swap chains, backbuffer formats, intervals, gamma ramp, cursor | DXGI swapchains/presenter | DXGI/Presenter can be reused, but D3D9 present rules must be emulated. |
| Raster rules | D3D9 half-pixel/texel conventions, depth-bias quirks, alpha-test ordering | D3D11 raster state conventions | Must match old game pixels, especially UI, fonts, and RenderWare geometry. |
| Caps | Games branch hard on `D3DCAPS8/9` | D3D11 feature levels are different | Need curated caps that expose what the frontend actually emulates. |

## Architecture Options

### Option A: Keep DXVK/WineD3D for D3D8/D3D9

This is the current router policy and should remain the short-term compatibility path. It avoids blocking D3D11 DXMT stabilization and avoids building a large D3D9 frontend before the PE32/WOW64 lane is ready for 32-bit games.

Limit: it is not the native strategic path. DXVK-MoltenVK adds Vulkan translation between D3D9 and Metal, while WineD3D goes through OpenGL.

### Option B: D3D8 to D3D9 to DXMT Core

Recommended native route.

Layering:

```text
D3D8 game
  -> MacRunner d3d8.dll wrapper
  -> MacRunner D3D9 frontend
  -> DXMT core resources/context/presenter/shader cache
  -> winemetal.so / Metal
```

This mirrors the DXVK upstream approach where D3D8 is mostly a compatibility wrapper over D3D9. The D3D9 frontend should call DXMT core abstractions directly where possible instead of pretending to be a D3D11 application.

Why this is clean:

- D3D8 differences are smaller than D3D9 differences. D3D8 should not get its own renderer.
- DXMT core already owns Metal device selection, resources, command queues, presenter, shader cache, format mapping, staging, residency, and window/Metal layer plumbing.
- D3D9 state tracking can build stable internal pipeline keys without forcing every change through public D3D11 COM calls.

### Option C: D3D8/D3D9 to D3D11 to DXMT

Viable as an MVP/prototype, but risky as the production path.

Pros:

- Reuses the existing D3D11 COM frontend and D3D11 coverage matrix.
- Easier to instrument against current D3D11 fixtures.
- Can generate internal SM5 DXBC/HLSL for fixed-function and old shaders, then feed existing DXMT `airconv`.

Cons:

- Still requires almost the full D3D9 frontend: caps, reset/lost-device, resource pools, state blocks, FVF, texture stages, old shader lowering.
- D3D11 public API boundaries add overhead and mismatched semantics.
- D3D9 state churn can create excessive D3D11 state-object/shader/view churn unless aggressively cached.
- Some D3D9 behaviors map better to the DXMT core than to exposed D3D11 interfaces.

Use this only if the team wants a quick proof that RenderWare fixed-function can reach a DXMT-presented frame.

### Option D: D3D8/D3D9 Direct to Metal

Not recommended as a separate renderer. It would duplicate the hardest solved DXMT pieces: window integration, swapchain/presenter, format handling, resource allocation, command submission, shader cache, and Metal quirks.

If direct-to-Metal work is needed, it should happen through DXMT core APIs, not a parallel backend.

## What Current DXMT Can Reuse

Reusable with little conceptual change:

- Metal device/backend plumbing: `engine/dxmt/src/winemetal`.
- DXGI adapter/factory/output parts where D3D9 can share adapter enumeration and display mode data.
- Presenter and CAMetalLayer/window path: `dxmt_presenter`, swapchain backing, `CreateMetalViewFromHWND`.
- Command queue/context and encoder infrastructure: `dxmt_context`, `dxmt_command_queue`, `dxmt_command`.
- Resource primitives: `dxmt_buffer`, `dxmt_texture`, allocation, staging, residency, dynamic upload.
- Format mapping: `dxmt_format`.
- Sampler and binding models.
- Shader cache and Metal cache plumbing.
- `airconv` and metallib writer after D3D9 fixed-function or old shader models are lowered to a supported intermediate.
- Existing D3D11 validation ideas: clear, triangle, texture, indexed, viewport/scissor, swapchain present.

Not reusable as-is:

- D3D9 caps and adapter mode reporting.
- D3D9 device lost/reset contract.
- D3D9 resource pools and managed-resource behavior.
- D3D8/9 state blocks.
- FVF and D3D9 vertex declaration conversion.
- Fixed-function T&L, lighting, material, fog, alpha test, and texture-stage combiners.
- D3D8/9 shader model 1.x/2.x/3.0 token parsing/lowering.
- D3D9 query semantics and occlusion/event support.
- D3D9 cursor/gamma/fullscreen quirks.

## RenderWare / GTA Vice City First Slice

Vice City classic is a 32-bit D3D8 target. It should be treated as a PE32/WOW64 plus graphics integration milestone, not only a graphics milestone.

Likely first-frame API surface:

- `Direct3DCreate8`, adapter/caps/mode enumeration, `CreateDevice`.
- Windowed/fullscreen `D3DPRESENT_PARAMETERS`.
- Vertex/index buffer creation, lock/unlock, stream source, indices.
- FVF or D3D8 vertex shader declarations.
- Texture creation/lock/upload, mipmaps, common formats: `A8R8G8B8`, `X8R8G8B8`, `R5G6B5`, `A1R5G5B5`, depth `D16`/`D24S8` class formats.
- Render states: z enable/write/func, cull, blend, alpha test, fog, lighting, shade mode, texture factor, depth bias.
- Texture-stage states: color/alpha op and args, modulate/select/disable, addressing, filtering, mip filtering.
- Transforms: world/view/projection/texture matrices.
- Draw calls: `DrawPrimitive`, `DrawIndexedPrimitive`, likely primitive types triangle list/strip.
- Present/reset handling.

Minimum RenderWare fixed-function MVP:

1. D3D8 wrapper over D3D9 object model.
2. D3D9 device with state tracker and state blocks stubbed enough for common save/restore.
3. FVF to internal input layout.
4. Fixed-function shader generator for unlit/lit textured geometry, alpha test, fog, and 1-2 texture stages.
5. Resource pool and lock/upload path for static and dynamic VB/IB/textures.
6. Swapchain/presenter through DXMT.
7. Pixel validation against Windows/CrossOver baseline once Lane A frees GPU time.

## Work Estimate

Small prototype, not product quality:

- D3D8 wrapper skeleton plus D3D9 proxy objects: 1-2 weeks if copied structurally from DXVK upstream.
- D3D9-to-D3D11/DXMT-core triangle with no fixed function: 2-3 weeks after build/COM plumbing.
- RenderWare fixed-function first pixel: 6-10 weeks after D3D11 DXMT present path is reliable and PE32/WOW64 can launch the game.

Usable old-game lane:

- D3D9 frontend state/resource/reset/caps coverage: 3-5 months.
- Shader model 1.x/2.x/3.0 lowering plus fixed-function completeness: 2-4 months overlapping if a second graphics engineer owns shader translation.
- Robust D3D8/D3D9 game compatibility comparable to DXVK: multi-quarter effort.

The largest cost is not Metal. It is D3D9 semantic compatibility.

## Recommended Plan

Phase 0 - keep router unchanged:

- D3D8/9 remain DXVK/WineD3D routed while DXMT D3D11 and PE32 stabilize.
- Add this note as the planning baseline; do not run GPU smoke during HK contention.

Phase 1 - D3D9 frontend design extraction:

- Read `engine/dxvk-upstream/src/d3d9` and classify reusable concepts: state model, caps, resource pools, fixed-function, shader translator, swapchain/reset.
- Define MacRunner-owned D3D9 frontend boundaries over DXMT core.
- Decide whether the first prototype emits D3D11 calls or calls DXMT core directly.

Phase 2 - D3D8 wrapper:

- Port the D3D8-on-D3D9 object model from DXVK upstream conceptually.
- Keep D3D8 renderer-free.
- Ensure 32-bit COM ABI and DLL exports are correct before game work.

Phase 3 - RenderWare slice:

- Implement the minimum D3D8/D3D9 fixed-function subset listed above.
- Validate with synthetic no-smoke fixtures first, then Vice City when GPU contention clears.

Phase 4 - broaden D3D9:

- Add shader model 1-3 lowering, state block completeness, managed pool correctness, queries, additional formats, gamma/cursor/fullscreen behavior, and regression corpus.

## Bottom Line

D3D8/D3D9 to Metal through DXMT is feasible, but it is a D3D9 frontend project, not a small DXMT backend switch. The right reuse point is DXMT core and winemetal, with D3D8 layered over D3D9. D3D8/D3D9 to D3D11 can accelerate a prototype, but should not become the long-term semantic boundary unless the team accepts state translation overhead and correctness risk.

## Lane D continuation checkpoint (2026-06-06)

- External blocker remains external to Lane D (HyperBridge/SEH path), so graphics work proceeds in owned scope.
- No local D3D11/DXGI gap exists to close today; Phase 7 planning for D3D8/9 remains a backlog design task.
- Continue with evidence-backed owned smoke/coverage refreshes when runtime contention clears and return only to real-game gates after external unblock.

## 2026-06-06 14:18:25 — backlog-run note

- Continuing Phase 7 design prep in-place until external runtime blocker is removed by Lane A/C.
- Keep DXMT and vkd3d smoke ownership unchanged.
- Defer all native D3D8/9 implementation to a later lane slice once x64 reach gate is open.

## 2026-06-13 01:58 — Lane D headless/game-torture checkpoint

- D3D9 DXMT/mock headless smoke: PASS, 31 D3D8/D3D9 runtime traces, zero unsupported calls.
- D3D9 Metal request-contract smoke: PASS, 31 requests generated without GPU execution.
- D3D9 game-torture PE fixtures added for arm64/x64/x86 (`d3d9.dll` shim + `d3d9_triangle_*.exe`).
- D3D9 game-torture x64/x86: PASS with mock replay artifacts.
- D3D9 game-torture category: exits cleanly with supported `2/2` PASS and blocked `1` infrastructure item.
- D3D9 game-torture arm64: external `ENGINE_MISSING` because `engine/wine/dist-pure-arm64/bin/wine` is absent; recorded in `reports/research/LANE-D-NEEDS.md`.

## 2026-06-13 06:55 — Rosetta validation blocker reclassified

- Current PE app-runs on Rosetta lanes are blocked before graphics: `hello_x64.exe`, `hello_x86.exe`, D3D9, and D3D11 fixtures all return `rc=247` with empty logs.
- Direct evidence: `engine/wine-x86_64/lib/wine/x86_64-unix/wine --version` returns SIGKILL/137 under Rosetta, while `engine/wine-x86_64/bin/wine --version` still prints `wine-11.0`.
- `tools/game_torture_runner.py` now reports this as `ROSETTA_WINE_SIGKILL` infrastructure blocker. D3D9 and D3D11 categories exit cleanly with `failed=0`, `runtime_supported_total=0` while the engine blocker is active.
- Headless D3D9 DXMT/mock and Metal request-contract smokes remain the Lane D validation source until Rosetta Wine and arm64 `dist-pure-arm64` are restored by their owners.

## 2026-06-13 07:10 — D3D9 fixed-function fog headless coverage

- Added `d3d9_fixed_function_fog_runtime.jsonl`: green FFP triangle at depth 0.5 with red linear fog, expected center pixel `(128,128,0)`.
- Translator now records `D3DRS_FOG*` into `d3d9_fog_state` and the FFP shader metadata.
- Mock backend applies linear/exp/exp2 fog RGB before output-merger blending; alpha is preserved.
- Metal request payload now includes `d3d9.fog_state` for native-helper contract checks.
- Validation:
  - `python3 -m pytest engine/graphics/tests/test_d3d9_translation.py engine/graphics/tests/test_d3d9_metal_request.py -q` -> `61 passed`.
  - `bash engine/graphics/scripts/run_d3d9_dxmt_headless_smoke.sh` -> `d3d9_trace_count=32`, PASS.
  - `D3D9_METAL_REQUEST_ONLY=1 bash engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh` -> `d3d9_metal_request_count=32`, PASS.

## 2026-06-13 08:10 — Unity#2 corpus and D3D8 RenderWare fog increment

- AI War 2 GOG/Inno package at `/Users/timurtoby/Documents/MacRunner/Main/game-ai.war.2-(91319)` was unpacked headlessly with `innoextract` into `artifacts/ai-war2-unity-corpus/extracted`.
- Added `run_unity_dxbc_airconv_corpus_smoke.sh`: generic Unity asset DXBC scanner with `UNITY_DXBC_DATA_DIR`, `UNITY_DXBC_CORPUS_NAME`, and `UNITY_DXBC_CORPUS_LIMIT` knobs; outputs per-corpus artifacts under `artifacts/unity-dxbc-airconv-corpus/<name>/`.
- AI War 2 result: `AIWar2_Data` yielded 2 valid unique DXBC blobs; `airconv -S` and `airconv -A` both passed for all blobs (`translate=2/2`, `render=2/2`), run dir `artifacts/unity-dxbc-airconv-corpus/ai-war2/run-20260613-080658`.
- Added `d3d8_renderware_fog_runtime.jsonl`: D3D8 RenderWare-style fixed-function triangle with red linear fog at z=0.5, expected center pixel `(128,128,0)`.
- Validation:
  - `python3 -m pytest engine/graphics/tests/test_d3d9_translation.py engine/graphics/tests/test_d3d9_metal_request.py -q` -> `63 passed`.
  - `bash engine/graphics/scripts/run_d3d9_dxmt_headless_smoke.sh` -> `d3d9_trace_count=33`, PASS.
  - `D3D9_METAL_REQUEST_ONLY=1 bash engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh` -> `d3d9_metal_request_count=33`, PASS.
