# ABZU Windowscodecs regsvr32 verify — immutable pre-run manifest

Classification: `DIAGNOSTIC_ONLY / NOT_GOLDEN`. Exactly one 300-second title attempt; no retry and no A/A or formal A/B claim. Absolute observations only.

## Treatment

- Root cause already sealed: fresh prefix + no template + skipped wineboot leaves `CLSID_WICImagingFactory2` unregistered; UE4 receives `REGDB_E_CLASSNOTREG`, then dereferences NULL at RVA `0x5bd471`.
- New treatment: `MACRUNNER_MR_RUN_REGSVR32_WINCODECS=1` invokes bounded `/s C:\windows\system32\windowscodecs.dll` after prefix synchronization and before title launch.
- The stage validates the System32 payload, chooses `regsvr32.exe` from the same `${MACRUNNER_PREFIX_SYSTEM32_ARCH}` view, and fails closed on missing payload, timeout, or nonzero registration rc.
- Selected System32 view is x86-64. AMD64 `windowscodecs.dll`: `d7c75f1d1e2de0ecaf8ebc7a5722662e772bc4a55f1f8b9debc6339eb9bdbb6a`; ARM64 sibling (not selected): `5ec85b7a47a918f4f3d5bf831be19c1dbbea4014cc4b2ca9fd69d72d0a6f1428`.
- `wineboot`, JIT, guest fault, Unixlib, CS and graphics semantics are unchanged.

## Source and runner identity

- Main HEAD: `4be5ec135492d622b13acc7a22a53738a0776024`; ABZU HEAD: `2b62f6b7ac71f1c64c00ce39e0ff6fb998dc01c8`. Dirty scopes are captured in `BRANCH-STATUS.txt`.
- Canonical Main runner: `039f945550e11c1209a09bc36f61b936777af9698cd41721627eb839f20863d7`.
- ABZU verification runner: `36a0bdb188a40d284ee0075eb871c309b41065a88706926f9e000db77fbb8e07`.
- Two-runner source guard: `a07a30e92d005346317b2dea6c2822c685462c3711e6a1597243ab2eef997dbf`; both runners pass `bash -n` and the guard.
- Immutable one-shot wrapper: `cfc6194b77852aa82e292d8f94097a0e851154e9eb6b10b7c313d3b324887c3d`.

## Runtime/build identity

- No rebuild in this task. Coherent treatment pair retained from the JIT observer build: Unix `4b16eda1e30b0d1678524b5414a4a098a5a8b85cb2abbe6553b9112ed3d8f272`; ARM64X PE `38841810948dd4e46abafa0bd103099bb3576ad95408e3645a9a7907f1828ea3`.
- HB source: `9e1b9ec42bb27271f7cba1e647388a650a7453c7e981a42a25699ddb211897af`; retained build log `717e9c18…` had errors 0 and `install skipped` 0.
- Runtime dist: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist`.
- Pre/post-restored overlay pair: Unix `14d3563d…`, PE `8e2a5691…`. Wrapper deploys and verifies both treatment halves and restores both under EXIT trap.
- Game SHA `b9bdbc74…`; DXMT d3d11/dxgi/winemetal `842fa9e7…` / `30eb89bf…` / `e49765a9…`; runtime winemetal overlay `7aa914d6…`; manifest `06e8f907…`.

## Environment and gates

- Existing stack remains: builtin Unixlib resolve, CS-forward, JIT fault observer, winemetal lifecycle/fallback, ARM64 syscall bridge, native syscall/direct-callback splits, benign EH noop, DXMT, services/RpcSs, actxprxy registration, `WINEMSYNC=1`, JIT direct memory, native memmove, single lookup, 524288-entry IR cache.
- Candidate A and disproved wait/event observers remain unset. `MACRUNNER_TRACE_UI_INPUT` remains unset.
- Export delta from the immediately preceding sealed probe is exactly `MACRUNNER_MR_RUN_REGSVR32_WINCODECS=1`; title timeout is task-required 300 seconds. Complete child environment is captured byte-exact before launch.
- Gates: registration stage rc 0; no WIC Factory2 NULL fault/`0x5bd471`; real CDF1/EnumAdapters1/D3D11CreateDevice; swapchain/GetBuffer/RTV/Present1; zero `c0000005`, `c000007b`, and `pc=0x60`.

## Fail-closed and cleanup

Before consuming the attempt, the wrapper rejects previous attempt, process/build/title collision, source/binary/runner/guard/hash drift, and incoherent deploy. It records host/locale/timezone, process state, free space, and byte-exact cache inventory. Afterward it records exit/process state, restores both runtime halves, repeats cache inventory and free-space capture. No cache deletion, `make clean`, global Wine kill, or retry is permitted. The pre-rebuild breakthrough floor remains separate and must continue to verify 6/6.
