# ABZU WIC native-regsvr32 verify — immutable pre-run manifest

Status: `NOT_GOLDEN`, one authorized attempt, timeout 300 seconds.

## Independent variable

The failed 2026-07-13 WIC attempt used guest x86-64 `regsvr32.exe` and exited `INVALID_ARG=1` before DLL load. This treatment changes only the WIC registration launcher to `$DIST/lib/wine/aarch64-windows/regsvr32.exe`; the registered payload remains the x86-64 `C:\windows\system32\windowscodecs.dll`. All runtime/JIT/graphics/cache inputs and diagnostics are retained from the preceding sealed attempt.

## Identity

- Main HEAD: `4be5ec135492d622b13acc7a22a53738a0776024`; ABZU HEAD: `2b62f6b7ac71f1c64c00ce39e0ff6fb998dc01c8`.
- Wrapper: `da1f64d525debacf00c540b5a2a1c6a695054f1f405fcaf360a9f3299a6ae0bd`.
- Main runner: `fbd679b8d3780f251859f6fc221a960397ba3356a4209931ce4346da6a6c2421`; ABZU runner: `99c390ba10498177833fdfaa1f0fb8ef4e990dc7f3d264fc364703f27535b7d0`; WIC guard: `788df964e77705728a5e9148743bcd1c22da5ba80810175d14987028584a6030`.
- Coherent treatment pair: Unix `4b16eda1e30b0d1678524b5414a4a098a5a8b85cb2abbe6553b9112ed3d8f272`; PE `38841810948dd4e46abafa0bd103099bb3576ad95408e3645a9a7907f1828ea3`.
- Pre-run overlay pair to restore: Unix `14d3563d105defde6efae10563e58b3ac7e88278b2533b96e882c8d6efacf2b5`; PE `8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc`.
- Game: `b9bdbc743742f45845b152df7663b042e2b5da5d318894584d70f6d63d6839e7`.
- DXMT d3d11/dxgi/winemetal: `842fa9e7…` / `30eb89bf…` / `e49765a9…`; manifest `06e8f907…`; overlay winemetal `7aa914d6…`.

## Environment and gates

The child environment is captured byte-exact as `CHILD-ENV.bin`. It retains builtin Unixlib resolve, CS forward, JIT fault trace, winemetal lifecycle/fallback, DXMT, service/RpcSs + actxprxy setup, ARM64/native syscall splits, direct callback split, benign EH noop, `WINEMSYNC=1`, 524288-entry IR cache, JIT direct memory, native memmove and single lookup. `MACRUNNER_MR_RUN_REGSVR32_WINCODECS=1`; general builtin-DllMain candidate and disproved wait/event observers remain off.

Gates: successful WIC registration and Factory2 activation; no RVA `0x5bd471` NULL read; real CDF1/EnumAdapters1/D3D11CreateDevice and swapchain/GetBuffer/RTV/Present1; zero `c0000005`, `c000007b`, and `pc=0x60`.

## Fail-closed and cleanup

The wrapper rejects an existing attempt, serialization collision, HEAD/source/runner/binary/graphics/game drift, or incoherent deployment. It records host/process/cache/disk state, restores both runtime halves on every exit, and forbids retry. No cache deletion, `make clean`, global Wine kill, or UI-input trace. Breakthrough floor verified 6/6 immediately before sealing; 57 GiB free.
