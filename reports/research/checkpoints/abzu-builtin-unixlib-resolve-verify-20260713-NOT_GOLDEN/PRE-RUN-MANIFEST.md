# ABZU AMD64 builtin Unixlib resolver verify — immutable pre-run manifest

Date: 2026-07-13  
Classification: `DIAGNOSTIC_ONLY / NOT_GOLDEN`  
Attempt budget: exactly one; title hard timeout 300 seconds; no retry.  
Independent treatment: `MACRUNNER_HB_BUILTIN_UNIXLIB_RESOLVE=1`. On a null AMD64 guest Unix-call handle, HyperBridge derives the caller module from the guest return PC, resolves that module's `MemoryWineUnixFuncs`, and directly uses the returned table. Compared with the prior full ABZU Fix-A/dynamic-IAT script, exported environment differs by exactly this one variable.

No A/A pair is claimed. This run may establish absolute facts (resolver hit, non-null ws2 handle, returned hostname, reached HRESULTs/crashes); it must not be used for quantitative regress/progress claims.

## Repository identity

- Main HEAD: `4be5ec135492d622b13acc7a22a53738a0776024`
- ABZU worktree HEAD: `2b62f6b7ac71f1c64c00ce39e0ff6fb998dc01c8`
- Golden `e7cca2e3`: retained and not modified.
- Main dirty tree is pre-existing and broad. The run fails closed on hashes for every source/output/guard in the treatment path rather than assuming the rest of the dirty tree is clean.

## Patch-by-evidence boundary

- Previous candidate: run every Unix-capable AMD64 builtin DllMain.
- Why it failed: the sole 300s run logged nonzero ws2_32 and dnsapi funcs but stayed in early loader attach, produced only three ThreadInit successes, never reached game main/gethostname, and left graphics registration-only.
- This treatment targets: the proven null-handle `WINE_UNIX_CALL` boundary only.
- Why this class: `ret_addr` identifies the exact guest caller; `MemoryWineUnixFuncs` is Wine's class-level loader capability and `virtual.c` caches the loaded Unix handle.
- Not extending to: arbitrary DllMain execution, ws2/winemetal allowlists, event/wait semantics, or native export routing.

## Treatment source and guards

- `engine/wine/dlls/ntdll/loader.c`: `f68c801b25eacd29f3dbcb0965816360325ed1983d01ffbf01b7c38b131be8b5`
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c`: `5d1e65879f5c75acf3bc6f506c4863031c941f1eec2bc6fdc3c3ba8144f441ea`
- `engine/wine/dlls/ntdll/unix/sync.c`: `2d17e9a6bb78be743ef5a0906fdaa1a744a482b1cabb3d12e0c7f643039581ab`
- class guard: `a60b3cdfae62385319a513b1ae851d8e757b7b7b8c93f79bbbf590a9fc078a33`
- CS-forward guard: `67fb683fd589b8b48fb1e8eb5889a17b275576befd349383f72b10b69e348b5f`
- dynamic-IAT guard: `9c3c9ab4a29fad0a5c93fc258127b78da7d5e72d05bd2f670afa66c94952127b`
- build log: `242b2c61c4253b7cd68b4eff853ebcd9ca94f3efa4f8bb340f1c8d9ee5b45c90` (`errors=0`, `install skipped=0`, 35 warning lines in pre-existing warning classes)
- run-once script: `6f7ee09cee437307c9892994d1338272fac4918692351e9812b523eca259940b` (`bash -n` PASS)
- source guards: class resolver plus CS-forward, dynamic-IAT, event-lifecycle, and main-009c all PASS.

## Coherent ntdll pair

- built Unix ntdll: `b60e43c3c1730194c04842099d2e5a8cde2af491b305f0ea9f24467927c484ff`
- built ARM64X PE ntdll: `38841810948dd4e46abafa0bd103099bb3576ad95408e3645a9a7907f1828ea3`
- pre-run runtime Unix half: `14d3563d105defde6efae10563e58b3ac7e88278b2533b96e882c8d6efacf2b5`
- pre-run runtime PE half: `8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc`
- Both treatment halves deploy together and are restored by an EXIT trap.

## Graphics, game, runner

- runtime x64 winemetal overlay: `7aa914d654101470b9fa5440dd3a82331087208f5094a5d7c9cb0cb875d9fc24`
- source d3d11: `842fa9e7228184ba46d1b81e2c75c0952fa813516b812f072c3d8cb9ff818ce8`
- source dxgi: `30eb89bf1b37e2d650006105087c4c2e3e13cd1c9b067d47e793dcc6b93f8035`
- source winemetal: `e49765a9e1a2f0f0522d24c07b0db48f769afa4e84908eca9f8b289289a55929`
- DXMT manifest: `06e8f90784215761e1163ea54ee12a92f604ac55de0dad55d6563cbf56968358`
- runner: `d2d23492abd564693cab90cb0c49204b6f357f1af3f4244bd6cda6200eec8ee2`
- game executable: `b9bdbc743742f45845b152df7663b042e2b5da5d318894584d70f6d63d6839e7`

## Child environment

The script writes byte-exact `CHILD-ENV.bin`. Requested treatment stack is fixed:

- `MACRUNNER_HB_BUILTIN_UNIXLIB_RESOLVE=1`
- `MACRUNNER_HB_WINEMETAL_X64_DLLMAIN=1`
- `MACRUNNER_HB_WINEMETAL_UNIX_FALLBACK=1`
- `MACRUNNER_GRAPHICS_BACKEND=dxmt`
- `MACRUNNER_MR_RUN_START_SERVICES=1`
- `MACRUNNER_MR_RUN_REGSVR32_ACTXPRXY=1`
- `MACRUNNER_HB_ARM64_SYSCALL_BRIDGE=1`
- `MACRUNNER_HB_NATIVE_SYSCALL_SPLIT=1`
- `MACRUNNER_HB_EH_BENIGN_NOOP=1`
- `MACRUNNER_HB_NATIVE_DIRECT_CALLBACK_SPLIT=1`
- `MACRUNNER_HB_CS_FORWARD_NTDLL=1`
- `WINEMSYNC=1`
- `MACRUNNER_HB_IR_CACHE_SIZE=524288`
- `MACRUNNER_HB_JIT_DIRECT_MEM=1`
- `MACRUNNER_HB_NATIVE_MEMMOVE=1`
- `MACRUNNER_HB_SINGLE_LOOKUP=1`

The rejected `MACRUNNER_HB_BUILTIN_X64_DLLMAIN` gate and old wait/event/main/heap observers and UI-input trace are explicitly unset. Backend/overlay/prefix/service variables match the prior full ABZU verify script.

## Fail-closed runtime records

Before deployment the script records host/locale/timezone, branch/status, all-process snapshot, filesystem space, and byte-exact translation-cache inventory. Any title/Wine/build collision, head/hash drift, previous attempt marker, or incoherent deployment aborts before consuming the title run. Afterward it records exit/time/processes, restores both runtime halves, re-inventories cache, and records free space.

Pre-seal checks: serialization conflicts `0`; filesystem free `57 GiB`; translation cache retained.
