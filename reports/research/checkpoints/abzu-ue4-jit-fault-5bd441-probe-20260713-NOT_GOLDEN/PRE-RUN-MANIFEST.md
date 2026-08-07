# ABZU UE4 JIT fault `0x1405bd441` observer — immutable pre-run manifest

Classification: `DIAGNOSTIC_ONLY / NOT_GOLDEN`. This is one absolute-fact probe, not an A/B regression claim; no A/A pair is claimed. Attempt budget is exactly one 120-second title run.

## Evidence boundary

- Sealed predecessor: `abzu-builtin-unixlib-resolve-verify-20260713-NOT_GOLDEN`.
- Guest block: `AbzuGame-Win64-Shipping.exe` RVA `0x5bd441`, function range `0x5bd3e0..0x5bd702`.
- Source disassembly: `CoCreateInstance({317d06e8-5f24-433d-bdf7-79ce68d8abc2}, IID_IWICImagingFactory)` returns `0x80040154` and leaves `[rbp-0x19]` NULL; block later executes `mov rax,[rcx]` at RVA `0x5bd471`.
- Question: does the JIT memory helper fail on that exact NULL read, or is the generic `JIT helper fault` caused by another operation/address?
- No execution semantics are changed. The observer only records the first failing special-read/write helper per JIT block and returns its original result.

## Repository and source identity

- Main HEAD: `4be5ec135492d622b13acc7a22a53738a0776024` (dirty scope captured separately in `BRANCH-STATUS.txt`).
- ABZU HEAD: `2b62f6b7ac71f1c64c00ce39e0ff6fb998dc01c8`.
- `unix/macrunner_hb.c`: `9e1b9ec42bb27271f7cba1e647388a650a7453c7e981a42a25699ddb211897af`.
- `unix/sync.c`: `2d17e9a6bb78be743ef5a0906fdaa1a744a482b1cabb3d12e0c7f643039581ab`.
- `loader.c`: `f68c801b25eacd29f3dbcb0965816360325ed1983d01ffbf01b7c38b131be8b5`.
- JIT observer guard: `50bcfccf05fc715f6de5d534fbcd55d24212e6be98ca0b2ee662773be2f8b6bc`.
- Unixlib/CS-forward/dynamic-IAT guards: `a60b3cdf…` / `67fb683f…` / `9c3c9ab4…`; all source guards PASS before build.
- Build log: `717e9c18d51c46f63b6ebe8b2b3cea17fe05111a3c35cf68b1202ec75c1122fa`; build RC 0, errors 0, `install skipped` 0, warnings 13.
- Wrapper: `b5e0272ff02e28f0408ff2fedefdc040f23972c2dbd7156b416df788bc22fe68`.

## Coherent ntdll pair and runtime overlay

- Treatment Unix: `4b16eda1e30b0d1678524b5414a4a098a5a8b85cb2abbe6553b9112ed3d8f272`.
- Matching ARM64X PE: `38841810948dd4e46abafa0bd103099bb3576ad95408e3645a9a7907f1828ea3`.
- Runtime dist: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist`.
- Pre/post-restored runtime Unix/PE: `14d3563d…` / `8e2a5691…`.
- Breakthrough floor made before this build: `artifacts/milestone-dist/abzu-abba-cleared-unixlib-resolve-BREAKTHROUGH-20260713/`, where Unix/PE remain `b60e43c3…` / `38841810…` and local SHA256SUMS is 6/6.
- Wrapper deploys both halves, hash-checks them, and restores both halves under an EXIT trap. No mixed Unix/PE deployment is allowed.

## Game, graphics, runner and environment

- Game SHA: `b9bdbc743742f45845b152df7663b042e2b5da5d318894584d70f6d63d6839e7`.
- DXMT d3d11/dxgi/winemetal: `842fa9e7…` / `30eb89bf…` / `e49765a9…`; runtime x64 winemetal overlay `7aa914d6…`; DXMT manifest `06e8f907…`.
- ABZU `mr-run.sh`: `d2d23492…`; executable/argv/prefix/dist are pinned in `run-once.sh`.
- Existing treatment stack remains enabled: builtin Unixlib resolve, CS forward, winemetal Unix lifecycle/fallback, ARM64 syscall bridge, native syscall/direct-callback splits, benign EH noop, DXMT, services + actxprxy, `WINEMSYNC=1`, JIT direct memory, native memmove, single lookup, 524288-entry IR cache.
- Candidate A remains explicitly unset. Old heap/event/main/wait observers and UI input trace are unset.
- Sole new exported observer is `MACRUNNER_HB_JIT_FAULT_TRACE=1`; timeout is reduced to 120 seconds by task requirement. Full child environment is captured byte-exact in `CHILD-ENV.bin` immediately before launch.
- Fresh throwaway prefix remains `USE_WARM_PREFIX=0`, `SKIP_WINEBOOT=1`; this state is part of the diagnostic contract, not silently inferred.

## Fail-closed and cleanup

Before consuming the attempt, the wrapper rejects prior attempt marker, build/title/Wine collisions, repository/hash drift, incoherent deploy, and records host/locale/timezone, all processes, free space and byte-exact translation-cache inventory. After the one attempt it records exit/process state, restores both runtime halves, re-inventories cache and records free space. Cache deletion, `make clean`, global Wine kills and retries are forbidden.
