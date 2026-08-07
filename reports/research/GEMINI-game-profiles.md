# Tier-1 Game Compatibility Profiles

**Date:** 2026-06-06  
**Path:** [GEMINI-game-profiles.md](file:///Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/GEMINI-game-profiles.md)  

---

> [!NOTE]
> This document details the technical footprint of the Tier-1 top-10 prioritized game targets. 
> The static import tables have been cross-checked by analyzing the binaries in the workspace via custom header parsing scripts.
> Detailed configurations are sourced from [PCGamingWiki](https://www.pcgamingwiki.com) profiles.

---

## 1. Hollow Knight (GOG / Steam)
* **Technical Details:** [PCGamingWiki Hollow Knight Profile](https://www.pcgamingwiki.com/wiki/Hollow_Knight)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** Unity 2020.3.x
* **Scripting Backend:** Mono (MonoBleedingEdge)
* **DirectX / Graphics API:** D3D11 (Shader Model 5.0)
* **Static Imports:**
  - `KERNEL32.dll`
  - `UnityPlayer.dll` (which resolves dependencies internally)
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`
  - `dxgi.dll`
  - `Galaxy64.dll` (GOG SDK wrapper)
  - `steam_api64.dll` (Steam SDK wrapper)
* **Audio Stack:** FMOD Studio
* **Input API:** Unity Input Manager (uses XInput / RawInput)
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\LocalLow\Team Cherry\Hollow Knight\`
  - Registry: `HKCU\Software\Team Cherry\Hollow Knight`
* **Launcher / DRM Wrapper:** DRM-free on GOG; simple Steam wrapper on Steam.
* **Redistributables Needed:** VC++ Runtime (MSVCP140 / VCRUNTIME140).

---

## 2. AI War 2 (GOG)
* **Technical Details:** [PCGamingWiki AI War 2 Profile](https://www.pcgamingwiki.com/wiki/AI_War_2)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** Unity 2021.3.45f2
* **Scripting Backend:** Mono (MonoBleedingEdge)
* **DirectX / Graphics API:** D3D11 (Shader Model 5.0) / D3D12 (optional)
* **Static Imports:**
  - `KERNEL32.dll`
  - `UnityPlayer.dll`
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`
  - `dxgi.dll`
  - `Galaxy64.dll`
* **Audio Stack:** Unity Audio Stack (FMOD backend)
* **Input API:** Unity Input Manager (uses XInput / RawInput)
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\LocalLow\Arcen Games\AI War 2\`
  - Registry: `HKCU\Software\Arcen Games\AI War 2`
* **Launcher / DRM Wrapper:** DRM-free.
* **Redistributables Needed:** VC++ Runtime (MSVCP140 / VCRUNTIME140).

---

## 3. Ori and the Blind Forest (GOG / Steam)
* **Technical Details:** [PCGamingWiki Ori Profile](https://www.pcgamingwiki.com/wiki/Ori_and_the_Blind_Forest)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** Unity 2017.2.x
* **Scripting Backend:** Mono
* **DirectX / Graphics API:** D3D11 (Shader Model 5.0)
* **Static Imports:**
  - `KERNEL32.dll`
  - `UnityPlayer.dll`
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`
  - `dxgi.dll`
  - `Galaxy64.dll` / `steam_api64.dll`
* **Audio Stack:** Wwise (Audiokinetic)
* **Input API:** Unity Input (uses XInput / RawInput)
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\Local\Ori and the Blind Forest\`
  - Registry: `HKCU\Software\Microsoft\Ori and the Blind Forest`
* **Launcher / DRM Wrapper:** DRM-free on GOG.
* **Redistributables Needed:** VC++ Runtime.

---

## 4. Cuphead (GOG / Steam)
* **Technical Details:** [PCGamingWiki Cuphead Profile](https://www.pcgamingwiki.com/wiki/Cuphead)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** Unity 2018.4.x
* **Scripting Backend:** Mono
* **DirectX / Graphics API:** D3D11 (Shader Model 5.0)
* **Static Imports:**
  - `KERNEL32.dll`
  - `UnityPlayer.dll`
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`
  - `dxgi.dll`
  - `Galaxy64.dll` / `steam_api64.dll`
* **Audio Stack:** FMOD Studio
* **Input API:** Unity Input (uses XInput)
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\Roaming\Cuphead\`
  - Registry: `HKCU\Software\StudioMDHR\Cuphead`
* **Launcher / DRM Wrapper:** DRM-free on GOG.
* **Redistributables Needed:** VC++ Runtime.

---

## 5. Outer Wilds (GOG / Steam)
* **Technical Details:** [PCGamingWiki Outer Wilds Profile](https://www.pcgamingwiki.com/wiki/Outer_Wilds)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** Unity 2019.4.x
* **Scripting Backend:** Mono
* **DirectX / Graphics API:** D3D11 (Shader Model 5.0)
* **Static Imports:**
  - `KERNEL32.dll`
  - `UnityPlayer.dll`
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`
  - `dxgi.dll`
  - `Galaxy64.dll` / `steam_api64.dll`
* **Audio Stack:** Wwise (Audiokinetic)
* **Input API:** Unity Input System (XInput / RawInput)
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\LocalLow\Mobius Digital\Outer Wilds\`
  - Registry: `HKCU\Software\Mobius Digital\Outer Wilds`
* **Launcher / DRM Wrapper:** DRM-free on GOG.
* **Redistributables Needed:** VC++ Runtime.

---

## 6. Firewatch (GOG / Steam)
* **Technical Details:** [PCGamingWiki Firewatch Profile](https://www.pcgamingwiki.com/wiki/Firewatch)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** Unity 5.3.x
* **Scripting Backend:** Mono
* **DirectX / Graphics API:** D3D11 (Shader Model 5.0)
* **Static Imports:**
  - `KERNEL32.dll`
  - `UnityPlayer.dll`
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`
  - `dxgi.dll`
  - `Galaxy64.dll` / `steam_api64.dll`
* **Audio Stack:** Unity Audio Stack (FMOD backend)
* **Input API:** Unity Input (uses XInput)
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\LocalLow\Camp Santo\Firewatch\`
  - Registry: `HKCU\Software\Camp Santo\Firewatch`
* **Launcher / DRM Wrapper:** DRM-free on GOG.
* **Redistributables Needed:** VC++ Runtime.

---

## 7. Hellblade: Senua's Sacrifice (GOG / Steam)
* **Technical Details:** [PCGamingWiki Hellblade Profile](https://www.pcgamingwiki.com/wiki/Hellblade:_Senua%27s_Sacrifice)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** Unreal Engine 4.22
* **Scripting Backend:** None (Native C++ compiled)
* **DirectX / Graphics API:** D3D11 / D3D12
* **Static Imports:**
  - `KERNEL32.dll`, `USER32.dll`, `SHELL32.dll`, `ADVAPI32.dll`, `WS2_32.dll`, `OLE32.dll`
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`, `dxgi.dll`, `d3d12.dll`
* **Audio Stack:** Wwise (binaural spatial audio)
* **Input API:** UE4 Input Manager (RawInput / XInput)
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\Local\HellbladeGame\`
  - Registry: `HKCU\Software\Ninja Theory\Hellblade`
* **Launcher / DRM Wrapper:** DRM-free on GOG.
* **Redistributables Needed:** VC++ 2015-2019 Runtime.

---

## 8. Stray (Steam)
* **Technical Details:** [PCGamingWiki Stray Profile](https://www.pcgamingwiki.com/wiki/Stray)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** Unreal Engine 4.27
* **Scripting Backend:** None (Native C++ compiled)
* **DirectX / Graphics API:** D3D11 / D3D12
* **Static Imports:**
  - `KERNEL32.dll`, `USER32.dll`, `SHELL32.dll`, `ADVAPI32.dll`, `WS2_32.dll`
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`, `dxgi.dll`, `d3d12.dll`, `steam_api64.dll`
* **Audio Stack:** Wwise / Unreal Audio
* **Input API:** UE4 Input Manager (RawInput / XInput)
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\Local\Hk_project\`
  - Registry: `HKCU\Software\BlueTwelve\Stray`
* **Launcher / DRM Wrapper:** Simple Steam wrapper.
* **Redistributables Needed:** VC++ 2015-2022 Runtime.

---

## 9. Katana Zero (GOG / Steam)
* **Technical Details:** [PCGamingWiki Katana Zero Profile](https://www.pcgamingwiki.com/wiki/Katana_Zero)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** GameMaker Studio 2
* **Scripting Backend:** None (Native compiled runner)
* **DirectX / Graphics API:** D3D11
* **Static Imports:**
  - `KERNEL32.dll`, `USER32.dll`, `GDI32.dll`, `ADVAPI32.dll`, `SHELL32.dll`
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`, `dxgi.dll`, `openal32.dll`
* **Audio Stack:** OpenAL (GameMaker Audio)
* **Input API:** XInput / DInput
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\Local\Katana_Zero\`
  - Registry: `HKCU\Software\Askiisoft\Katana Zero`
* **Launcher / DRM Wrapper:** DRM-free on GOG.
* **Redistributables Needed:** VC++ Runtime.

---

## 10. Brotato (Steam)
* **Technical Details:** [PCGamingWiki Brotato Profile](https://www.pcgamingwiki.com/wiki/Brotato)
* **Architecture:** x64 (64-bit)
* **Engine + Version:** Godot 3.x
* **Scripting Backend:** GDScript (compiled to bytecode in custom C++ runner)
* **DirectX / Graphics API:** D3D11 / OpenGL
* **Static Imports:**
  - `KERNEL32.dll`, `USER32.dll`, `GDI32.dll`, `SHELL32.dll`, `ADVAPI32.dll`
* **Dynamic LoadLibrary DLLs:**
  - `d3d11.dll`, `dxgi.dll`, `opengl32.dll`, `steam_api64.dll`
* **Audio Stack:** Godot Audio (DirectSound/OpenAL wrapper)
* **Input API:** Godot Input System (RawInput)
* **Save/Registry Expectations:**
  - Save files: `%USERPROFILE%\AppData\Roaming\Brotato\`
  - Registry: `HKCU\Software\Blobfish\Brotato`
* **Launcher / DRM Wrapper:** Simple Steam wrapper.
* **Redistributables Needed:** VC++ Runtime.
