# AI War 2 Compatibility Research Report

**Date:** 2026-06-06  
**Path:** [GEMINI-aiwar2-compat.md](file:///Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/GEMINI-aiwar2-compat.md)  
**Target Directory:** `/Users/timurtoby/Downloads/game-ai.war.2-(91319)`  

---

## 1. Executive Summary

AI War 2 is a modern x86_64 strategy game developed by Arcen Games. The target files represent a GOG (Good Old Games) release. The following key facts have been established:
- **Installer Type:** Inno Setup (GOG installer)
- **DRM Status:** DRM-free
- **Architecture:** x86_64 (64-bit only)
- **Engine:** Unity 3D
- **Unity Engine Version:** `2021.3.45f2`
- **DirectX Version:** Direct3D 11 (default) / Direct3D 12 (optional/fallback)

---

## 2. Installer Architecture & Structure

The installer directory contains the base game and a `dlc/` directory with three DLC installers:

### Base Game
- **Main Executable:** `setup_ai_war_2_5.812_(64bit)_(91319).exe` (2.6 MB)
- **Archive Binary Data:** `setup_ai_war_2_5.812_(64bit)_(91319)-1.bin` (1.88 GB)
- *Note:* The split of a small `.exe` and a large `.bin` file is the signature layout of Inno Setup installers packaged by GOG.

### DLCs (located under `dlc/`)
- `setup_ai_war_2_the_neinzul_abyss_5.812_(91319).exe` (292.6 MB)
- `setup_ai_war_2_the_spire_rises_5.812_(91319).exe` (245 MB)
- `setup_ai_war_2_zenith_onslaught_5.812_(91319).exe` (394.3 MB)
- *Note:* These are self-contained Inno Setup installers that require the base game to be installed first.

---

## 3. DRM Status

As a **GOG Release**, the installer and the resulting files are completely **DRM-free**. 
- There are no Steam client wrapper dependencies (though steamworks DLLs exist for optional multiplayer integration, e.g., `Facepunch.Steamworks.dll`).
- No online authentication, custom launcher-level DRM, or kernel-level anti-cheat (such as Easy Anti-Cheat or BattlEye) is present.
- The game can be safely launched directly via `AIWar2.exe` without any background store client.

---

## 4. Engine & Graphics API Analysis

By listing the contents of the installer, we successfully identified the engine and graphics specifications:

### Unity Engine Specs
- **Game Engine:** Unity 3D (64-bit).
- **Unity Version:** `2021.3.45f2`.
  - *Verification:* Extracted directly from `globalgamemanagers` (first 15 bytes contain the version string `2021.3.45f2`).
  - *Managed Runtime:* Mono scripting backend is used (evidenced by the presence of `AIWar2_Data/Managed/mscorlib.dll`, `UnityEngine.CoreModule.dll`, etc.).
- **Vulkan / Native Plugins:** Contains plugins for Alembic, USD (Pixar Universal Scene Description), and TBB.

### DirectX and Graphics Pipeline
- **Default Graphics API:** Direct3D 11 (D3D11).
  - Unity 2021.3 games running under Windows default to D3D11.
  - The runtime uses `d3d11.dll` and `dxgi.dll` for presenting frames.
- **Direct3D 12 (D3D12) Support:**
  - Supported natively by the Unity engine version `2021.3`.
  - Can be forced by appending the command-line argument `-force-d3d12` to the game launch.
- **Vulkan Support:**
  - Supported natively by the Unity engine version `2021.3` on Windows.
  - Can be forced by appending the command-line argument `-force-vulkan` to the game launch.

---

## 5. Compatibility Recommendations for MacRunner

1. **Loader Lane:** Must be run under `x86_64-hyperbridge` (or `x86-rosetta-wow64` fallback).
2. **Graphics Lane:** Requires D3D11 rendering backend. On macOS, this should be routed to **DXMT** (our Direct3D 11 to Metal translator) or **DXVK-MoltenVK** fallback.
3. **Execution Safety:** Because the game has zero DRM and runs under Unity 2021.3, it should behave similarly to Hollow Knight once the `GFXDEVICE` initialization gate is resolved on the JIT/SEH side.
