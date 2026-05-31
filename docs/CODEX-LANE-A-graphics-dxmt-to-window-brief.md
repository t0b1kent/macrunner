# CODEX Lane A (main mac) — GRAPHICS / DXMT path to the Hollow Knight window

**Date:** 2026-06-01
**Why now:** JIT perf is good (fallback-zero, hot blocks promoted to native). But the latest Hollow
Knight runs show **NO `GfxDevice` / `d3d11` / `dxgi` / `Direct3D` markers at all** — graphics never
starts. So the missing window is NOT only a throughput problem: the game is not reaching/clearing
the GRAPHICS bring-up. Pure JIT micro-optimization has hit diminishing returns for the window goal.
Shift focus to the **DX11 → Metal (DXMT)** path. You have the full stack + the game to test on.

## FIRST: diagnose WHY graphics doesn't start (evidence before code)
Run Hollow Knight with graphics/module tracing and find the exact point Unity's renderer bring-up
stops. Questions to answer with pasted log lines:
1. Does Unity even call into `d3d11.dll` / `dxgi.dll` (CreateDevice / CreateDXGIFactory /
   CreateSwapChain)? Or does it die BEFORE, still in CPU/Mono init (then it IS still throughput)?
2. If it reaches d3d11/dxgi — what's the first failure (missing export, unimplemented method,
   winemetal bridge, Metal device creation)?
3. Is `winemetal`/DXMT loaded and initialized for the x64 game across the EC boundary?
Write findings to `reports/research/HB-GRAPHICS-bringup-diagnosis-20260601.md` before patching.

## THEN: drive DX11→Metal far enough for a window + first frame
Scope = the graphics lane (now yours; Kimi sidelined):
- `engine/dxmt/**` (winemetal/nativemetal/airconv shader→AIR/metallib), `engine/graphics/**`,
  `engine/vkd3d/**`.
- The x64-side path: game x64 → native ARM64 `d3d11.dll`/`dxgi.dll` → DXMT → Metal, across the
  ARM64EC boundary. Get: device create → swapchain → a native macOS window on-screen → present →
  first frame. Use the existing DXMT test fixtures (`engine/dxmt/tests/dx11/*.cpp`) to validate
  pieces without the full game where possible.
- Known prior issue (from Kimi's notes `reports/research/KIMI-GRAPHICS-CORE-STATUS-20260528.md`):
  off-screen window placement made captures look blank — force the window on-screen and verify
  frame CONTENT, not a blank grab.

## GATE
Hollow Knight reaches a rendered frame / main menu on-screen (screenshot with real content), OR a
clear diagnosis that the block is elsewhere with the exact failing call pasted. Evidence, not status.

## SCOPE / BOUNDARIES
- Graphics + engine are yours. **Decoder/lifter (`hb_decode_x64.c`, `hb_lift_x64.c`) are Lane B's
  (the Air) — do NOT edit them** (merge collision). JIT codegen stays yours.
- The JIT perf work is NOT wasted — it's the permanent speed foundation for all games; you can keep
  promoting genuinely-hot blocks opportunistically, but the PRIMARY goal now is the graphics/window.
- Run hygiene: `scripts/mr-run.sh`, `scripts/mr-clean.sh --prune`. Don't leave orphan Wine.
- No commits gating needed — you've been self-committing; keep doing that.
