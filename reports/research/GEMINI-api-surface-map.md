# Win32 / NT API & DLL Surface Map (Lane C Breadth)

**Date:** 2026-06-06  
**Path:** [GEMINI-api-surface-map.md](file:///Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/GEMINI-api-surface-map.md)  

---

> [!NOTE]
> This map outlines the union of Win32 and NT system dependencies required by our Tier-1 game targets. 
> It cross-references these dependencies against MacRunner's Wine dll implementation (`engine/wine/dlls/`) and provides a proactive **Gap List** for Lane C.

---

## 1. Union of Required System DLLs

Based on the static PE analysis of Unity (e.g., `UnityPlayer.dll`), Unreal Engine 4, GameMaker Studio 2, and Godot, the target games require the following Windows system libraries:

| Core Subsystem | System DLLs |
|---|---|
| **OS / Kernel** | `KERNEL32.dll`, `ADVAPI32.dll`, `VERSION.dll`, `bcrypt.dll` |
| **Windowing / GDI** | `USER32.dll`, `GDI32.dll`, `dwmapi.dll`, `IMM32.dll` |
| **Graphics** | `d3d11.dll`, `dxgi.dll`, `opengl32.dll`, `d3d12.dll` (optional) |
| **COM / Object Model**| `ole32.dll`, `OLEAUT32.dll`, `combase.dll` |
| **Shell / Pathing** | `SHELL32.dll`, `SHLWAPI.dll`, `SETUPAPI.dll` |
| **Input / Hardware** | `HID.DLL`, `xinput1_4.dll` (or `xinput1_3.dll` fallback) |
| **Networking** | `WS2_32.dll`, `WINHTTP.dll`, `IPHLPAPI.DLL`, `CRYPT32.dll` |
| **Media / Sound** | `WINMM.dll`, `dsound.dll` |
| **WinRT Foundations** | `api-ms-win-core-winrt-l1-1-0.dll`, `api-ms-win-core-winrt-string-l1-1-0.dll` |

---

## 2. Repo-wide DLL Coverage Audit

We analyzed the `engine/wine/dlls/` directory inside the repository to verify which of the required libraries are supported as built-in DLLs vs which are missing or require external wrappers:

### A. Fully Implemented Built-ins (Present in `engine/wine/dlls/`)
- **OS / Kernel:** `kernel32`, `advapi32`, `version`, `bcrypt` (and `bcryptprimitives`).
- **Windowing / GDI:** `user32`, `gdi32` (with WOW64 GdiSharedHandleTable mirror), `imm32`, `dwmapi`.
- **Graphics:** `d3d11`, `dxgi`, `opengl32`, `d3d12`, `d3d12core` (via vkd3d).
- **COM:** `ole32`, `oleaut32`, `combase`.
- **Shell / Network:** `shell32`, `shlwapi`, `setupapi`, `ws2_32`, `winhttp`, `iphlpapi`, `crypt32`.
- **Media:** `winmm`, `dsound`.

### B. Missing or Redirected (Not in `engine/wine/dlls/` root)
- **WinRT DLLs:** `api-ms-win-core-winrt-l1-1-0`, `api-ms-win-core-winrt-string-l1-1-0`, `api-ms-win-core-synch-l1-2-0`.
- **Input Helpers:** `xinput1_4` (and other XInput versions are often combined under `xinput1_3` or stubbed).
- **External SDKs:** `steam_api64.dll`, `Galaxy64.dll`, `openal32.dll`. (These are game-packaged dependencies, not OS files; they load natively from the game's executable directory).

---

## 3. Gap List & Recommendations for Lane C

The following gaps represent potential loader or execution blocks that Lane C must proactively resolve:

### Gap 1: WinRT Core & String API Set Forwards (`c0000135` Risk)
* **API Sets:**
  - `api-ms-win-core-winrt-l1-1-0.dll`
  - `api-ms-win-core-winrt-string-l1-1-0.dll`
  - `api-ms-win-core-synch-l1-2-0.dll`
* **Symptoms:** Modern Unity engines (2020+) statically probe or dynamically load WinRT string helpers for telemetry or display integration. If the loader cannot find these DLLs or resolve their API sets, the game will fail with a `STATUS_DLL_NOT_FOUND (c0000135)` error before `DllMain` completes. See [Windows API Sets on Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/apiindex/windows-apisets).
* **Lane C Action:** 
  1. Ensure that the built-in Wine loader resolves these api-ms forwards to `combase.dll` or `kernelbase.dll` dynamically (refer to the [WineHQ apisetschema implementation](https://github.com/wine-mirror/wine/tree/master/dlls/apisetschema)).
  2. If the game does a direct `LoadLibrary` check, write simple forwarding specs in `engine/wine/dlls/apisetschema/apisetschema.spec`.

### Gap 2: Asynchronous Named Pipes for Decompressors (`GENERIC_WOW64_FAULT` Risk)
* **APIs:** `CreatePipe`, `PeekNamedPipe`, `ReadFile` (overlapped), `WriteFile` (overlapped).
* **Symptoms:** Installer decompressors (such as `unarc.dll` or `arc.exe` during GOG/FitGirl installations) rely on asynchronous named pipes to communicate unpack progress back to the setup GUI. If named pipe locking or asynchronous reads block, the installer hangs indefinitely.
* **Lane C Action:** Audit the named pipe implementation in `engine/wine/dlls/ntdll/unix/virtual.c` and make sure asynchronous file handles do not block on guest execution cycles.

### Gap 3: Registry Virtualization for Save & Config Paths
* **APIs:** `RegCreateKeyExW`, `RegOpenKeyExW`, `RegSetValueExW`.
* **Symptoms:** Unity and Unreal games store player settings in the Windows registry (`HKCU\Software\<Developer>\<Game>`). 
* **Lane C Action:** Verify that registry operations are backed up correctly in the user's bottle prefix (`user.reg`) and do not fail on basic security access permissions.

---

## 4. References & Sources
- **Microsoft API Set Documentation:** [Windows API Sets (Microsoft Learn)](https://learn.microsoft.com/en-us/windows/win32/apiindex/windows-apisets)
- **Winehq API Set Guidelines:** [Developer Guidelines for API Sets (WineHQ)](https://wiki.winehq.org/Developer_Guidelines)
- **Wine Repository apisetschema:** [apisetschema source code (GitHub)](https://github.com/wine-mirror/wine/tree/master/dlls/apisetschema)
- **Winehq Wiki:** [WineHQ Developer Wiki (WineHQ)](https://wiki.winehq.org/)

