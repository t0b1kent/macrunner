# ChatGPT (web search) — ARM64EC ntdll research prompt (PASTE-READY, standalone)

> Вставь весь блок ниже целиком в ChatGPT с включённым web search. Он самодостаточный —
> у ChatGPT нет доступа к нашему репо, поэтому все факты о нашей системе изложены прямо здесь.

---

You are a systems-engineering researcher. I'm building a translation layer that runs
**x86_64 Windows applications and games on Apple Silicon (ARM64) macOS WITHOUT Rosetta**,
using a custom x86_64→ARM64 binary translator ("HyperBridge") plus a pure-ARM64-native
fork of Wine 11. Graphics go DirectX 11/12 → Metal via a DXMT-style translation layer
with a native ARM64 Metal backend (a `.so` reached through Wine's `WINE_UNIX_CALL` boundary).

## The concrete blocker
When an x86_64 Windows PE runs on our ARM64 Wine host, Wine's ntdll loader hard-requires
two exports from `x86_64-windows/ntdll.dll`:

```
err:module:load_ntdll_functions __wine_unix_call_dispatcher_arm64ec not found
err:module:load_ntdll_functions KiUserEmulationDispatcher not found
err:virtual:virtual_setup_exception stack overflow
```

Our `x86_64-windows/ntdll.dll` does NOT export `__wine_unix_call_dispatcher_arm64ec` or
`KiUserEmulationDispatcher`. So x86_64 PEs fail at load → our DirectX→Metal layer never
initializes → no game runs. Our Wine was evidently built **without ARM64EC support**.

The target architecture we want working:
`x64 Windows.exe → ARM64 Wine host → HyperBridge x64 emulation → x86_64-windows DLLs
→ WINE_UNIX_CALL → native ARM64 Metal backend → Apple GPU`.

## What I need you to research and report (with sources/links + versions + commit IDs)

### 1. ARM64EC in Wine
- What ARM64EC ABI is (Microsoft's emulation-compatible hybrid PE for x64-on-ARM64),
  briefly, with Microsoft docs links.
- When/where ARM64EC support landed in Wine upstream: version, year, relevant
  commits/merge-requests (search winehq gitlab / wine-devel mailing list / github mirror).
- Exactly which Wine build flags enable it: `--enable-archs=...`, configure options,
  what `x86_64-windows/ntdll.dll` must be (hybrid PE?) for it to export
  `__wine_unix_call_dispatcher_arm64ec` + `KiUserEmulationDispatcher`. Cite the Wine
  source files/functions where these exports are produced.

### 2. Toolchain requirements
- Which llvm-mingw / clang / lld versions can target ARM64EC
  (`arm64ec-w64-windows-gnu` or similar). Minimum versions.
- Can a recent llvm-mingw (e.g. the 2024-2025 ucrt releases) build ARM64EC PEs on macOS
  as the build host? Any known blockers building Wine-ARM64EC on macOS specifically.

### 3. Existing prebuilt sources
- Does WineHQ ship a ready ARM64EC build for macOS/Linux? Where.
- CrossOver / CodeWeavers engineering notes on ARM64EC (they ship Apple Silicon Wine —
  do they use ARM64EC, Rosetta, or something else?).
- Whisky / Game Porting Toolkit (GPTK) / Mythic: do they use ARM64EC, or wine64 + Rosetta 2
  as the x86_64 CPU emulator? What's each one's actual approach.

### 4. Alternative: Rosetta-as-CPU fallback
- Running x86_64 Wine under Apple Rosetta 2 (Rosetta as the x86 CPU, Wine x86_64 unmodified):
  what changes in the stack? Does a native-ARM64 Metal backend `.so` still fit, or must
  the whole graphics path also be x86_64?
- Tradeoffs vs a true ARM64EC build: performance, control, and the risk that Apple removes
  Rosetta 2 (rumored ~2027-2028).

### 5. Recommendation + stepwise plan
- Bottom line in 3-5 lines: what's needed, how realistic each path is, which you'd pick.
- A prioritized, stepwise implementation plan for BOTH: (a) adding ARM64EC to a Wine build,
  (b) a Rosetta-fallback. With expected outcomes and risks per step.
- An "Open questions" section for anything you could not establish — do NOT guess, mark
  it unknown.

Please use web search aggressively and cite every non-obvious claim with a link, version
number, or commit hash.
