# Engine Change Journal — append-only log of every engine edit

**Purpose**: engine/ is large; only key source files force-tracked in git. This
journal is the **memory** of what changed/why, surviving context compaction.
Codex appends EVERY engine edit here. Never delete entries.

## Format (append newest at top)

```
## YYYY-MM-DD HH:MM — <short title>
File(s): path:line
Type: ROOT-FIX | REVERT | DIAGNOSTIC | WORKAROUND(TODO)
What: [exact change]
Why: [root cause / audit ref]
Verify: [how confirmed — smoke case / screenshot]
Status: applied | reverted | superseded-by-<entry>
```

## Rules
- Log BEFORE rebuild, update Status after verify.
- Mark WORKAROUND only with TODO + link to real root.
- If reverting a prior entry → reference it, mark old as superseded.
- Tracked engine files: `git diff engine/...` shows actual changes. Use it +
  this journal together = full memory.

---

## Entries

## 2026-07-28 08:15 — x64-thread-context mutex made recursive (SIGUSR1 suspend-handler self-deadlock)
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c:321
Type: ROOT-FIX
What: `macrunner_hb_x64_thread_context_mutex` initializer changed `PTHREAD_MUTEX_INITIALIZER` → `PTHREAD_RECURSIVE_MUTEX_INITIALIZER` (+ contract comment). Covers all 8 lock sites at once.
Why: run31b live deadlock, instruction-level proven: HK boot froze after `<RI> Input initialized.` when SIGUSR1 (Wine suspend, e.g. Mono/Boehm GC stop-the-world) interrupted an x64 thread inside the per-block `macrunner_hb_update_current_x64_context` trylock region (interrupted PC update+0x138, disasm-proven inside trylock..unlock), and its own `usr1_handler → NtGetContextThread → macrunner_hb_get_x64_thread_context` hard-locked the same non-recursive mutex = self-deadlock; `Loading.PreloadManager` (try_thread_creation_semantic→NtGetContextThread) deadlocked behind it → whole-process freeze. Report: reports/phase4-hollow-knight/RUN31B-RESULT-SUSPEND-HANDLER-SELF-DEADLOCK-ON-X64-CTX-MUTEX-20260728.md.
Verify: recursive sig `0x32aaaba2` byte-verified at __DATA 0x19cd00 in build output AND deployed `dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so` (SHA 9f616472a18ca3a6→7ab1d576c3d778be); prior instruments survived relink. Runtime validation = run32 (pending slot).
Status: applied

## 2026-07-21 18:44 — Decode legacy MOVNT packed-store family for Unity JIT corridor
File(s): engine/hyperbridge/src/hb_decode_x64.c:4598; engine/hyperbridge/src/hb_decode_x86.c:1235; engine/hyperbridge/tests/hb_test_runner.c:13827
Type: ROOT-FIX
What: Added x64/x86 decode coverage for legacy `MOVNTPS m128,xmm`, `MOVNTPD m128,xmm`, and `MOVNTDQ m128,xmm` as 128-bit `HB_INS_SSE_MOV` memory stores, rejecting register ModRM forms. Added focused decode/JIT tests and an exact Unity corridor compile test.
Why: The pixel-first HK continuation reproduced the exact Unity block and proved the real JIT failure was not the first RIP-relative `MOVDQA`, but `UnityPlayer.dll+0xe1028d` bytes `66 0f e7 14 07` (`MOVNTDQ m128,xmm`) lowering to `UNSUPPORTED/273`.
Verify: `--fast-family unity_movnt_store` PASS 4/4; real Unity corridor and containing `.pdata` function compile with `block_fail=0 instr_fail=0`; phase1_core PASS 50/50; W03 static/source floors PASS. Broad default `hb_test_runner` remains the known non-green 453/29 floor. One bounded HK runtime was attempted once and stopped fail-closed before managed product deployment, so product pixels remain UNKNOWN.
Status: applied-uncommitted-NOT_GOLDEN

## 2026-07-21 12:10 — Snapshot W03 env gates before suspend-capable workers
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c; engine/wine/dlls/ntdll/unix/loader.c; engine/hyperbridge/include/hb_memory.h; engine/hyperbridge/include/hb_runtime.h; engine/hyperbridge/src/hb_memory.c; engine/hyperbridge/src/hb_runtime.c
Type: ROOT-FIX
What: Removed the obsolete per-import HOTIMPORT getenv and eagerly snapshotted only the sync-import, hb_memory_write direct-gate, and persistent-cache-key environment flags in the existing single-threaded construction path. Hot execution now reads initialized fields while retaining each flag's exact presence/nonempty/atoi semantics.
Why: A frozen HK sample pinned SIGUSR1 delivery while the import thunk held libc's environment lock; wait_suspend retained that lock and peer hb_memory/cache paths formed a process-wide fanout.
Verify: Focused source contract 6/6, jit_signal_ownership 2/2, callback/x18/signal static floors PASS, translation cache 4/4. Fresh isolated ntdll pair Unix c185ef86... / PE 9c91c7d6... deployed hash-identically; Unix codesign PASS. Sole HK child passed +301.5s and +330s sample had zero wait_suspend/usr1_handler/unfair-lock fanout, but missing required noalloc profiler init forced INVALID_DEPLOYMENT stop at +456.3s. Broad HB runner remains nondeterministic/not green and was not represented as a passing floor.
Status: applied-uncommitted-NOT_GOLDEN

## 2026-07-18 16:42 — Add bounded scene/data file API probe
File(s): engine/wine/dlls/ntdll/unix/file.c, tools/test_hb_scene_loading_probe_source.py
Type: DIAGNOSTIC
What: Added default-off `MACRUNNER_HB_SCENE_LOADING_PROBE` records for bounded, semantic `NtCreateFile`/attribute-query observations of serialized scenes, global managers, resources, managed runtime, and online-subsystem paths. Records include API, path class, status, access/disposition/options, NT path, and resolved Unix path; the probe never changes file semantics.
Why: Hollow Knight's black bootstrap needed a direct discriminator between inaccessible Unity/GOG data and a managed scene-activation gate after graphics, shader translation, and vertex upload were excluded.
Verify: Focused source contract PASS. Project-env/ccache ntdll build RC0 with no `install skipped`; signed build/dist are byte-identical SHA `76c77d0ac5d43dce29e2f4310d943dd2c963cbe94d9cb0b20efe7b3d4afb7724`, strict codesign PASS. One sealed 1800s run proved Mono/scripts complete and game-local Galaxy DLL access succeeds. Unity serialized I/O bypassed these hooks, an explicit limitation. Static IL plus the immutable empty PlayerPrefs template proved the actual wall is `StartManager` waiting for first-run `GameLangSet` confirmation before `Menu_Title` activation; report `SCENE-LOADING-CAUSE.md`.
Status: applied; diagnostic root cause complete

## 2026-07-17 10:10 — Add bounded DXMT backbuffer readback and Draw-state probe
File(s): engine/dxmt/src/dxmt/dxmt_context.cpp, engine/dxmt/src/winemetal/{Metal.hpp,winemetal.h,winemetal_thunks.c,winemetal_thunks.h,unix/winemetal_unix.c}
Type: DIAGNOSTIC
What: Added default-off `MACRUNNER_HB_GPU_READBACK_PROBE`. DXMT appends W^X-independent Metal texture-to-shared-buffer blits after bounded color clears, after bounded render passes containing Draw commands, and immediately before presenter encoding. Completion handlers calculate an exact logical-pixel FNV-1a hash, black/nonblack/colorful counts and RGBA ranges/means without a GPU wait. The same gate captures the first 32 actual Draw calls with persistent encoder RTV/PSO/VS/PS, viewport/scissor, raster, buffer and texture state.
Why: Hollow Knight reaches 3046 Draws and 1390 successful Present1 calls while the native window remains byte-black. GPU truth is required to distinguish zero fragment coverage from a presenter failure before changing renderer or managed scene behavior.
Verify: ARM64 Unix WineMetal and x86_64 `d3d11.dll`/`winemetal.dll` builds passed; the only ARM64 linker diagnostic is the pre-existing LLVM 15 libunwind re-export warning, x86_64 final build has 0 errors/0 warnings and no `install skipped`. Unix-call native/WOW64 tables both contain index 132. Deployed SHAs: signed `winemetal.so` `36924559…`, `d3d11.dll` `df8a4863…`, `winemetal.dll` `612696f2…`; strict codesign passed. Runtime verification pending sealed 1800 s HK run.
Status: applied; runtime pending

## 2026-07-16 09:05 — Add bounded opcode 0x27fd frame-finalize probe
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c
Type: DIAGNOSTIC
What: Added env-gated `MACRUNNER_HB_FRAME_FINALIZE_PROBE`. The exact Gfx-worker dequeue boundary records Unity base without a module scan; opcode handler `Unity+0x11cc3b5` captures payload, Gfx object, vtable and slot `+0xa38`. Only the first configured events trace block/module transitions, vfunc entry bytes, return address and register arguments; all logging is budgeted and inactive outside the exact event chain.
Why: Readable-range evidence proved handler selection but did not identify whether dominant opcode `0x27fd` was frame finalization, submission, or Present. A target-resolved event trace was required before patching the renderer or scheduler.
Verify: Project-env/ccache build RC0 with no `install skipped` or probe compile error; built SHA `dc7232ec070cfd20f8743dfbc97882acc01dd1b834df01fb05dc389a50eb249f`, signed deployed SHA `72527cecf4dab85b309241e65a086de8fd8ac959c7bdf8b3a04dc7c8b88b5409`, strict codesign passed. Sealed 600 s run recorded 244 `0x27fd` ranges, resolved `+0xa38` to `Unity+0x6cda80`, and observed 25 Unity-only returns with no D3D/DXGI transition. Report `FRAME-FINALIZE-PROBE-RESULT.md`.
Status: applied

## 2026-07-16 04:53 — Add event-bounded Unity readable-range decoder probe
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c
Type: DIAGNOSTIC
What: Added env-gated `MACRUNNER_HB_READABLE_RANGE_DECODE_PROBE`. On a Gfx-worker wake/refill event it captures the packed readable start/end and exact bytes immediately before the fused refill-return/consumer block, recomputes effective readable as `min(+0x110, max(0, +0xc0 - +0x114))`, selects the one range containing `0x27a9`, then records a bounded consumer control-flow trace. The prior ring per-block probe can remain off.
Why: The previous ring trace proved token receive and readable-limit update but could not distinguish a missing decoder/handler from a later rendering wall. Two invalid probe formulations were rejected: an epilogue-only hook was skipped by HyperBridge block fusion, and `+0x10c` was stale at block entry. The final probe uses the actual fused block boundary and producer counters.
Verify: Project-env/ccache rebuild returned RC0 with no `install skipped`; strict deployed codesign passed. Built ntdll SHA `1751613233a94f6f0d708898a789517d8d4ebf347092d13d65e8e2952a1cf7f7`, signed dist SHA `acdc9346fd00442b927140cdbae28c4a17a691cfff5da7c0a52f8396b5e55455`. Sealed 600 s diagnostic run decoded `0x27a9 -> Unity+0x11cb4f7`, then `0x2713` into real D3D11 ClearRTV/ClearDSV calls; report `READABLE-RANGE-DECODER-RESULT.md`.
Status: applied

## 2026-07-15 23:18 — Add event-armed Unity Gfx ring-pop/callback probe
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c:23251-23690,24164-24247,34925
Type: DIAGNOSTIC
What: Added env-gated `MACRUNNER_HB_RING_POP_PROBE` with an independent ~5000-line budget. A Gfx-worker `dequeue-ready` event arms TLS state for exactly one token cycle; the next wait or a 4096-block cap disarms it. The probe preserves the cleared wait address through the scheduler's atomic signal record, resolves the worker object from nonvolatile `RDI` on exact Unity resume, asserts `[object+0x60] == signal`, then records raw availability fields (`c0/10c/110/114/140/148/14c/154`), backing-buffer words, and dynamic callbacks `+0x180/+0x118/+0x128` at the exact Unity worker RVAs.
Why: The sealed scheduler run proved wake→dequeue-ready pairs, including token `0x27a9`, but not whether the returned readable range advanced or which callbacks ran. Unity byte truth shows `object+0xc0 - object+0x114` is the current allocation-block availability and the post-wake chain tests `+0x180` then calls `+0x118`; event-armed snapshots distinguish failed refill, skipped callback, timing callback and downstream dispatch without a continuous observer.
Verify: Final source diff/audit passed. Rebuilt through project env/ccache with no new probe warning/error and no `install skipped`; unsigned build SHA `337521d996b875d4a1fcb76bcb1fd27ac8369a1fd881d380dd752d5909f51d63`, deployed ad-hoc signed SHA `5a8dce42961a2322327250647d2fd7bb8ea1f774854c1c31365280829eca1d9c`, strict codesign passed. Sealed 600s HK run proved 208/208 object-signal matches, `0x27a9` publication/refill, `+0x180=NULL`, `+0x118=Unity+0x9e0da0` timing callback, main alive at +590.918s, all fault families zero, and no real GetBuffer; report `RING-POP-PROBE-RESULT.md`.
Status: applied

## 2026-07-15 22:28 — Add event-driven Unity Gfx-thread boundary probe
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c:22899,23245-23455,23700-24009,29050,32899
Type: DIAGNOSTIC
What: Added env-gated `MACRUNNER_HB_GFX_THREAD_EVENT_PROBE`. It logs only thread create/name, Unity Gfx worker `WaitOnAddress` wait/return edges, a matching producer wake, and calls to exact synthetic import slot 984 (`0x6f0000003d80`). The valid build captures the caller of Unity's semaphore-release/acquire helper from `[rsp+0x30]` (nested import return + helper shadow/prologue), allowing the enqueue and worker-continuation sites to be disassembled. It does not register a per-block hook or create a sampler/observer thread.
Why: The post-u128 baseline creates the swapchain and keeps main alive, but never calls `GetBuffer`. Evidence is needed at the actual main-producer/Gfx-consumer boundary to distinguish no enqueue, missing wake, failed dequeue, or a command stream that never requests the backbuffer. The same run must also identify the hot synthetic thunk without guessing from its slot number.
Verify: Source diff-check and call/signature audit passed. The first diagnostic exposed and rejected the initial `+0x28` attribution; corrected rebuild through project env/ccache had no new probe warning/error and no `install skipped`. Valid build SHA `25ffbb6b349df01cabe403c4c14a488a5dfdacc252cf476123df031cba84ec97`, deployed ad-hoc signed SHA `8467388bd799ec9f6f630e8938e6092cdee2916b5b1f2e8a0e1651c7a84c5b45`, strict codesign passed. Sealed 600s HK run produced 240/240 enqueue-wake/dequeue-ready pairs (220/220 after MakeWindowAssociation), with no runtime fault family; report `GFX-THREAD-EVENT-PROBE-RESULT.md`.
Status: applied

## 2026-07-15 18:40 — Fix stale live-host write protection cache
File(s): engine/hyperbridge/src/hb_arm64_codegen.c:5985, engine/hyperbridge/tests/hb_test_runner.c:17136
Type: ROOT-FIX
What: Changed `hb_jit_live_host_ptr()` so write requests do not reuse cached live host pointers. Write paths now refresh Mach VM protection before allowing raw stores; stale RW cache entries no longer bypass W^X after a guest page flips back to RX. Expanded the executable-page XMM store regression to prime the live pointer cache while RW, flip the mapping to RX, then issue an unaligned 128-bit store across the page boundary.
Why: Hollow Knight reached a second raw memcpy-to-RX path through `hb_jit_helper_store_u128` and `hb_jit_live_host_ptr`. The previous 128-bit store fix covered the general memory-write path, but this helper could reuse a cached writable host pointer after Mono changed the live page protection, corrupting executable code and later blocking the graphics path.
Verify: `hb_test_runner --fast-family jit_xmm_store_wx` passed 2/2. Rebuilt `libhyperbridge.a`, relinked/deployed/codesigned arm64ec `ntdll.so`, then ran the 600s Hollow Knight verification: Mono 0x2791bc, UDF0, SIGILL, SIGBUS, fastfail, import951, c000007b and pc=0 were all 0; main stayed alive to timeout. Graphics remains blocked before real GetBuffer/RTV/OMSet/Present and pixel gate is black.
Status: applied

## 2026-06-07 03:48 — PE32 deploy: copy wow64/xtajit DLLs into syswow64
File(s): scripts/sync-prefix-from-dist.sh
Type: DIAGNOSTIC
What: Fixed prefix-sync deployment to explicitly mirror WOW64 CPU-provider modules into `syswow64` (`wow64cpu.dll`, `wow64.dll`, `wow64win.dll`, `xtajit.dll`), not only in `system32`.
Why: PE32/i386 handoff probes showed all `system32` backend DLLs present but `syswow64` missing, preventing 32-bit WOW64 path selection and producing `PE32_HANDOFF_TRACE` starvation with no `BTCpu` markers.
Verify: `bash scripts/sync-prefix-from-dist.sh --prefix "$PWD/bottles/generic-x86"` then checked with `ls`/`rg` confirms all four DLLs now exist in prefix `bottles/generic-x86/drive_c/windows/syswow64`; phase0 handoff traces still classify as `WAIT_TRACE_INSUFFICIENT` until wow64 handoff markers appear.
Status: applied

## 2026-05-24 18:31 — Add private guest low-VA backing primitive
File(s): engine/hyperbridge/include/hb_memory.h, engine/hyperbridge/src/hb_memory.c, engine/hyperbridge/tests/hb_test_runner.c
Type: ROOT-FIX
What: Added `hb_memory_map_private()` and `host_base` tracking so a fixed guest VA can be backed by a separate host mapping above macOS `__PAGEZERO`; read/write/protect now use host backing for allocated/private regions while preserving existing live-host mappings.
Why: PE32/i386 under native arm64 HyperBridge cannot use real low host addresses like `0x400000`; macOS kills low-pagezero arm64 processes, so the WOW64 bridge needs guest-VA identity decoupled from host backing.
Verify: `engine/hyperbridge make test` PASS; `./tests/hb_test_runner` reports `189 passed, 0 failed` including `memory_private_guest_low_va_backing`.
Status: applied-foundation

## 2026-05-24 12:25 — Fix shell32 drive PIDL path for file dialogs
File(s): engine/wine/dlls/shell32/shfldr_fs.c:75, engine/wine/dlls/shell32/shfldr_fs.c:1425, engine/wine/dlls/shell32/shfldr_fs.c:1518
Type: ROOT-FIX
What: Added a drive-PIDL path helper using `_ILGetDrive()` and used it in `IFSFldr_PersistFolder3_Initialize()` / `InitializeEx()` before falling back to `SHGetPathFromIDListW()`.
Why: Product Notepad++ Open dialog created correctly but the file list was blank. Trace proved `CreateFolderEnumList()` received a bogus drive display path (`L" (C:)"`) instead of `C:\...`, so shell32 enumerated no real files. The root was filesystem-folder initialization for drive PIDLs, not rendering, not icons, and not general input dispatch.
Verify: Rebuilt and synced x86_64/aarch64 `shell32.dll` and `comdlg32.dll`; `scripts/verify-build-freshness.sh` PASS. Real `MacRunner.app` card `Notepad++ Installed E2E` launched the installed prefix, and on-screen capture `reports/installer-e2e/product-open-dialog-after-drive-fix.png` shows the Open dialog populated with folders/files (`autoCompletion`, `plugins`, `langs.model.xml`, `notepad++.exe`, etc.).
Status: verified-onscreen

## 2026-05-23 15:31 — Toolbar icons verified on real screen
File(s): engine/hyperbridge/src/hb_decode_x64.c, engine/hyperbridge/src/hb_lift_x64.c, engine/hyperbridge/src/hb_interpreter.c, engine/wine/dlls/ntdll/unix/loader.c:1697
Type: ROOT-FIX
What: Closed the Notepad++ toolbar black-icon investigation on the stock Wine render baseline. The icon color fix remains in HyperBridge blit/vector handling; today's loader fix removed the bootstrap blocker that prevented the real window from reaching visual verification.
Why: Tiny probes had already proven PNG/ICO sources became colorful after the engine-side icon ABI fix, but visual closure was blocked until a real Notepad++ window could be captured. After the native-helper DLL-lane fix, Notepad++ reached a live CGWindow with colored toolbar icons.
Verify: Real on-screen user screenshot at 2026-05-23 15:25 shows colored Notepad++ toolbar icons. Window-cropped capture `reports/visual-regression/notepad-gate-manual-20260523-152238/notepad-window.bmp` reports `toolbar_band_colorful=10464` and `colorful=13304`; no black/gray toolbar square regression visible. Prior active-DLL probe `reports/phase-h/MILESTONE/TOOLBAR-BLACK-ONSCREEN-PINPOINT-20260523.md` had verified the active WinSxS ARM64 `COMCTL32.dll` path on the same stock-render baseline.
Status: verified-onscreen

## 2026-05-23 15:20 — Keep native helper DLL lane ARM64
File(s): engine/wine/dlls/ntdll/unix/loader.c:1697, engine/wine/dlls/ntdll/unix/virtual.c:3549
Type: ROOT-FIX
What: In ARM64 helper processes running under `MACRUNNER_HB_X64_LOADER`, force wrong-arch prefix builtin DLL candidates to resolve from the current-machine `aarch64-windows` builtin lane instead of falling back to the x64 prefix file. Also narrowed the AMD64-image machine-mismatch allowance to the real AMD64 app lane.
Why: Fresh host-exec trace proved `wineboot.exe` itself was native ARM64 at `0x140000000`, but it later mapped x64 `gdi32/shell32/user32/kernelbase/ntdll/comctl32/win32u` from `C:\windows\system32`, installed x64 fault handling inside the native helper, and previously led to the `module_from_pc`/`mach_vm_read_overwrite` fault-storm boundary.
Verify: Rebuilt/installed/codesigned `dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so`; `scripts/verify-build-freshness.sh` PASS. `reports/phase-h/wineboot-after-native-helper-dll-lookup-20260523-152205/` shows `wineboot_rc=0` after 2s, no `machine=0x8664` maps, and no `macrunner-host-exec-register-range` lines.
Status: verified

## 2026-05-23 05:06 — Toolbar black on-screen source boundary
File(s): engine/wine/dlls/comctl32/imagelist.c, reports/phase-h/MILESTONE/smoke-tools/npp_ui_smoke_helper.c, reports/phase-h/MILESTONE/TOOLBAR-BLACK-ONSCREEN-PINPOINT-20260523.md
Type: DIAGNOSTIC
What: Added temporary env-gated ImageList boundary probes and helper readback fields to compare the active toolbar source ImageList, add-time DIB bits, composite DC, and final toolbar DC on the real Notepad++ window path.
Why: Stock Wine render baseline is locked; old Wine render patches must not return. The probe needed to distinguish present/flush failure from source-memory corruption before ImageList draw.
Verify: `scripts/verify-build-freshness.sh` passed after each temporary comctl32 rebuild. Run `reports/phase-h/npp-x64-20260523-045849` proved active WinSxS ARM64 `COMCTL32.dll`, `normal_imagelist=0x000000010bfc6b40`, `printwindow_colorful_pixels=0`, and `add_dib_bits_raw` already black for all 32 toolbar images before alpha/mask processing. See `reports/phase-h/MILESTONE/TOOLBAR-BLACK-ONSCREEN-PINPOINT-20260523.md`.
Status: source-boundary-proved-temp-wine-trace-removed-stock-render-baseline-restored

## 2026-05-22 21:55 — Visual gate Marlett native-titlebar classification
File(s): tools/visual_regression/gate_notepad.py:663, reports/phase-h/MILESTONE/smoke-tools/marlett_render_probe.c, reports/phase-h/MILESTONE/smoke-tools/marlett_window_probe.c
Type: DIAGNOSTIC
What: Changed `marlett_center` to SKIP when the `nc_buttons` region is the native macOS titlebar, and added Marlett render/window probes that isolate Marlett glyph rendering from Notepad++.
Why: Valid Notepad++ capture is window-cropped, but `nc_buttons` points at the macOS top-level titlebar, not Wine-rendered caption buttons. Probes show `DrawFrameControl(DFC_CAPTION, ...)` renders min/max/close Marlett glyphs correctly in both DIB and real window-surface paths, so the prior `marlett_center` FAIL was a gate-region classification bug, not a Wine-render or HyperBridge opcode failure.
Verify: Re-running `gate_notepad.py` on `reports/visual-regression/notepad-gate/gate-screenshot-main.bmp` changes `marlett_center` to SKIP with `mean_luma=31.4`, `dark_ratio=0.977`; remaining FAILs are toolbar/bands/status and scrollbar functional harness permission.
Status: applied

## 2026-05-22 20:20 — Fix win32u KUSER shared-data address on Apple ARM64
File(s): engine/wine/dlls/win32u/message.c:49
Type: ROOT-FIX
What: Replaced the hardcoded `0x7ffe0000` `_KUSER_SHARED_DATA` pointer with `WINE_USER_SHARED_DATA_ADDRESS`.
Why: After the session-map ABI fix, the remaining native fault was a read at `0x7ffe0324`. That is `_KUSER_SHARED_DATA` low memory, while Apple/ARM64 uses the high Wine mapping from `winternl.h`. This proves a source/memory bug, not an opcode bug.
Verify: `reports/phase-h/npp-x64-20260522-201704` removed `fault=0x7ffe0324`; clean run `reports/phase-h/npp-x64-20260522-201955` has `native_faults=0`, `fault_7ffe=0`, `failed_desktop=0`.
Status: applied

## 2026-05-22 20:15 — Fix win32u NtMapViewOfSection ARM64 unix ABI boundary
File(s): engine/wine/dlls/win32u/win32u_private.h:36, engine/wine/dlls/win32u/winstation.c:129, engine/wine/dlls/win32u/dce.c:916, engine/wine/dlls/win32u/dib.c:1543
Type: ROOT-FIX
What: Added `win32u_map_view_of_section()` and routed win32u section-map callers through it. On ARM64 it passes `struct __wine_nt_section_extra *` to unix ntdll; other architectures keep the normal 10-argument call.
Why: Probe showed `shared_session_init()` opened `\KernelObjects\__wine_session`, then stalled before returning from `NtMapViewOfSection`. Source boundary: win32u was using the 10-argument public call while `ntdll/unix/virtual.c:6709` expects the ARM64 unix 8-argument form with `__wine_nt_section_extra *`; the 8th argument (`ViewUnmap`) was crossing as the destination pointer shape.
Verify: Causal run `reports/phase-h/npp-x64-20260522-201522` showed `after NtMapViewOfSection`, `after shared_session_init`, `after winstation_init`, and removed `Cannot get server thread queue` / `failed to create desktop window`.
Status: applied

## 2026-05-21 16:36 — Add primitive fallback for MDI menu-bar magic bitmaps
File(s): engine/wine/dlls/win32u/menu.c:1968
Type: ROOT-FIX
What: Added a generic GDI primitive glyph fallback for `HBMMENU_MBAR_*` magic bitmap buttons after the existing Marlett text draw. The fallback is guarded by a pixel check: Marlett remains the primary renderer, and primitives are used only if the sampled output is empty/black/unchanged.
Why: Real CG captures show the Notepad++ top-right MDI menu buttons as empty/black square frames. Trace and source audit identify this as `win32u/menu.c` magic bitmap handling, not `user32/DrawFrameControl`, so the earlier `user32/uitools.c` fallback cannot affect the visible path.
Verify: pending rebuild + real CG crop of the top-right MDI buttons; toolbar must remain colorful.
Status: applied

## 2026-05-21 15:16 — Restore DFC_CAPTION glyph fallback for Marlett-empty controls
File(s): engine/wine/dlls/user32/uitools.c:912, engine/wine/dlls/win32u/defwnd.c:1333
Type: ROOT-FIX
What: Added a DFC_CAPTION family fallback renderer for close/min/max/restore symbols using the existing GDI primitive layer, and call it after the Marlett `TextOut` path in both user32 `DrawFrameControl(DFC_CAPTION)` and win32u nonclient caption drawing.
Why: Audit `docs/ENGINE-AUDIT-RENDER-marlett.md` identifies the Marlett glyph fetch/render path as the empty-button root; the real Notepad++ window shows the MDI caption buttons as empty square frames. Those client-area buttons use `user32/nonclient.c` -> `DrawFrameControl(DFC_CAPTION)` -> `user32/uitools.c`, so fixing only `win32u/defwnd.c` would miss the visible defect.
Verify: pending rebuild + strict real-window center crop for close/min/max/restore buttons. Build note: user32 has no `GetSysColorPen`, so the user32 fallback creates a solid pen from `GetSysColor(colorIdx)` and deletes it after drawing.
Status: applied

## 2026-05-21 15:16 — Route transparent masked imagelist draws through mask alpha
File(s): engine/wine/dlls/comctl32/imagelist.c:1712
Type: ROOT-FIX
What: Extended `ImageList_DrawIndirect` so transparent masked image-list draws without true alpha use `alpha_blend_image()`, which already derives alpha from `hbmMask`, instead of the legacy black-filled temp bitmap plus SRCAND/SRCPAINT path.
Why: Audit `docs/ENGINE-AUDIT-RENDER-tab.md` traces the Notepad++ tab black square to `ILD_TRANSPARENT` mask draws where the temp bitmap starts black and transparent pixels are copied through as black. The alpha-from-mask path preserves destination background for masked-out pixels.
Verify: real-window screenshot supplied by user at 15:20 showed this was too broad: the tab artifact was reduced, but the normal toolbar image-list path regressed to blank slots.
Status: superseded-by-2026-05-21-15:38-transparent-mask-background-fill

## 2026-05-21 15:38 — Use destination background for transparent mask temp bitmap
File(s): engine/wine/dlls/comctl32/imagelist.c:1712, engine/wine/dlls/comctl32/imagelist.c:1765
Type: ROOT-FIX
What: Restored the normal `ImageList_DrawIndirect` alpha dispatch so only true alpha/explicit alpha states use `alpha_blend_image()`. For the legacy `ILD_TRANSPARENT + hbmMask` path, initialize the temporary color bitmap from the actual destination pixel at the draw point, falling back to `GetBkColor()`, instead of black before `SRCAND`/`SRCPAINT`.
Why: The audit's minimal-risk fix is to remove the black temp fill that creates tab black squares, while preserving the classic image+mask path needed by toolbar icons.
Verify: pending rebuild + real-window toolbar, tab, and Save As icon crops.
Status: applied

## 2026-05-21 14:32 — Remove superseded disabled-mask experiments
File(s): engine/wine/dlls/comctl32/toolbar.c:736, engine/wine/dlls/comctl32/imagelist.c:1570
Type: REVERT
What: Removed the sentinel-DIB disabled silhouette helper from `TOOLBAR_DrawMasked` and reverted the non-alpha `ILS_SATURATE` mask polarity inversion. The verified fix is the direct default-image fallback in `TOOLBAR_DrawImage`, not the mask/saturate experiments.
Why: Real-window verification after the 14:24 change showed visible disabled icons, while the 14:04/13:39 attempts either produced blank slots or a full tile. Keeping those experiments active would risk unrelated indeterminate/masked toolbar states.
Verify: Rebuild `reports/rebuild-comctl32-disabled-direct-default-clean-20260521-1432.log`; trace `reports/disabled-toolbar-direct-default-clean-retry-20260521-1440.trace.log` still shows all disabled slots using `draw_masked=0`; real-window crop `reports/phase-h/npp-x64-20260521-144105/manual-disabled-slots-y100.png` keeps visible icons.
Status: verified

## 2026-05-21 14:24 — Draw disabled toolbar fallback from default imagelist
File(s): engine/wine/dlls/comctl32/toolbar.c:934
Type: ROOT-FIX
What: For disabled toolbar buttons with no disabled image list, keep the default image list and draw it through the normal `ImageList_Draw` path instead of routing to `TOOLBAR_DrawMasked`.
Why: The 14:04 sentinel-DIB attempt proved the branch was active, but trace `reports/disabled-toolbar-rendered-silhouette-20260521-1420.trace.log` showed `changed=256 transparent=0` for every disabled slot, so the offscreen draw produced a full tile rather than an icon silhouette. The actual root for the current blank-hole family is the disabled fallback route into `TOOLBAR_DrawMasked`; bypassing that route uses the same visible default-image path as enabled toolbar buttons.
Verify: Rebuilds `reports/rebuild-comctl32-disabled-direct-default-20260521-1424.log` and `reports/rebuild-comctl32-disabled-direct-default-clean-20260521-1432.log`; traces `reports/disabled-toolbar-direct-default-20260521-1424.trace.log` and `reports/disabled-toolbar-direct-default-clean-retry-20260521-1440.trace.log` show indices 2/3/10/11/26/28/29/31 all using `draw_masked=0`; real-window crops `reports/phase-h/npp-x64-20260521-142539/manual-disabled-slots-y100.png` and `reports/phase-h/npp-x64-20260521-144105/manual-disabled-slots-y100.png` show visible Save/SaveAll/Undo/Redo/macro icons with per-slot content >0.
Status: verified

## 2026-05-21 14:04 — Render disabled toolbar silhouettes from normal image
File(s): engine/wine/dlls/comctl32/toolbar.c:736
Type: ROOT-FIX
What: Added a disabled toolbar renderer for the `TOOLBAR_DrawMasked` zero-alpha+mask fallback family: draw the normal image-list item into a sentinel 32bpp DIB, derive opacity from pixels that actually changed, convert those pixels to a premultiplied grayscale disabled silhouette, and alpha-blend it onto the toolbar.
Why: Trace `reports/phase-h/npp-x64-20260521-140013/stderr.log` proved the disabled fallback branch was active for indices 2/3/10/11/26/28/29/31 and source RGB existed, but `alpha_blend_mask_alpha ... opaque=0 transparent=256` for every slot. The mask bitmap is empty for this family, so mask-derived alpha cannot produce Save/SaveAll/Undo/Redo/macro silhouettes.
Verify: Rebuild `reports/rebuild-comctl32-disabled-rendered-silhouette-20260521-1404.log` plus real-window crop `reports/phase-h/npp-x64-20260521-141716/manual-toolbar-band.png` still showed the disabled slots blank; trace `reports/disabled-toolbar-rendered-silhouette-20260521-1420.trace.log` showed `changed=256 transparent=0`, proving the sentinel draw generated a full tile, not an icon silhouette.
Status: superseded-by-2026-05-21-14:24-draw-disabled-toolbar-fallback-from-default-imagelist


## 2026-05-21 13:39 — Correct disabled saturate mask polarity
File(s): engine/wine/dlls/comctl32/imagelist.c:1570
Type: ROOT-FIX
What: For non-alpha `ILS_SATURATE` image-list draws, interpret the monochrome mask as the opacity silhouette before `GdiAlphaBlend`; this is the disabled toolbar zero-alpha+mask family used by Save/SaveAll/Undo/Redo/macro buttons.
Why: The 12:55 fix routed disabled fallback buttons into the right saturate path, but preserved the normal image-list mask polarity (`1 = transparent`). Per-slot verification on `reports/phase-h/npp-x64-20260521-132226/disabled_slots_y*.png` showed the exact disabled slots were still blank holes, meaning the silhouette bits were being made transparent.
Verify: Rebuild `reports/rebuild-comctl32-disabled-mask-polarity-20260521-1339.log` plus trace `reports/phase-h/npp-x64-20260521-140013/stderr.log` showed the mask path still produced `opaque=0 transparent=256`; per-slot crop `reports/phase-h/npp-x64-20260521-135005/disabled_slots_postfix.png` still showed blank holes.
Status: superseded-by-2026-05-21-14:04-render-disabled-toolbar-silhouettes-from-normal-image


## 2026-05-21 12:55 — Disabled toolbar mask-backed saturate path
File(s): engine/wine/dlls/comctl32/toolbar.c:740, engine/wine/dlls/comctl32/imagelist.c:1523
Type: ROOT-FIX
What: Routed disabled toolbar fallback images that have a monochrome mask but no alpha through `ILS_SATURATE`, and added gated trace around the saturate/mask-alpha path.
Why: Notepad++ does not provide a disabled imagelist for Save/Undo/Redo/macro buttons, so comctl32 falls back to the default zero-alpha+mask imagelist via `TOOLBAR_DrawMasked`. The old legacy mask construction produced flat grey squares; the disabled family needs grayscale drawn through the mask silhouette.
Verify: Rebuild `reports/rebuild-comctl32-disabled-saturate-20260521-1329.log`; trace `reports/phase-h/disabled-toolbar-postfix-20260521-130541.log` showed 8 disabled fallback buttons using `draw_masked_saturate ... state=ILS_SATURATE mask=1`, but the cited crops were wrong slots (Close/Print/macro visible icons, not disabled Save/SaveAll/Undo/Redo). Full onscreen capture `reports/phase-h/npp-x64-20260521-132226/onscreen-icon-real-window.png` still shows disabled slots as blank holes, so this verification was false.
Status: verify-was-false-disabled-slots-open

## 2026-05-21 12:06 — Onscreen icon verification reset
File(s): engine/wine/dlls/comctl32/imagelist.c:504, reports/agent-prompts/CODEX-ICONS-FALSE-VERIFIED-ONSCREEN.txt
Type: DIAGNOSTIC
What: Reclassified icon verification: `toolbar_render_cg` and dialog CG crops are proxy captures only; they no longer count as proof for toolbar or Save As icon rendering.
Why: User screenshots from the same build showed the real Notepad++ toolbar still grey while `toolbar_render_cg` reported colorful pixels, proving CG capture and the onscreen window path diverge.
Verify: Next valid proof must be a live onscreen window capture or screen crop of the real Notepad++/Save As window, not `toolbar_render_cg`.
Status: applied

## 2026-05-21 11:10 — Remove Marlett caption fallback overlays
File(s): engine/wine/dlls/win32u/defwnd.c:1333, engine/wine/dlls/win32u/menu.c:1968, engine/wine/dlls/user32/uitools.c:912, engine/wine/dlls/user32/nonclient.c:105
Type: REVERT
What: Removed the PatBlt fallback glyph overlays that drew manual min/max/close outlines after the native Marlett `TextOut` path.
Why: `marlett_probe.exe` showed builtin Marlett is present and `CreateFontIndirect("Marlett")` selects Marlett with valid glyph outlines for `0/1/2/r`; the visible empty-square controls were the fallback overlays, not missing Marlett. `CreateFontA` selecting GOST is a separate x64 stack-argument issue and does not apply to native caption drawing.
Verify: pending rebuild + Notepad caption screenshot/visual gate.
Status: applied

## 2026-05-21 11:09 — Revert Marlett font matcher trace
File(s): engine/wine/dlls/win32u/font.c:2349
Type: REVERT
What: Removed temporary exact-face matcher diagnostics added for Marlett/Tahoma.
Why: The focused x64 probe showed Marlett loads and selects correctly through `CreateFontIndirect`; no permanent font matcher instrumentation is needed.
Verify: pending rebuild.
Status: applied

## 2026-05-21 10:50 — Marlett font matcher trace
File(s): engine/wine/dlls/win32u/font.c:2349
Type: DIAGNOSTIC
What: Added gated `MACRUNNER_TRACE_VISUAL_FONT` diagnostics around exact font-family matching for Marlett/Tahoma to print requested charset signature, candidate face signatures, selected face, and rejection conditions.
Why: x64 `marlett_probe.exe` showed `WINEDATADIR` is correct and `EnumFontFamiliesEx("Marlett")` sees Marlett, but `CreateFont("Marlett", SYMBOL_CHARSET)` selects `GOST Type BU`; the evidence points to matcher rejection rather than missing font files.
Verify: Replaced by `marlett_probe.exe` using `CreateFontIndirectA`, which selected `Marlett` and returned valid glyph outlines for `0/1/2/r`; this disproved the matcher-missing-font hypothesis for native caption drawing.
Status: superseded-by-2026-05-21-11:09-revert-marlett-font-matcher-trace


## 2026-05-21 10:02 — Mask-backed binary alpha image lists
File(s): engine/wine/dlls/comctl32/imagelist.c:504
Type: ROOT-FIX
What: Generalized `add_with_alpha` classification for 32bpp images with a separate monochrome mask and binary/near-binary alpha; these mask-backed resources now use the classic image+mask path instead of alpha blending.
Why: Save As trace `reports/phase-h/npp-x64-20260521-095156` showed shell tree/list image lists had `hbmMask`, binary/near-binary alpha, and colorful source entries, while the CG icon bands stayed `colorful=0`. This is the same mask-backed family as the toolbar opaque-alpha case, extended by evidence to partial binary alpha.
Verify: `reports/phase-h/npp-x64-20260521-100358` only proved the offscreen/CG capture path (`toolbar_render_cg status=PASS colorful_pixels=9136` and CG Save As crop `colorful=880`). User screenshots from the same family showed the real onscreen toolbar still grey, so this verification was false for the window path.
Status: verify-was-false-onscreen-open

## 2026-05-21 09:56 — Save As image-list trace
File(s): engine/wine/dlls/comctl32/imagelist.c:397, reports/phase-h/npp-x64-20260521-095156/stderr.log
Type: DIAGNOSTIC
What: Ran full Save As CG probe with `MACRUNNER_TRACE_TOOLBAR_ICONS=1`; tied `SysTreeView32/SysListView32` image lists to `ImageList_Add/Draw` entries.
Why: The existing folder classifier only checked for black squares; manual crop showed no colored folder glyphs, requiring trace of the actual common-control image lists.
Verify: image lists `0x000000010bf42010` and `0x000000010bf41f40` had colorful source entries but Save As icon bands remained `colorful=0`.
Status: applied

## 2026-05-21 09:40 — Restore toolbar opaque-alpha mask family
File(s): engine/wine/dlls/comctl32/imagelist.c:504
Type: ROOT-FIX
What: Restored `add_with_alpha` classification for 32bpp images with fully opaque alpha plus a separate monochrome mask; this family now falls back to the classic image+mask path instead of being stored as alpha-only image data.
Why: Evidence run `reports/phase-h/npp-x64-20260521-093529` showed actual toolbar imagelist `0x000000010C021C00` had 32 entries, 30/32 with `alpha_pixels=256/256` and `mask=1`, while `toolbar_render_cg` failed with `colorful_pixels=0`. This is a separate BMP+mask family from indexed ICO folder icons.
Verify: Rebuild `reports/rebuild-comctl32-toolbar-20260521-094052.log`; later full Visual Regression Lab only reported `toolbar_render_cg status=PASS colorful_pixels=9136`, which is now known to be an invalid proxy for the real onscreen toolbar.
Status: verify-was-false-onscreen-open

## 2026-05-21 09:39 — Toolbar two-path trace
File(s): engine/wine/dlls/comctl32/imagelist.c:397, reports/phase-h/npp-x64-20260521-093529/stderr.log
Type: DIAGNOSTIC
What: Ran bounded UI smoke with `MACRUNNER_TRACE_TOOLBAR_ICONS=1`; parsed actual Notepad++ toolbar normal imagelist alpha/mask profile before changing code.
Why: The prior revert mixed two icon families; trace was required to distinguish toolbar 32bpp BMP+mask from folder indexed ICO conversion.
Verify: `toolbar_render_cg status=FAIL colorful_pixels=0`; toolbar imagelist had `mask=1` on 32/32 entries and fully opaque alpha on 30/32 entries.
Status: applied

## 2026-05-21 09:14 — Keep indexed ICO palette expansion root fix
File(s): engine/wine/dlls/user32/cursoricon.c:768, engine/wine/dlls/user32/cursoricon.c:803, engine/wine/dlls/user32/cursoricon.c:966
Type: ROOT-FIX
What: Kept explicit 4bpp/8bpp indexed icon palette expansion into a 32bpp color bitmap via `SetDIBits`, with caller-side failure check and warning.
Why: Kimi audit `ENGINE-AUDIT-RENDER-shell-icons.md` identifies silent `StretchDIBits` indexed-to-32bpp conversion failure as the shared root for folder icons and toolbar icon color loss.
Verify: Full Visual Regression Lab `reports/phase-h/npp-x64-20260521-100358` reported `folder_icons_visual status=PASS` and CG Save As crop `colorful=880`; because toolbar CG diverged from the real window, Save As folder icons need a real onscreen dialog crop before this counts as verified.
Status: applied-pending-onscreen-verify

## 2026-05-21 23:46 — HyperBridge blit SIMD batch
File(s): engine/hyperbridge/include/hb_decoder.h, engine/hyperbridge/include/hb_ir.h, engine/hyperbridge/src/hb_decode_x64.c, engine/hyperbridge/src/hb_lift_x64.c, engine/hyperbridge/src/hb_interpreter.c, engine/hyperbridge/tests/hb_test_runner.c
Type: ROOT-FIX
What: Added x64 decode/lift/interpreter coverage for the missing blit SIMD batch: `PMULLW`, `PMULHW`, `PMULHUW`, `PMADDWD`, `PADDSB`, `PADDSW`, `PADDUSB`, `PADDUSW`, `PAVGB`, `PAVGW`, and SSSE3 `PSHUFB`. Added an explicit unaligned `MOVDQU` runtime test; `MOVDQA/MOVDQU` remain handled by the existing generic `SSE_MOV` path.
Why: The pure-upstream `comctl32` oracle still renders toolbar/dialog icons as black/missing under HyperBridge, so the fix belongs in the translator. These instructions are common in alpha/blit/color conversion paths; semantics were matched to Intel SSE2/SSSE3 behavior and the sse2neon equivalents (`vqadd`, rounded average, multiply-high/madd, and PSHUFB mask-zero shuffle).
Verify: Forced rebuild of `engine/hyperbridge/libhyperbridge.a` and `tests/hb_test_runner`: 174 passed, 0 failed. Forced relink/codesign of `dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so`; `scripts/verify-build-freshness.sh` PASS. Live run started as `reports/phase-h/npp-x64-20260521-234647`; on-screen verdict pending user check.
Status: applied-pending-onscreen-verify

## 2026-05-21 09:12 — Revert mono icon mask bypass
File(s): engine/wine/dlls/user32/cursoricon.c:768, engine/wine/dlls/user32/cursoricon.c:990
Type: REVERT
What: Removed `create_stretched_mono_icon_mask` and the `mono_mask_stretched` bypass; monochrome icons now use the normal `StretchDIBits` mask-copy path with return checking.
Why: The requested root fix is indexed 4bpp/8bpp palette expansion for ICO color planes, not a monochrome icon special case.
Verify: pending rebuild + Visual Regression Lab toolbar and Save As folder icon screenshot.
Status: applied

## 2026-05-21 09:08 — Remove stale CopyImage fallback helper
File(s): engine/wine/dlls/comctl32/imagelist.c:177
Type: REVERT
What: Removed unused `macrunner_icon_stats` / `macrunner_get_icon_stats` helper left behind after deleting the CopyImage black-icon source fallback.
Why: The helper existed only to support the removed band-aid path and would otherwise keep misleading diagnostic code in comctl32.
Verify: pending rebuild + Visual Regression Lab toolbar and Save As folder icon screenshot.
Status: applied

## 2026-05-21 09:05 — Revert imagelist icon band-aids
File(s): engine/wine/dlls/comctl32/imagelist.c:560, engine/wine/dlls/comctl32/imagelist.c:1772, engine/wine/dlls/comctl32/imagelist.c:2852
Type: REVERT
What: Removed opaque-alpha-with-mask fallback in `add_with_alpha`, transparent destination pixel sampling in `ImageList_DrawIndirect`, and CopyImage black-icon source fallback in `ImageList_ReplaceIcon`.
Why: User-provided Kimi audit identifies the icon root cause in indexed ICO conversion via `user32/cursoricon.c`; these comctl32 changes were band-aids and had regressed toolbar rendering.
Verify: pending rebuild + Visual Regression Lab toolbar and Save As folder icon screenshot.
Status: superseded-by-2026-05-21-09:40-restore-toolbar-opaque-alpha-mask-family

(append here)

## 2026-05-21 17:24 — Indexed icon SetDIBits target selection
File(s): engine/wine/dlls/user32/cursoricon.c:966
Type: ROOT-FIX
What: Kept the explicit 4bpp/8bpp ICO palette expansion path, but stopped selecting the destination DDB into the memory DC before calling `SetDIBits`; non-indexed icons still select the DDB for `StretchDIBits`.
Why: `SetDIBits` is not a blit into the selected DC target. The previous root fix could silently fail or leave shell folder/document icons blank because it wrote to a bitmap that was already selected.
Verify: Live Open dialog after rebuild `reports/phase-h/npp-x64-20260521-180103` froze and stopped accepting UI input.
Status: reverted-by-2026-05-21-18:08-dialog-freeze

## 2026-05-21 18:08 — Revert indexed icon SetDIBits target selection
File(s): engine/wine/dlls/user32/cursoricon.c:966
Type: REVERT
What: Restored the previous selected-DC call sequence for the indexed ICO `SetDIBits` path.
Why: The not-selected DDB change improved some dialog glyphs but caused a real Open dialog UI freeze; keep the app usable and continue folder-icon work through a safer HICON/ImageList path.
Verify: pending rebuild/install plus live Open dialog responsiveness check by user.
Status: applied-pending-onscreen-verify

## 2026-05-21 18:28 — Remove destination GetPixel from transparent imagelist path
File(s): engine/wine/dlls/comctl32/imagelist.c:1766
Type: ROOT-FIX
What: Kept the narrow transparent-mask temp-bitmap path, but stopped sampling the destination HDC with `GetPixel`; transparent draws now use the destination background color from the DC state.
Why: Live run after the dialog freeze revert created `new 2` and then froze during tab repaint. The risky remaining change in the tab/ImageList path was synchronous destination readback from a real window HDC during paint.
Verify: Live run `reports/phase-h/npp-x64-20260521-184638` still froze after a short time; user reports freeze is time-based and independent of action.
Status: superseded-by-2026-05-21-18:53-revert-transparent-imagelist-background

## 2026-05-21 18:53 — Revert transparent imagelist background change
File(s): engine/wine/dlls/comctl32/imagelist.c:1766
Type: REVERT
What: Restored the legacy transparent-mask temp bitmap initialization to black instead of any destination/background color sampling.
Why: Time-based UI freeze has higher priority than the tab black-tile artifact. The transparent ImageList draw path is broad and repaint-driven, so remove it from the stability equation before continuing visual fixes.
Verify: pending rebuild/install plus live idle/new-tab responsiveness check by user.
Status: applied-pending-onscreen-verify

## 2026-05-21 18:58 — Disabled toolbar grayscale from default image
File(s): engine/wine/dlls/comctl32/toolbar.c:821
Type: ROOT-FIX
What: Replaced the always-colorful disabled fallback with a toolbar-local renderer: draw the default image to a sentinel 32bpp DIB, convert changed pixels to a premultiplied gray silhouette, and alpha-blend that for disabled buttons when no disabled imagelist exists.
Why: CrossOver/upstream behavior is stateful: enabled buttons are colorful, disabled Save/Undo/macro buttons are visible gray silhouettes, and state changes should redraw through the normal enabled path.
Verify: pending rebuild/install plus live empty-doc and edited-doc toolbar check by user.
Status: applied-pending-onscreen-verify

## 2026-05-21 20:40 — Step0 stock comctl32 oracle experiment
File(s): engine/wine/dlls/comctl32/*
Type: DIAGNOSTIC
What: Saved the current comctl32 diff to `reports/stock-comctl32-step0-20260521-204000/current-comctl32.diff` and temporarily restored tracked baseline comctl32 sources for a stock/baseline DLL test on the current HyperBridge runtime.
Why: CrossOver/upstream Wine renders the Notepad++ toolbar/dialog icons correctly; testing baseline comctl32 in our runtime localizes whether the remaining toolbar/folder/tab bugs live in our comctl32 patch stack or deeper in win32u/winemac/HyperBridge.
Verify: Rebuild `reports/rebuild-stock-comctl32-step0-20260521-204549.log`; live run `reports/phase-h/npp-x64-20260521-205048` still froze and still showed incomplete Open-dialog icons. User screenshot at 21:00 confirms the issue survives baseline comctl32.
Status: deeper-than-comctl32

## 2026-05-21 17:24 — 32bpp pattern brush row bounds
File(s): engine/wine/dlls/win32u/dibdrv/primitives.c:873
Type: ROOT-FIX
What: In `pattern_rects_32`, bounded the no-AND-mask pattern copy by the actual DWORDs available in the brush row (`brush->stride / 4`) instead of `brush->width`.
Why: The scrollbar audit identifies the black square as a 1bpp checker/pattern brush being applied on a 32bpp target; using pixel width as a DWORD count can overread into zero mask storage and copy black pixels into the scrollbar track.
Verify: Live `--hold` launch `reports/phase-h/npp-x64-20260521-173928` opened but the window became unresponsive.
Status: reverted-by-2026-05-21-17:46-scrollbar-selected-track-patcopy

## 2026-05-21 17:46 — Scrollbar selected track PATCOPY
File(s): engine/wine/dlls/win32u/dibdrv/primitives.c:873, engine/wine/dlls/user32/scroll.c:148
Type: REVERT, ROOT-FIX
What: Reverted the generic `pattern_rects_32` row-bound experiment after it made the live window unresponsive, and changed the classic scrollbar selected-page track paint to `PATCOPY` instead of the pattern/invert path.
Why: User confirmed scrolling functionality works and the remaining issue is only the black painted square. Avoiding the selected-track pattern ROP removes that visible artifact without changing scroll behavior or touching the generic DIB pattern engine.
Verify: pending rebuild/install plus live scrollbar paint check by user.
Status: applied-pending-onscreen-verify

## 2026-05-21 22:42 — HyperBridge XMM PACK saturation family
File(s): engine/hyperbridge/src/hb_decode_x64.c:1370, engine/hyperbridge/src/hb_lift_x64.c:320, engine/hyperbridge/src/hb_interpreter.c:2014, engine/hyperbridge/tests/hb_test_runner.c:5854
Type: ROOT-FIX
What: Added decode/lift/interpreter coverage for the SSE2 XMM PACK saturation family: `PACKSSWB`, `PACKUSWB`, and `PACKSSDW`. Kept upstream Wine `comctl32` as the validation target instead of restoring rendering band-aids.
Why: Pure upstream Wine 11.0 `comctl32` still produced black/missing toolbar/dialog icon pixels under HyperBridge while CrossOver/Rosetta renders the same app correctly, localizing the icon bug below Wine. `PACKUSWB` was absent from HyperBridge and is the final signed-word-to-unsigned-byte saturation step used by alpha/DIB blit paths; semantics were cross-checked against sse2neon (`vqmovn`/`vqmovun` PACK equivalents).
Verify: `engine/hyperbridge/tests/hb_test_runner` after forced rebuild: 171 passed, 0 failed. Rebuilt `libhyperbridge.a`, relinked and codesigned `dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so`.
Status: applied-pending-onscreen-verify

## 2026-05-22 00:59 — HyperBridge blit SIMD batch trace verdict
File(s): engine/hyperbridge/include/hb_decoder.h, engine/hyperbridge/include/hb_ir.h, engine/hyperbridge/src/hb_decode_x64.c, engine/hyperbridge/src/hb_lift_x64.c, engine/hyperbridge/src/hb_interpreter.c, engine/hyperbridge/tests/hb_test_runner.c
Type: ROOT-FIX, DIAGNOSTIC
What: Completed the requested x64 blit SIMD batch after the PACK fix: `PADDUSB`, `PAVGB/PAVGW`, `PMULLW/PMULHW/PMULHUW`, `PMADDWD`, `PSHUFB`, plus explicit tests for the existing XMM `MOVDQA/MOVDQU` move paths. Added env-gated `MACRUNNER_HB_TRACE_SIMD` tracing for XMM/vector IR execution.
Why: `reports/engine-audit/HYPERBRIDGE-SIMD-AUDIT.md` flagged this family as missing/high-risk for icon and alpha blit paths. Semantics were checked against Intel SDM and sse2neon equivalents (`vqadd`, rounded average, multiply/madd, `vqtbl`-style shuffle, XMM load/store).
Verify: `engine/hyperbridge/tests/hb_test_runner` after forced rebuild: 174 passed, 0 failed. Rebuilt `libhyperbridge.a`, relinked and codesigned `dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so`; `scripts/verify-build-freshness.sh` passed. Live traces `reports/phase-h/npp-x64-20260522-002712` and `reports/phase-h/npp-x64-20260522-005947` showed no unsupported/fault lines and did not execute the new arithmetic/shuffle ops; active translated path is CRT vector memory code (`MOVD`, `PUNPCK`, XMM `LOAD/STORE` via `MOVDQA/MOVDQU`).
Status: batch-applied-tests-pass-visual-unchanged-evidence-shifts-to-vector-memory-or-lower-render-layer

## 2026-05-22 03:58 — HyperBridge vector-memory dataflow trace
File(s): engine/hyperbridge/src/hb_interpreter.c:1078
Type: DIAGNOSTIC
What: Added gated `MACRUNNER_HB_TRACE_SIMD_DATA` pre/post data dumps for selected XMM/vector IR operations. The trace records XMM register bytes plus source/destination guest memory bytes for `MOVD`, `PUNPCK`, XMM `LOAD`, and XMM `STORE`, with optional guest-PC range filters (`MACRUNNER_HB_TRACE_SIMD_DATA_GUEST_START/END`) and a bounded line budget.
Why: The SIMD batch made missing arithmetic/shuffle opcodes non-issue, but live icons stayed black. The next required question was whether active vector-memory ops corrupt bytes silently or whether data is already zero before SIMD/memory copy.
Verify: `engine/hyperbridge/tests/hb_test_runner`: 174 passed, 0 failed. Rebuilt `libhyperbridge.a`, relinked/codesigned `dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so`, and `scripts/verify-build-freshness.sh` passed. Filtered live run `reports/phase-h/npp-x64-20260522-035848` over guest range `0x140419500..0x140419d00` produced `load_pairs ok=186 bad=0` and `store_pairs ok=232 bad=0`; every XMM load copied source memory into XMM exactly and every XMM store wrote the source XMM bytes exactly. Zero output stores had zero `src2/xmm0` before the store, so the byte loss is not in `MOVDQA/MOVDQU` load/store translation.
Status: vector-memory-cleared-evidence-points-before-x64-simd-or-native-gdi-icon-layer

## 2026-05-22 05:12 — Icon resource source-pixel trace
File(s): engine/wine/dlls/user32/cursoricon.c:853
Type: DIAGNOSTIC
What: Added env-gated `MACRUNNER_TRACE_ICON_SOURCE` / `MACRUNNER_TRACE_VISUAL_BATCH` logging for icon resource color bits and indexed 4/8bpp palette expansion, before `SetDIBits`/`StretchDIBits` fill the icon DIB.
Why: Runtime `win32u` DIB traces showed `dibdrv` copies source pixels to destination without zeroing them; the remaining question is whether icon pixels are already black/zero at resource decode or become black during DIB fill. This trace answers the upstream side of that split without restoring comctl32/user32 rendering workarounds.
Verify: Rebuilt `dlls/user32/x86_64-windows/user32.dll` and `dlls/user32/aarch64-windows/user32.dll` with the project llvm-mingw/ccache environment; installed dist copies contain `macrunner-icon-source`. Runtime trace `reports/phase-h/npp-x64-20260522-053703/stderr.log` produced 4 `resource_color` samples: 64/48/32/16px all had colorful/nonwhite/alpha pixels before DIB fill. The paired DIB trace showed `dibdrv` copy preservation (`preserved=25 bad=0`), so `StretchDIBits`/`SetDIBits` fill is not the first zero point for this PE icon path.
Status: source-and-fill-cleared-next-trace-imagelist-or-shell-dialog-path

## 2026-05-22 18:57 — GDI shared table init before desktop bootstrap

Scope: source/init boundary, not a rendering patch.

Evidence: stock `gdi32!SelectObject` faulted at `rva=0x33804` (`0x3940392a`, `ldrb w10, [x9,#0xe]`) because `PEB->GdiSharedHandleTable` was still `NULL`. Diagnostic caller mapping showed `user32!create_icon_frame` calling `SelectObject` with both `hdc` and `obj` equal to `0xc0000005`; the status came from early `CreateCompatibleDC` during desktop/display-driver bootstrap before `win32u:gdi_init()` had run.

Fix: move `gdi_init()` before `shared_session_init()` in `win32u:init_user()`, so cursor/icon loading during early desktop bootstrap has a valid GDI shared handle table.

Verify: rebuilt clean `win32u.so` and `gdi32.dll`; `scripts/verify-build-freshness.sh` passed. Clean control run `reports/phase-h/npp-x64-20260522-185707` had `native_faults=0` and `SelectObject(c0000005,c0000005)=0`. Remaining `failed to create desktop window` / server queue errors are a separate desktop bootstrap blocker.

## 2026-05-22 06:08 — ImageList first-zero pixel trace
File(s): engine/wine/dlls/comctl32/imagelist.c:31, engine/wine/dlls/comctl32/imagelist.c:178, engine/wine/dlls/comctl32/imagelist.c:305, engine/wine/dlls/comctl32/imagelist.c:495, engine/wine/dlls/comctl32/imagelist.c:1431
Type: DIAGNOSTIC
What: Added env-gated `MACRUNNER_TRACE_IMAGELIST` / `MACRUNNER_TRACE_VISUAL_BATCH` pixel summaries for ImageList ingest and draw: source `HBITMAP`, optional mask, image-list storage after add, stored slot before draw, composed temporary bitmap, and destination HDC after final blit.
Why: The user32 icon-resource trace proved PE icon bits are colorful before DIB fill, and the win32u/dibdrv trace proved `StretchDIBits`/`SetDIBits` preserve those bits. The next unknown is whether color disappears at the common-controls ImageList boundary or later during draw.
Verify: pending rebuild/install and one bounded Notepad++ run with `MACRUNNER_TRACE_IMAGELIST_BUDGET` routed to `RUN_DIR/stderr.log`; parse with grep/script only, not `cat`.
Status: diagnostic-added-pending-build-runtime-trace

## 2026-05-22 07:08 — ImageList trace env gate fix
File(s): engine/wine/dlls/comctl32/imagelist.c:181
Type: DIAGNOSTIC
What: Changed the new `MACRUNNER_TRACE_IMAGELIST` gate from `GetEnvironmentVariableA()` to `getenv()`, matching the existing working visual traces in `comctl32/tab.c`.
Why: Fresh comctl32 DLLs contained the trace strings, but runtime produced zero `macrunner-imagelist` lines while other env-gated traces worked. The neighboring comctl32 visual trace uses `getenv()`, so the diagnostic gate was the blocker, not evidence that ImageList was inactive.
Verify: pending rebuild and bounded Notepad++ trace.
Status: diagnostic-gate-fixed-pending-build-runtime-trace

## 2026-05-22 09:05 — HyperBridge REP MOVS string-copy family
File(s): engine/hyperbridge/include/hb_decoder.h, engine/hyperbridge/include/hb_ir.h, engine/hyperbridge/src/hb_decode_x64.c, engine/hyperbridge/src/hb_lift_x64.c, engine/hyperbridge/src/hb_interpreter.c, engine/hyperbridge/tests/hb_test_runner.c
Type: ROOT-FIX
What: Added the x64 `MOVS` string-copy family (`A4`/`A5` with optional `F3`/`F2` repeat prefix) to decoder, IR, lifter, interpreter, and regression tests. The interpreter copies byte/word/dword/qword units from `RSI` to `RDI`, honors `RFLAGS.DF`, updates `RSI`/`RDI`, and drains `RCX` for repeat-prefixed forms.
Why: The active stock-Wine icon/dialog path still loses pixels after source-resource and DIB fill traces proved data is colorful and preserved. `rep movs` is a common CRT/icon/ImageList copy primitive and was absent from HyperBridge, so missing string-copy execution can leave destination image buffers zeroed without implicating Wine patches or cache state.
Verify: pending HyperBridge test run, forced `libhyperbridge.a` rebuild, forced `ntdll.so` relink/codesign, `scripts/verify-build-freshness.sh`, and one bounded Notepad++ visual/icon run. If unchanged, the next opcode family must come from `reports/hyperbridge-gaps/NEXT-CODEX-ORDER.md`, not manual guessing.
Status: applied-pending-build-runtime-verify

## 2026-05-22 10:12 — Fast validation hb_test_runner hookup
File(s): engine/hyperbridge/tests/hb_test_runner.c, tools/hb_oracle/fast_validate_family.sh
Type: DIAGNOSTIC
What: Added `hb_test_runner --fast-family rep_movs|string_ops` and changed `fast_validate_family.sh` to build/run that real HyperBridge test path instead of generating placeholder oracle payloads and synthetic trace files.
Why: AGENTS now requires the acceleration pipeline after every family fix. The previous scaffold could report `available_todo_hook` and fake PASS data without executing HyperBridge semantics, which is not acceptable for root-cause opcode work.
Verify: `tools/hb_oracle/fast_validate_family.sh string_ops` returns `FAST VALIDATION: PASS`, writes `reports/hyperbridge-validation/LATEST-FAST-VALIDATION.{md,json}`, reports `hb_test_runner: pass`, and FileCheck passes 2/2 on the runner trace.
Status: applied-fast-validation-hook-live

## 2026-05-22 10:27 — HyperBridge CMPS/LODS string-op family
File(s): engine/hyperbridge/include/hb_decoder.h, engine/hyperbridge/include/hb_ir.h, engine/hyperbridge/src/hb_decode_x64.c, engine/hyperbridge/src/hb_lift_x64.c, engine/hyperbridge/src/hb_interpreter.c, engine/hyperbridge/tests/hb_test_runner.c, tools/hb_filecheck/examples/string_ops_cmps_lods.check, tools/hb_oracle/fast_validate_family.sh
Type: ROOT-FIX
What: Added `CMPSB/W/D/Q` (`A6/A7`) and `LODSB/W/D/Q` (`AC/AD`) with optional `F3`/`F2` repeat prefixes to decode, IR, lift, interpreter, tests, and fast validation. `CMPS` compares `[RSI] - [RDI]`, materializes SUB/CMP flags, advances `RSI/RDI` by `DF`, and implements `REPE/REPNE` ZF stop rules. `LODS` loads `[RSI]` into `AL/AX/EAX/RAX`, advances `RSI` by `DF`, and drains `RCX` for repeated forms.
Why: `reports/hyperbridge-gaps/NEXT-CODEX-ORDER.md` names `STRING-OPS-CMPS-LODS-CODEX-BRIEF` as the next family after REP MOVS. This closes the adjacent string runtime family without guessing the next opcode manually.
Verify: `./scripts/test-hyperbridge.sh` passed with 185 C tests, 0 failed. `tools/hb_oracle/fast_validate_family.sh string_ops` passed with `hb_test_runner: pass` and FileCheck 3/3. Forced `libhyperbridge.a` rebuild and `ntdll.so` relink/codesign; `scripts/verify-build-freshness.sh` passed. Manual hold run `reports/phase-h/npp-x64-20260522-105540` remained visually unchanged per user screenshot: toolbar/folder/dialog black-icon artifacts still present.
Status: tests-pass-runtime-visual-unchanged-next-family-from-next-codex-order
## 2026-05-22 10:58 — Lock clean upstream Wine render baseline
File(s): engine/wine/dlls/comctl32/imagelist.c, engine/wine/dlls/comctl32/toolbar.c, engine/wine/dlls/user32/cursoricon.c
Type: REVERT, DIAGNOSTIC
What: Restored `imagelist.c` and `toolbar.c` from `reports/upstream-wine-11.0-reference/comctl32/` and restored `cursoricon.c` from the Wine 11.0 upstream reference cached at `reports/upstream-wine-11.0-reference/user32/cursoricon.c`.
Why: Current visual runs were ambiguous because Wine render files were mixed between old workaround patches and stock code. The icon investigation now needs a stable stock-Wine baseline so remaining black/missing icons can be attributed to HyperBridge/lower-layer behavior instead of residual comctl32/user32 changes.
Verify: Rebuilt/installed comctl32/user32 with the canonical llvm-mingw + ccache-prefixed build environment; `scripts/verify-build-freshness.sh` passed. `dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so` was reinstalled by `make install` and re-signed with `codesign --force --sign -`.
Status: stock-baseline-locked-ready-for-pinpoint

## 2026-05-23 01:18 — Visual gate evidence and P0 TEB64 carrier hardening
File(s): tools/visual_regression/gate_notepad.py, scripts/visual-gate-notepad.sh, reports/phase-h/MILESTONE/smoke-tools/npp_ui_smoke_helper.c, reports/phase-h/MILESTONE/smoke-tools/npp_open_saveas_helper.c, engine/wine/dlls/win32u/win32u_private.h, engine/wine/dlls/win32u/winstation.c, engine/wine/dlls/win32u/gdiobj.c, engine/wine/dlls/ntdll/unix/unix_private.h, engine/wine/dlls/ntdll/ntdll_misc.h
Type: DIAGNOSTIC, ABI-HARDENING
What: Reclassified `marlett_center` as SKIP when the captured region is the native macOS titlebar, made the Save/Open dialog helper path readiness-gated, removed the remaining 120s dialog-hold clamp, and added P0 guard logic so WOW TEB64 derivation uses `WowTebOffset` and verifies it matches the `GdiBatchCount` carrier before TEB64/PEB64 use.
Why: On-screen captures show stock-Wine toolbar icons are real black/gray square artifacts (`toolbar_render_cg black_ratio=0.5911 colorful_pixels=0`), not disabled-state gray. Folder-icon proof is blocked earlier by command/menu dispatch: Notepad++ reports `cmd_open=41002`, but `NPPM_MENUCOMMAND`, `WM_COMMAND`, sync sends, focus attach, and `Ctrl+O` all fail to create the Open/Save dialog.
Verify: Rebuilt and installed `win32u.so` and `ntdll.so` into `dist-pure-arm64`, signed both ad-hoc, and `./scripts/verify-build-freshness.sh` passed. Post-P0 UI smoke kept editor/menu inventory alive but still failed the visual/menu blockers: `menu_alt_file_input FAIL`, `menu_mouse_file_input FAIL`, `toolbar_render_cg FAIL`, `client_black_bands FAIL`; clean exit passed.
Status: p0-carrier-guard-applied-visual-command-dispatch-still-blocked
## 2026-06-08 13:28 — Loader native-entry allowlist for winmm/ucrtbase
File(s): engine/wine/dlls/ntdll/loader.c
Type: ROOT-FIX
What: Completed the ARM64 host / x64 main native-entry allowlist by adding `winmm.dll` to the existing `win32u.dll`/`kernelbase.dll`/`ucrtbase.dll` path so Wine internal ARM64X builtins that require native initialization run their native entry instead of being skipped or sent through the x64 runner.
Why: Audio lane evidence named `winmm/ucrtbase` as the remaining native-entry blocker; `ucrtbase.dll` was already in the in-progress loader fix, while `winmm.dll` was missing from the narrow allowlist.
Verify: `clang -fsyntax-only` passed for `aarch64-windows`, `x86_64-windows`, and `i386-windows` loader targets. Direct `make -C engine/wine .../loader.o` was blocked before code compilation by local toolchain state (`aarch64-w64-mingw32-clang` missing and `x86_64-w64-mingw32-gcc` rejecting clang-style flags).
Status: syntax-validated-ready-for-coordinator-merge-loader-not-touched-after-fix

## 2026-06-09 17:13 — ARM64X normalized callback native dispatch
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c
Type: ROOT-FIX
What: In `macrunner_hb_dispatch_x64_callback`, classify the post-normalize target against the metadata-bearing ARM64X module and call executable ARM64X native targets directly instead of routing them into the x64 PE fallback.
Why: Hollow Knight normalized `ntdll.dll` callback `0x87fffa170c0` to ARM64 native entry `0x87fff9f2a60`, but the residual already-normalized target could still miss the old `target != original_target` guard and re-enter fallback.
Verify: Built `make -C /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine -j$(sysctl -n hw.ncpu) dlls/ntdll/ntdll.so`, copied to `/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`, and ad-hoc codesigned. One HK run with absolute `DIST` and `HK`, `WINEDEBUG=+virtual,+seh`, `MACRUNNER_HB_TRACE_CALLBACK_ROUTE=1` produced `wine-process-primary=1`, `dispatch-fallback=0`, `normalize-trace=80`, `NtUserCreateWindowEx=0`; remaining blocker is post-direct signal loop on target `0x87fff9f2a60`.
Status: fallback-fixed-new-post-direct-signal-blocker

- 2026-06-09 23:45 Lane A HK: fixed ARM64EC PE `user_shared_data` initializer in `dlls/ntdll/thread.c`; native `NtGetTickCount` was reading unmapped low `0x7ffe0320` while Wine mapped USD at high `0x7ffe0000000`.

- 2026-06-09 23:56 Lane A HK: fixed ARM64EC `set_security_cookie` early-loader seed to avoid `GetCurrentProcessId()` x18/TEB deref while loader is still bootstrapping.
2026-06-10 00:09 · Lane A HK: low-PE x64 Ldr entry now routes by AMD64 executable-section match instead of fragile PE env reread in ntdll/signal_arm64.c.
2026-06-10 00:18 · Lane A HK: ARM64 loader now treats an already-loaded AMD64 main image as HyperBridge-required by machine evidence, avoiding env-only false gate.
2026-06-10 00:24 · Lane A HK: LdrInitializeThunk now falls back to AMD64 main image AddressOfEntryPoint when X0/PC are non-executable, preserving strict executable-section validation.
2026-06-10 00:30 · Lane A HK: added bounded Ldr no-thread-entry marker for image machine/AEP diagnostics before NtContinue.
 · Lane A HK · added bounded loader_init phase-probe to classify primary pre-Ldr stall · next: one ntdll build/deploy + one HK run
2026-06-10 00:52 · Lane A HK · added bounded loader_init phase-probe to classify primary pre-Ldr stall · next: one ntdll build/deploy + one HK run
2026-06-10 00:54 · Lane A HK · loader phase-probe compile-fix: guard CONTEXT register logging by __arm64ec__/native arm64 · rebuild ntdll
2026-06-10 01:12 · Lane A HK · root-cause fix: segv_handler no longer logs macrunner-hb-signal-entry through Wine ERR while x18=0; this avoids recursive __wine_dbg_output x18 fault before loader_init · rebuild/run next
2026-06-10 01:31 · Lane A HK · diagnostic: primary normalized Ldr maps to signal_arm64ec.c, so added bounded ARM64EC LdrInitializeThunk markers around context_arm_to_x64/loader_init · rebuild/run next
2026-06-10 01:43 · Lane A HK · root-cause fix: init_syscall_frame no longer raw-returns x64 guest to normalized ARM64EC Ldr; it records pending_x64 entry and returns to x64 thunk so earlyinit HB fallback owns ARM64EC setup · rebuild/run next
2026-06-10 01:55 · Lane A HK · fix update: earlyinit pending_x64 fallback no longer requires !raw_is_guest/!sigill; first depth=0 x64 Ldr thunk fault uses recorded entry before x16 dispatcher can steal routing · rebuild/run next
2026-06-10 02:07 · Lane A HK · fix update: pending fallback now stores original ARM64 CONTEXT from init_syscall_frame, synthesizes AMD64_CONTEXT from it, clears pending context after use, and forces route PC to pending entry before x16 can override · rebuild/run next

## 2026-06-10 02:19 Lane A pending raw-x64-entry
- signal_arm64.c forced pending x64 entry now keeps raw x64 thunk PC instead of post-normalizing to ARM64 native, preserving HB-owned ARM64EC entry sequencing.

## 2026-06-10 02:36 Lane A preserve raw x64 callback dispatch
- Evidence: `reports/phase4-hollow-knight/laneA-pending-raw-x64-entry-20260610-022020/run.log` showed `MacRunner Phase F using pending ... hb_pc=0x87fff9f0500`, then `macrunner-hb-callback-dispatch target=0x87fff9c7418 original=0x87fff9f0500`; lower dispatch re-normalized the forced raw x64 entry to ARM64 native.
- Fix: add one-shot preserve-raw flag from `signal_arm64.c` pending route into `macrunner_hb_dispatch_x64_callback`, plus ARM64X metadata-based x64 guest classification for raw ARM64X x64 code.

## 2026-06-10 03:01 Lane A ARM64EC entry context/tag alignment
- Evidence: preserve-raw run kept `target=0x87fff9f0500` and `dispatch-pe-fallback=0`, but never reached `macrunner-hb-ldr-after-loader-init`. Static bytes at the x64 entry thunk are `... e9 0a 6f fd ff`, a tail jump to ARM64EC native `LdrInitializeThunk+1`; PE source expects the original ARM64 context and calls `context_arm_to_x64()`.
- Fix: pending route now preserves the saved ARM64 init context in x0/RCX for the ARM64EC entry thunk, and direct-native dispatch clears ARM64EC bit0 before branching to native ARM64 code.

## 2026-06-10 03:15 Lane A callback-run probe
- Evidence: armctx/tag-align run preserved the saved ARM64 context and raw x64 target, with `dispatch-pe-fallback=0` and no pc0, but stopped after stack allocation/translation-cache-open without `LdrInitializeThunk` marker.
- Probe: add bounded `macrunner-hb-callback-run` stages for `x64-signal-callback` under `MACRUNNER_HB_TRACE_CALLBACK_ROUTE` to identify whether hang occurs before ABI, before first block run, inside runtime, or after native transition.

## 2026-06-10 03:30 Lane A native ARM64EC entry call
- Evidence: callback-run probe showed `stage=after-run block=1 ... next_pc=0x87fff9c7418` after 5 x64 thunk instructions, then timeout before another block or loader marker. The raw x64 entry thunk only tail-jumps to native ARM64EC `LdrInitializeThunk`.
- Fix: pending raw callback dispatch now detects this ARM64X native entry and calls it via `macrunner_hb_call_arm64_pe_import12_on_stack` with the saved ARM64 init context and `arm_ctx->Sp`, instead of JIT/interpreting the x64 thunk and falling into native bytes.

## 2026-06-10 03:44 Lane A native entry stack pointer
- Evidence: native-entry run reached `macrunner-hb-callback-native-entry` with `stack_top=arm_ctx->Sp`, then timed out before any `LdrInitializeThunk`/loader marker. `macrunner_hb_arm64_pe_call12` stores outgoing stack args at SP, so top-of-stack is the wrong scratch pointer.
- Fix: pending native entry call now uses the allocated signal/caller stack pointer (`arm64_stack_args` / saved x1) as explicit stack, while keeping `arm_ctx->Sp` only as diagnostic fallback.

## 2026-06-10 03:58 Lane A PE Ldr entry probe
- Evidence: native-stack run reached `macrunner-hb-callback-native-entry` with signal stack, but still timed out before `macrunner-hb-ldr-after-loader-init`.
- Probe: add PE-side `macrunner-hb-ldr-entry` and `macrunner-hb-ldr-after-xlat` markers in ARM64EC `LdrInitializeThunk` to distinguish call-entry failure from pre-loader context conversion/loader hang.
## 2026-06-12 22:55 — Preserve original AMD64 exec ranges for builtin x64 DLLs

Lane D added an original-PE executable section side-table for HyperBridge:
- `engine/wine/dlls/ntdll/unix/loader.c` registers original AMD64 executable sections from `nt_descr` before Wine rewrites builtin module headers into synthetic `.text/.data`.
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c` stores those ranges and makes `macrunner_hb_pc_in_executable_section()` prefer them for registered modules.
- `engine/wine/dlls/ntdll/unix/unix_private.h` declares the registration hook.

Reason: HK x64 DXMT was spinning before DXGI markers because DXGI builtin runtime headers reported `.text sec_size=0xbaef6`, making disk `.rdata` strings at RVAs like `0x15cc0/0x16040` look executable to HyperBridge. Disk PE `.text` is only `0x13bd6`; original exec ranges should prevent data-as-code in builtin x64 DLLs.

Validation: `ntdll.so` builds and is ad-hoc signed in `dist-arm64ec-spike`; final HK run is pending because `x18waittrace2` currently owns HK/mr-run.

## 2026-06-13 07:10 — Lane D D3D9 fixed-function fog headless path
File(s): engine/graphics/d3d9_to_d3d11.py, engine/graphics/runtime_backend/mock_executor.py, engine/graphics/runtime_backend/metal_executor.py, engine/graphics/traces/runtime_samples/d3d9_fixed_function_fog_runtime.jsonl, engine/graphics/tests/test_d3d9_translation.py, engine/graphics/tests/test_d3d9_metal_request.py, engine/graphics/scripts/run_d3d9_dxmt_headless_smoke.sh, engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh
Type: GRAPHICS-COVERAGE
What: Added D3D9 fixed-function fog metadata and headless validation. `D3DRS_FOG*` now records `d3d9_fog_state`; the mock backend applies linear/exp/exp2 fog RGB before output-merger blending; Metal request payloads expose `d3d9.fog_state`.
Why: D3D9 headless coverage had alpha/depth/cull/scissor/sampler/combiner paths but no fog coverage, while fog is part of the RenderWare-era fixed-function slice.
Verify: `python3 -m pytest engine/graphics/tests/test_d3d9_translation.py engine/graphics/tests/test_d3d9_metal_request.py -q` -> `61 passed`; `bash engine/graphics/scripts/run_d3d9_dxmt_headless_smoke.sh` -> `d3d9_trace_count=32`, PASS; `D3D9_METAL_REQUEST_ONLY=1 bash engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh` -> `d3d9_metal_request_count=32`, PASS.
Status: headless-validated

## 2026-07-02 04:21 — Lane A loader-init exit=5 locale EC mirror/frontier
File(s): engine/wine/dlls/ntdll/signal_arm64.c, engine/wine/dlls/kernelbase/locale.c
Type: ROOT-FIX + DIAGNOSTIC-SAFETY
What: Scoped PE first-chance Unity IAT dump to real `UnityPlayer.dll` modules with sufficient `SizeOfImage`, so tracing no longer dereferences fixed Unity offsets in small DLLs. Corrected kernelbase locale EC mirror delta from stale `0x2820` to current twin-block delta `0x27d8` and added sort pointer verification logging.
Why: The quiet loader-init `exit=5` after `kernelbase` DllMain was a native `c0000005` read at `kernelbase.dll!GetStringTypeW+0xa0` (`rva=0x366b4`, `ldrh w11,[x12,x11,lsl#1]`, `fault=0`). `nm` showed native/EC `sort` blocks at `0x1522b0` and `0x14fad8`; the old mirror copied 0x48 bytes before the EC `sort`, leaving `sort.ctype_idx` NULL.
Verify: Built/deployed `ntdll.so`, PE `aarch64-windows/ntdll.dll`, and `aarch64-windows/kernelbase.dll` into `dist-arm64ec-spike`. Filtered classify run `reports/phase4-hollow-knight/laneA-exit5-locale-delta-verify-20260702-041609` reached `LADDER_RUNG: 6 (mono-init)` with self-check PASS; old `GetStringTypeW` native fault and loader-init exit=5 are gone. New blocker is `mono-2.0-bdwgc.dll rva=0x385e73` memory fault / `WINDOW_SERVER_ERROR c000007b`.
Status: loader-init-cleared-next-mono-runtime-fault

## 2026-07-02 08:40 — Lane A Mono init HB exec registration
File(s): engine/wine/dlls/ntdll/unix/virtual.c, engine/wine/dlls/ntdll/unix/unix_private.h
Type: ROOT-FIX
What: Reconnected HyperBridge executable-memory registration to current-process `NtAllocateVirtualMemory`, `NtAllocateVirtualMemoryEx`, and `NtProtectVirtualMemory` success paths. Born-exec `PAGE_EXECUTE_*` allocations now register immediately; protect-to-exec transitions register against the containing valloc allocation.
Why: After the locale fix, HK reached Mono and failed before window creation. Raw logs showed the derived `WINDOW_SERVER_ERROR c000007b` label was wrong: the status came from HB runtime failure, not NtUser/window server. The June WIP-era Mono/JIT exec-registration fix had not survived in `virtual.c`; without these calls, anonymous Mono exec pages rely on later dynamic-exec side effects instead of the strict guest exec gate.
Verify: Built/deployed/codesigned `ntdll.so`. Filtered HK classify `reports/phase4-hollow-knight/laneA-mono-standard-env-try1-082507` reaches `LADDER_RUNG: 8 (gfxdevice)` with two `macrunner-hb-exec-memory-register event=alloc` lines and no Mono runtime fault. Forced diagnostic `MACRUNNER_HB_JIT_DIRECT_MEM=1` still reproduces a separate direct-mem JIT fault at `mono-2.0-bdwgc.dll+0x385e73` (`cmpw %r8w,(%rbx)`, `rbx=0xffffffff01000166`), so direct-mem remains disabled for the mainline gate.
Status: mono-init-cleared-next-gfxdevice

## 2026-07-02 09:24 — Lane A DXMT smoke kernelbase locale registry mirror
File(s): engine/wine/dlls/kernelbase/locale.c
Type: ROOT-FIX + RUNTIME-DEPLOY
What: Extended the ARM64X locale EC mirror to include registry handle statics and the full `entry_*` localized registry cache family, including `entry_sintlsymbol`.
Why: Fast DX11 smoke with real dxmt prefix sync failed before DXMT code. Raw signal was `kernelbase.dll+0x59930` BUS read fault `fault=0x3e000002da`, `esr=0x92000005`, `insn=0x794002e8`; `pefile+capstone` disassembled this as `RegSetKeyValueW+0x8c: ldrh w8, [x23]`, reading an invalid `subkey` pointer. The previous locale mirror explicitly excluded `entry_*` registry caches; this is the reached sibling class.
Verify: Rebuilt/deployed `aarch64-windows/kernelbase.dll` to `engine/wine/dist` and `dist-arm64ec-spike`. Direct dxmt `mr-run` smoke `reports/research/laneA-dx11-mrrun-direct-after-kernelbase-20260702-092009.log` exits `0` with real `CreateDXGIFactory1`, `D3D11CreateDevice`, `Present`, `Present1`, and `pixel_readback=PASS`; old `kernelbase+0x59930` fault is gone.
Status: fast-dx11-smoke-cleared-next-hk-rung9

## 2026-06-13 08:10 — Lane D Unity#2 DXBC corpus and D3D8 RenderWare fog matrix
File(s): engine/graphics/scripts/run_unity_dxbc_airconv_corpus_smoke.sh, engine/graphics/traces/runtime_samples/d3d8_renderware_fog_runtime.jsonl, engine/graphics/tests/test_d3d9_translation.py, engine/graphics/tests/test_d3d9_metal_request.py, engine/graphics/scripts/run_d3d9_dxmt_headless_smoke.sh, engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh
Type: GRAPHICS-COVERAGE
What: Added a generic Unity DXBC corpus smoke and validated AI War 2 as the second Unity target after unpacking its GOG/Inno installer into `artifacts/ai-war2-unity-corpus/extracted/AIWar2_Data`. Added a D3D8 RenderWare fog trace so the fixed-function fog path is pinned through the D3D8 facade and Metal request contract.
Why: Lane D needed non-HK Unity shader corpus coverage plus the next RenderWare/GTA VC matrix increment in owned headless scope.
Verify: `run_unity_dxbc_airconv_corpus_smoke.sh aarch64` -> AI War 2 `blobs=2 translate=2/2 render=2/2`; `python3 -m pytest engine/graphics/tests/test_d3d9_translation.py engine/graphics/tests/test_d3d9_metal_request.py -q` -> `63 passed`; `bash engine/graphics/scripts/run_d3d9_dxmt_headless_smoke.sh` -> `d3d9_trace_count=33`, PASS; `D3D9_METAL_REQUEST_ONLY=1 bash engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh` -> `d3d9_metal_request_count=33`, PASS.
Status: headless-validated

## 2026-07-02 10:22 — Lane A rung-9 snapshot and GLM DXMT deploy probe
File(s): engine/graphics/dist/dxmt/* runtime payload, artifacts/milestone-dist/hk-rung9-dxgi-factory-20260702-100320
Type: RUNTIME-DEPLOY + DIAGNOSTIC
What: Created the first restorable verified HK rung-9 DXGI-factory snapshot, then copied GLM/graphics-prep DXMT runtime DLLs into the current DXMT dist overlay. Only runtime payloads were copied; the graphics-prep worktree was not edited.
Why: GLM's staged DXMT build contains the OPTIONS4 / CheckFeatureSupport `E_INVALIDARG` class fix, and HK's post-device-fence silence could have been a caps-query bailout before `CreateSwapChain`.
Verify: Hash report `reports/research/dxmt-glm-options4-deploy-20260702-100554.sha256.txt` records source/dest hashes (`source_head=8400e1e`, `main_head=fbb6b25`). HK GLM runs did not reach `CreateSwapChain`: the 300s run reached real `CreateDXGIFactory2` but no device fence/swapchain/present; the 420s confirmation run timed out earlier with no real DXGI boundary. Samples point to CPU-bound HyperBridge memory-protect sync and display-driver `KeUserModeCallback` transport, not DXMT caps queries.
Status: GLM-DXMT-did-not-advance-HK-next-HB-display-callback-transport

## 2026-07-02 12:08 — Lane A warm translation cache and HB perf cleanup
File(s): scripts/laneA-run-hk.sh, engine/hyperbridge/src/hb_memory.c, engine/hyperbridge/src/hb_arm64_codegen.c
Type: PERF-FIX + DIAGNOSTIC-HARNESS
What: Made HK runs warm by default with `MACRUNNER_HB_TRANSLATION_CACHE=1`, added explicit `MACRUNNER_HK_COLD_RUN=1`, and scoped the default cache root by deployed `ntdll.so` sha16. Enabled real D3D boundary and DXGI swapchain evidence markers in the HK wrapper. Avoided full region-tree rebuilds for permission-only `hb_memory_protect` calls, and cached disabled HK trace env gates inside the JIT helper.
Why: `laneA-run-hk.sh` had forced cold translation for every HK run, confounding GLM-vs-preGLM DXMT A/B and adding large variance. The persistent block key does not include the engine binary/codegen hash, so a shared unscoped cache would be a wrong-code hazard after rebuilds. Warm profiles then exposed HB bookkeeping costs: `hb_memory_protect -> rebuild_region_tree -> tree_insert` and repeated disabled diagnostic `getenv()` checks in `hb_jit_helper_exec_ir_block_once`.
Verify: Built/deployed/codesigned `ntdll.so` (`e8e0fabee9b97498bcf15c4accdacf410c513853fead276b7d759841b0123c2f`) and used cache root `artifacts/hb-translation-cache/ntdll-e8e0fabee9b97498`. Warm pre-GLM and GLM behavior matched, so GLM DLLs are not the regression. After the memory fix, sample `tree_insert` terms dropped from 540/787 to 130/205; after JIT env caching, latest warm sample `getenv` terms dropped to 17-22. Filtered evidence run `reports/phase4-hollow-knight/laneA-postjit-warm-evidence-glm-try1-120655` classifies as rung 9 (`D3D11_CREATE_DEVICE_MISSING`, `real_factory=2`, self-check PASS). True cold-no-cache baseline `reports/phase4-hollow-knight/laneA-cold-nocache-glm-try1-121112` classifies as rung 9 `PRESENT_MISSING` with self-check PASS (`real_factory=2`, `d3d11_device_markers=1`, `swapchain=0`, `present=0`). Populate run on the same closure reached raw `macrunner-dxmt-fence` but no `CreateSwapChain`/`Present`.
Status: warm-default-enabled-HB-hotspots-reduced-next-post-DXGI-stall

## 2026-07-02 13:06 — Lane A HK HWND to CAMetalLayer binding
File(s): engine/wine/dlls/winemac.drv/window.c, engine/wine/dlls/winemac.drv/d3dmetal.c, engine/wine/dlls/winemac.drv/macdrv.h, engine/dxmt/src/winemetal/unix/winemetal_unix.c
Type: ROOT-FIX + RUNTIME-DEPLOY
What: Added a winemac on-demand `win_data` realization helper for D3D swapchain creation before normal `WindowPosChanging`, reattached the D3DMetal client surface after realization, and added a full `winemetal[HWND]` binding field trace.
Why: HK reached real `IDXGIFactory2::CreateSwapChainForHwnd(hwnd=0x20054)` but failed because the macdrv HWND binding path had no realized `win_data`/Cocoa window at swapchain time. `hwnd=0x20054` is same-thread, root/top-level, desktop-parented Unity window; returning a fake orphan layer would have hidden all frames.
Verify: Built/deployed `winemac.so` (`11956cf41ee60909`) and `winemetal.so` (`ff02feed8ac54e17`). Warm HK run `reports/phase4-hollow-knight/laneA-hk-hwnd-bind-fix-125609-try1-125711` logs `d3d_on_demand result=0x72aa417a0 cocoa=0x73cd88000`, `client_cocoa_view=0x72aa20f00 ret_view=0x72aa31b80 ret_layer=0x764179770 attached_to_hwnd=1`, and `CreateSwapChainForHwnd rc=0x0 swapchain=0xedcb05c90`. Filtered classify reports `LADDER_RUNG: 11 (swapchain)`.
Status: hwnd-binding-cleared-next-post-swapchain-timeout

## 2026-07-02 13:48 — Lane A DXGI Present classifier de-noise
File(s): tools/triage/analyze_d3d_gate.py
Type: DIAGNOSTIC-FIX
What: Excluded `macrunner-hb-dxgi-swapchain: candidate method=Present` lines from real Present detection.
Why: Factory slot 8 is `MakeWindowAssociation`, and the generic unknown-object candidate trace was inflating `present_count` even though no real swapchain Present occurred.
Verify: Reclassified `reports/phase4-hollow-knight/laneA-hk-glm-latest-postswap-long-132919-try1-132920`; filtered D3D counts are now `swapchain=2 present=0`, with `LADDER_RUNG: 11 (swapchain)` and `PRIMARY_CLASS=PRESENT_MISSING`.
Status: present-marker-false-positive-removed

## 2026-07-02 18:02 — Lane A Unity+0x2b5605 race/init-order discriminator
File(s): engine/hyperbridge/src/hb_arm64_codegen.c
Type: DIAGNOSTIC-FIX + ROOT-CAUSE-NARROWING
What: Diagnosed the one-shot `UnityPlayer.dll+0x2b5605` post-swapchain fault as descriptor corruption upstream of the consumer load (`mov rsi,[rdx]` at `+0x2b5587`), not a mapping/protect fault. Fixed the HB diagnostic store-drain knob so `MACRUNNER_HB_JIT_DIRECT_STORE_FENCE=1` covers offset/fused direct stores as well as the generic direct-store path.
Why: The fence A/B shifted pre-swapchain odds, but the codegen knob was incomplete: offset stores such as Unity's `[rsi+0x40]` and event-container `[r8+0x628]` did not get the requested post-store `DMB ISH`. The producer walk showed the bad `rsi` came from descriptor field `[rdx]`; the normal targeted trace consumed `{0,0,1}` and passed `+0x2b5605`.
Verify: HyperBridge rebuilt; forced Unix `ntdll.so` relink/deploy/codesign (`48566904f9b703831d90bd6d5903e79694a1156fdd3f52b8cba6449057fbaa4a`). Broad `hb_test_runner` remains not clean in this workspace (`446-449 passed`, `20-23 failed`, mostly code-size/host-perm cases). Patched fence coverage runs `laneA-laneA-fencecov-populate-20260702-172446-try1-172501` and `laneA-laneA-fencecov-warm-20260702-173252-try1-173337` both classify rung 11 `PRESENT_MISSING`: real `CreateSwapChainForHwnd rc=0`, no GetBuffer/RTV, no runtime-fail, no `+0x2b5605`.
Status: rung11-preserved-post-swapchain-wall-not-solved-by-direct-store-fence

## 2026-07-02 15:14 — Lane A HK post-swapchain HB memory bookkeeping
File(s): engine/hyperbridge/src/hb_memory.c, engine/wine/dlls/ntdll/unix/macrunner_hb.c
Type: PERF-FIX + DIAGNOSTIC
What: Added a narrow `UnityPlayer.dll+0x2b5605` address-class dump for HB/virtual/Mach mappings, then removed the hot full-region-tree rebuild from split/protect bookkeeping. Split-created right fragments are inserted into the existing region treap immediately; `hb_memory_protect` no longer rebuilds the full tree after metadata-only permission changes. The second sampled hotspot, `split_all_regions_at`, now uses the region tree lookup for the normal non-overlap invariant instead of scanning every region.
Why: The raw post-swapchain fault was one-shot in the prior GLM run, but samples during the post-device/post-swapchain window showed HB infrastructure burning CPU in `macrunner_hb_sync_virtual_region -> hb_memory_protect`, first under `rebuild_region_tree -> tree_insert`, then under `split_all_regions_at`.
Verify: Built/deployed/codesigned `ntdll.so` (`a12a7823b7071c248d3bac578255a18ad90bc0d85abcdd29233d8ab90e0ea70a`). Filtered classifier on `reports/phase4-hollow-knight/laneA-laneA-hk-addrclass-splitfast-20260702-150106-try1-150106` still reports real `CreateSwapChainForHwnd` (`rc=0`, `hwnd=0x20054`), with no `GetBuffer` or real Present. Samples after the first fix show `rebuild_region_tree/tree_insert` gone; splitfast sample no longer contains `split_all_regions_at`/tree rebuild terms. The old `UnityPlayer+0x2b5605` fault did not replay in the instrumented reruns, so no HB/Mach address-class dump was captured yet.
Status: hb-bookkeeping-hotspots-reduced-rung11-preserved-next-getbuffer-or-replay-unity-fault

## 2026-07-02 18:18 — Lane A ABZU import bridge fixes
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c
Type: ROOT-FIX
What: Imported ABZU's ARM64 PE import FP marshalling fix so guest XMM0-XMM7 are mirrored into native q0-q7 on PE import calls and native q0 is written back to guest XMM0. Removed the synthetic COM apartment semantic handler so `CoInitialize*` and `CoUninitialize` execute Wine's real COM implementation instead of returning fake success.
Why: ABZU proved double-returning CRT imports such as `wcstod` returned `0.0` because the bridge only copied integer `RAX`, and proved fake `CoInitialize=S_OK` left `NtCurrentTeb()->ReservedForOle` empty so real `CoCreateInstance` failed `CO_E_NOTINITIALIZED`.
Verify: Forced Unix `ntdll.so` rebuild/deploy/codesign succeeded from this source closure (`2cc696d858cbb25c4441aff7c7d599a83cd911a600fc278e70ca9f5ea6cc5b73`). ABZU handoffs: `reports/abzu/rebaseline-20260702-rsi-producer/FP-RETURN-CLOSURE.md`, `COM-APARTMENT-CLOSURE.md`.
Status: applied-next-HK-post-swapchain-resample

## 2026-07-02 18:51 — Lane A Mono fusion module gate
File(s): engine/hyperbridge/include/hb_context.h, engine/hyperbridge/src/hb_arm64_codegen.c, engine/hyperbridge/src/hb_runtime.c, engine/wine/dlls/ntdll/unix/macrunner_hb.c
Type: ROOT-FIX
What: Added a per-block `HB_CONTEXT_CODEGEN_MONO_MODULE` flag set by the Wine bridge from the LDR owner of `block_pc`, and made Mono metadata/string JIT fusions require that flag. Bumped the persistent JIT cache version to 18 so pre-gate fused blobs cannot be reused.
Why: ABZU proved the Mono metadata/string fusion patterns can match non-Mono code (`windowscodecs.dll+0xDED8` on a UE4 path), so pattern-only emission was unsafe outside `mono-2.0-bdwgc.dll`/`mono.dll`.
Verify: HyperBridge rebuild plus forced Unix `ntdll.so` relink/deploy/codesign succeeded (`7638362cb8fc593269ca4f46eb7566cf47e1035703f94eae8d276bf9e12bedc7`).
Status: applied-awaiting-ABZU-regression-recheck

## 2026-07-02 21:55 — Lane A HK wait-handle identity trace
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c
Type: DIAGNOSTIC
What: Added env-gated wait-handle tracking for HB-routed `CreateEvent*`, `CreateSemaphore*`, `ReleaseSemaphore`, `Set/Reset/PulseEvent`, `WaitForSingleObject*`, and `CloseHandle`, plus a post-`CreateSwapChainForHwnd` wait trace bypass with its own budget.
Why: HK's rung-11 wall is a wait after swapchain creation; the old wait trace named only raw handles and exhausted its budget before the post-swapchain window.
Verify: Built/deployed/codesigned diagnostic `ntdll.so` twice: wait-handle baseline `b8b7d4978dd1bf707026a19e24c94786443e5a17945a7b248c1b836a0776c887`, then post-swapchain-budget build `519e95fb72156ff006f15eb7f2d35234365b4ef77d9f37edde4e103cd2a5c595`. Filtered classify on `reports/phase4-hollow-knight/laneA-wait-handle-20260702-211155-try1-211257` remains rung 11 with `PRESENT_MISSING` and secondary `WAIT_DEADLOCK`.
Raw verdict: The durable waits in that run are not the DXGI frame-latency waitable and no `GetFrameLatencyWaitableObject` marker appears. The parked handles are Unity-created semaphores: `0x70..0xb8` from `CreateSemaphoreExW(initial=0,max=2147483647)` and `0x110` from `CreateSemaphoreW(initial=0,max=2147483647)`. No `ReleaseSemaphore` targets those handles in the captured run.
Status: diagnostic-only-no-behavioral-fix-yet

## 2026-07-02 22:09 — Lane A HK swapchain-creator wait/message trace
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c, docs/ACTIVE-INVESTIGATION.md
Type: DIAGNOSTIC
What: Extended the HK wait trace to record both guest and native thread ids for the thread that creates the DXGI swapchain, mark waits from that same thread, and log user32 message-pump/window-visibility imports (`GetMessage*`, `PeekMessage*`, `WaitMessage`, `MsgWaitForMultipleObjects*`, `ShowWindow`, focus/active/foreground calls, and sent/posted/dispatch messages) with the swapchain hwnd context. User32 message tracing is separately gated by `MACRUNNER_HB_TRACE_USER32_MESSAGE` so it can run without the full `MACRUNNER_HB_TRACE_ABI` flood.
Why: The previously captured seven never-released semaphores are Unity job-system workers sitting idle, not the producer. The real wall must be named on the swapchain-creator/main-render thread after `MakeWindowAssociation`.
Verify: Built/deployed/codesigned diagnostic `ntdll.so` (`122ec18648af98e6e5826dc6ee8f38b96991d205542178647cc573e4cf6c7279`). Warm HK v2 run `reports/phase4-hollow-knight/laneA-swapchain-creator-wait-v2-20260702-222503-try1-222503` filtered as `PRESENT_MISSING` with `SELF_CHECK=PASS` and raw `CreateSwapChainForHwnd rc=0`, `swapchain=2`, `present=0` (ladder line still reports rung 9 despite swapchain evidence). Warm HK v3 sample `reports/phase4-hollow-knight/laneA-swapchain-creator-wait-v3-20260702-223734-try1-223734/sample-postswap-creator-pinned-224340.txt` captured creator guest `tid=0x64`, native `0x2bfe358` / `Thread_46130008`: no `NtWaitForSingleObject`, `GetMessage`, or `WaitMessage` on the creator; hot stack is `macrunner_hb_call_import_thunk -> macrunner_hb_try_kernel32_handle_semantic -> macrunner_hb_sync_virtual_region -> hb_memory_protect`. Trace-only rebuilds may reuse a previous populated translation cache root; codegen/translation-semantics rebuilds still require a fresh root.
Status: verified-producer-thread-cpu-active-in-hb-protect-sync

## 2026-07-02 23:20 — Lane A HK sync semantic kill-switch
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c, docs/ACTIVE-INVESTIGATION.md
Type: DIAGNOSTIC
What: Added env-gated bypasses for the wait-address helper and for all or only synchronization-shaped imports handled by `macrunner_hb_try_kernel32_handle_semantic`. The gates print `macrunner-hb-semantic-bypass` lines so A/B runs prove which shortcut path was disabled.
Why: HK's swapchain creator sampled under `NtWaitForAlertByThreadId` and `macrunner_hb_try_kernel32_handle_semantic`, matching the same smell as the removed fake COM shortcut. The decisive discriminator was to route sync operations away from the HB semantic shortcut and compare the warm frontier.
Verify: Built/deployed/codesigned diagnostic `ntdll.so` (`6fb3483b0e660fb2f2c0eb3c3435c7f428edecbf674dc4011a7fa45827a959e9`). Kill-switch run `reports/phase4-hollow-knight/laneA-sync-killswitch-20260702-225918-try1-225918` bypassed `kernel32-handle=39843` and `wait-address=53` calls, but filtered classify stayed rung 11 `PRESENT_MISSING` with real `CreateSwapChainForHwnd rc=0`, no `GetBuffer`, and no real Present. Baseline wait-address run `reports/phase4-hollow-knight/laneA-waitaddr-baseline-20260702-231255-try1-231255` shows `WakeByAddressSingle` finding and alerting the waiter (`addr=0x3a0414cb0`, `tid=00f4`, waiter caller `0x87efce9ee71`, waker caller `0x87efce9eebb`), so alert delivery is not swallowed for the observed path.
Status: sync-shortcut-hypothesis-refuted-next-hb-special-memory-sync

## 2026-07-02 23:55 — Lane A HK in-flight import trace
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c, docs/ACTIVE-INVESTIGATION.md
Type: DIAGNOSTIC
What: Added `MACRUNNER_HB_TRACE_INFLIGHT_IMPORT` entry/return tracing around `macrunner_hb_call_arm64_pe_import12_for_ctx`, including seq id, guest/native tid, dispatch kind, `module!function`, target, guest PC/return address, status, and args0-11. The default gate logs only the swapchain creator thread; `MACRUNNER_HB_TRACE_INFLIGHT_IMPORT_ALL=1` enables full-process tracing.
Why: The previous sample stack showed the swapchain creator inside our import-dispatch machinery. The correct discriminator is not CrossOver's internal wait, but which guest-observable native import is in flight and whether it actually returns.
Verify: Built/deployed/codesigned final diagnostic `ntdll.so` (`5b637d73740031b4a2e88f9ddf0e530203da787b3b4609488cf8fc2a3da18f1f`). Evidence run `reports/phase4-hollow-knight/laneA-inflight-import-20260702-233300-try1-233300` reached rung 11 on warm cache: real `CreateSwapChainForHwnd rc=0`, no `GetBuffer`, no real Present. Sanitized filtered classify is `PRESENT_MISSING`, self-check PASS. Raw in-flight parse found no unmatched creator import at timeout; creator `tid=0x24` / `Thread_46229431` loops through returning SRW imports (`ReleaseSRWLockExclusive`, `AcquireSRWLockExclusive`, `TryAcquireSRWLockExclusive`, shared acquire/release). Sample shows creator CPU-active in `hb_jit_runtime_run` with special read/write Mach calls, not parked in `NtWaitFor*`; direct `pe_call12` executes on the same native thread via `blr target`, not an executor thread.
Status: nonreturning-import-refuted-next-special-memory-srw-throughput

## 2026-07-03 01:12 — Lane A HK special-memory throughput and directmem gates
File(s): engine/wine/dlls/ntdll/unix/macrunner_hb.c, engine/hyperbridge/src/hb_arm64_codegen.c, engine/hyperbridge/src/hb_runtime.c, docs/ACTIVE-INVESTIGATION.md
Type: DIAGNOSTIC / GATE
What: Added `MACRUNNER_HB_TRACE_SPECIAL_ACCESS_SAMPLE` to summarize swapchain-creator special read/write PCs and addresses without flooding logs. Added explicit codegen gates for directmem isolation: `MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN=0` now overrides `JIT_DIRECT_MEM=1`; new `MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM=0` disables scalar GPR direct-memory lowering while leaving XMM direct-memory separately controlled. Bumped persistent JIT cache version to 19 and added the scalar-mem flag to the cache key.
Why: HK's post-swapchain creator thread is CPU-active in guest JIT work, not waiting. The needed split was progress-vs-spin and which direct-memory subfamily reopens the Mono `c000007b` fault under `JIT_DIRECT_MEM=1`.
Verify: Built/relinked/deployed/codesigned `ntdll.so` after the final gate patch (`6531b51a7439d85d60f6e3f22419f4fbeb124ca9143185146426eeed5f673e4c`). Baseline sampler `reports/phase4-hollow-knight/laneA-special-sample-baseline-20260703-000016-try1-000016` reached swapchain then timed out with no `GetBuffer`; sampler showed PC/addr/page uniqueness overflow and advancing ranges, so this is real work crawling, not a spin loop. `MACRUNNER_HB_DIRECT_MEM=1` run `laneA-special-directmem-ab2-20260703-002219-try1-002219` removed almost all copy syscalls (`read_direct_ok=85646969`, `read_mach=7246`, `write_direct_ok=17245545`, `write_mach=240`) but still timed out before `GetBuffer`. `JIT_DIRECT_MEM=1,NATIVE_MEM_IR=0` reproduced `mono-2.0-bdwgc.dll+0x385e73`; block trace proved the native fault is a direct scalar `CMP word [RBX],R8W` load (`ldrh`), not the qword native-mem IR path. With `MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM=0`, the Mono fault disappeared and the run reached real DXGI/D3D11 calls, but still timed out before swapchain due helper/mach throughput.
Status: scalar-directmem-fault-class-isolated-no-rung-advance

## 2026-07-03 12:22 — Lane A HK reproducible scalar-directmem default
File(s): engine/hyperbridge/src/hb_arm64_codegen.c, scripts/mr-run.sh
Type: REPRODUCIBILITY / GATE
What: Made `MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM` default to off unless `MACRUNNER_HB_JIT_DIRECT_MEM=1` is explicitly enabled, and made `mr-run.sh` export the safe default `MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM=0`.
Why: The rung-11 floor is only reproducible with the scalar direct-memory fault class gated off. A stale local `ntdll.so` rebuild (`fc1a6924...`) was mistaken for clean HEAD; a true clean rebuild produced a new signed `ntdll.so`, and the safe env reached filtered rung 11. The default runner must not silently re-enable the known `mono+0x385e73` scalar directmem class.
Verify: Clean HyperBridge + forced ntdll rebuild produced deployed `ntdll.so` `6fbf685b6c4373744b043f4fd9b4cc839ed03310eb223a87196d914727b8a5fa`. HK run `reports/phase4-hollow-knight/laneA-reconcile-head-clean-safe-try1-120422` with `MACRUNNER_HB_DIRECT_MEM=1`, `JIT_DIRECT_MEM=0`, `JIT_DIRECT_SCALAR_MEM=0`, `NATIVE_MEM_IR=0` reached filtered rung 11 (`CreateSwapChainForHwnd` marker present), no `GetBuffer`/RTV/Present.
Status: clean-head-rung11-reproduced-safe-default-landed

## 2026-07-03 18:37 — Lane A dispatch gates and HK direct-copy default
File(s): engine/hyperbridge/include/hb_context.h, engine/hyperbridge/include/hb_runtime.h, engine/hyperbridge/src/hb_context.c, engine/hyperbridge/src/hb_arm64_codegen.c, engine/hyperbridge/src/hb_runtime.c, scripts/laneA-run-hk.sh
Type: PERFORMANCE / REPRODUCIBILITY / GATE
What: Added default-off dispatch fast-path gates for `MACRUNNER_HB_SINGLE_LOOKUP`, `MACRUNNER_HB_BLOCK_CHAIN`, and `MACRUNNER_HB_INDIRECT_IC`, with cached runtime env lookups so the gates do not reintroduce the old hot-path environ-lock class. Kept block chaining in-tree but default-off with an explicit `UNSAFE: single-slot tail patch, needs chain-entry trampoline` warning, and moved chain metadata out of the hot cache-entry body. Made lane A default the safe `MACRUNNER_HB_DIRECT_MEM=1` direct-copy path.
Why: Dispatch/chaining work needs a reproducible safe floor before the chain-entry/trampoline redesign. Uncached gate checks regressed HK below the rung-11 floor; cached gates restored the verified behavior. Direct-copy is the safe special-memory throughput path and does not enable the known JIT native-memory fault classes.
Verify: Deployed `ntdll.so` `26e89535d2ace28b4ee552704ce263fd88135c82772c8ce97322aa013afd288b` reached rung 11 with flags off (`laneA-dispatch-noflags-d2f2-final-try1-173933`), with `MACRUNNER_HB_SINGLE_LOOKUP=1` (`laneA-dispatch-B-singlelookup-warm-try1-180223`), and with `MACRUNNER_HB_DIRECT_MEM=1` (`laneA-dispatch-directmem-noflags-try1-181344`). `MACRUNNER_HB_BLOCK_CHAIN=1` is not enabled: the current single-slot tail patch faults and needs the trampoline redesign.
Status: safe-dispatch-gates-landed-directmem-defaulted-block-chain-off

## 2026-07-03 21:48 — Lane A dispatch-rate counter
File(s): engine/hyperbridge/src/hb_runtime.c
Type: DIAGNOSTIC / PERFORMANCE-METRIC
What: Extended `MACRUNNER_HB_TRACE_DISPATCH_STATS` with a real `dispatches` counter and `dispatches_per_s`. The counter increments once per actual dispatch-loop native-block execution, independent of legacy block/step accounting and independent of whether `MACRUNNER_HB_SINGLE_LOOKUP` is enabled. Stats-only runs now stay on the legacy dispatch loop; the counter is emitted from both legacy and fast-path loops.
Why: HK A/B showed the existing heartbeat block/step counters are not comparable under `SINGLE_LOOKUP`; a separate dispatch-rate metric is needed before deciding whether the lookup fast path is a real throughput win. The first cut incorrectly made stats-only select the alternate fast-path loop, which regressed the baseline, so legacy emission is required for a clean A/B.
Verify: Forced `hb_runtime.o`/HyperBridge rebuild, targeted `ntdll.so` relink/deploy/sign produced `5320dc26341442b93197aee2d43245788f0cf77ac5f9be2107c8740132db731f`. Smoke run `laneA-dispatch-rate-legacy-smoke-try1-220510` emitted `macrunner-hb-dispatch-stats` lines with `total_dispatches` and `dispatches_per_s` under `SINGLE_LOOKUP=0`.
Status: dispatch-rate-counter-ready-for-singlelookup-ab

## 2026-07-03 22:42 — Lane A HK single-lookup default
File(s): scripts/laneA-run-hk.sh
Type: PERFORMANCE / DEFAULT-FLIP
What: Defaulted `MACRUNNER_HB_SINGLE_LOOKUP=1` in the HK lane wrapper and logged the effective value per run. `MACRUNNER_HB_SINGLE_LOOKUP=0` remains the explicit opt-out for double-lookup measurements.
Why: Corrected dispatch-rate A/B on deployed `ntdll.so` `5320dc26341442b93197aee2d43245788f0cf77ac5f9be2107c8740132db731f` preserved rung 11 and improved the real swapchain frontier.
Verify: `DIRECT_MEM=1,SINGLE_LOOKUP=0` run `laneA-dispatchrate2-dm1-900-try1-220803` reached `CreateSwapChainForHwnd rc=0` at 332.911s with pre-swap dispatch rate 0.924M/s. `DIRECT_MEM=1,SINGLE_LOOKUP=1` run `laneA-dispatchrate2-dm1-singlelookup-900-try1-222356` reached `CreateSwapChainForHwnd rc=0` at 150.172s with pre-swap dispatch rate 1.865M/s. Both stayed rung 11 with no GetBuffer/RTV/real Present.
Status: single-lookup-default-on-for-hk-lane

## 2026-07-03 23:08 — Lane A Fable NATIVE_MEMMOVE port
File(s): engine/hyperbridge/src/hb_arm64_codegen.c, engine/hyperbridge/src/hb_runtime.c
Type: PERFORMANCE / GATE
What: Ported Fable's default-off `MACRUNNER_HB_NATIVE_MEMMOVE` uCRT SSE2 memmove entry fast path. The codegen guard matches the memmove entry signature, calls helper id 31, and falls back to the original block when the helper declines. Bumped the persistent cache version to 20 and added a native-memmove cache-key flag.
Why: HK and ABZU are both JIT-throughput bound in string/memory-heavy startup work. The memmove fast path is isolated and default-off, so it can be A/B tested without changing the current rung-11 floor.
Verify: Forced `hb_arm64_codegen.o`/`hb_runtime.o`/HyperBridge rebuild, targeted `ntdll.so` relink, and deployed `ntdll.so` `1cf6f31c3da49da458042b90c1d9b9c37592bb07d01f077f230715d762b1bc98` over backup `artifacts/deploy-backups/20260703-230114-native-memmove/ntdll.so.before` (`9a955f2a86f7552fbeba8b472ca69098f3b57852f1d5b6a2ef1033411d4db3fc`). Default-off smoke `laneA-fable-native-memmove-off-smoke-try1-230152` reached filtered rung 11 (`CreateSwapChainForHwnd rc=0`), with no `GetBuffer`/RTV/real Present and no native-memmove hits.
Status: native-memmove-port-landed-default-off

2026-07-04 · ARM64X CodeMap-aware run_x64 in-image dispatch
- Added CodeMap type classifier for ARM64X ranges and used it in run_x64 in-image routing so type=2 x64/CHPE code is executable-translatable while type=1/type=0 are not decoded as x64 bytes.
- Validation: control smoke crash-clean; ABZU 900s closed nonexec/refuse/dxgi storm but still red on runtime c000007b=3 and no real D3D11CreateDevice.

## 2026-07-04 - HK Mono plateau region-fusion WIP

- Added default-off `MACRUNNER_HB_REGION_FUSION` superblock/region fusion path from UTFFUSION WIP for hot cross-block backedges.
- Narrowed Wine-side enablement to Mono modules for HK metadata/type-resolution plateau.
- Added rate-limited region-fusion reject diagnostics under `MACRUNNER_HB_TRACE_REGION_FUSION=1`.
- Rebuilt HyperBridge and relinked arm64ec-spike `ntdll.so`; floor is blocked until source is clean and driftcheck policy is satisfied.

2026-07-04 · JIT last_result per-block reset
- Cleared `ctx->last_result` before each native JIT block execution to prevent stale helper MEMORY_FAULT state from poisoning later control-only blocks.
- Validation: control smoke clean; ABZU c000007b/runtime_fail/jit_fail dropped to zero, but new host-side c0000005 surfaced in ntdll debug-string path before real D3D11CreateDevice.

## 2026-07-10 — Lane A ARM64X dual delay-import view normalization

File(s): `engine/wine/include/winnt.h`, `engine/wine/tools/winedump/pe.c`, `engine/wine/dlls/ntdll/loader.c`, `tests/hyperbridge/test_arm64x_delay_import_view.py`
Type: ROOT FIX / ARM64X LOADER
What: Corrected the ARM64EC V2 metadata field names used by Wine and winedump, then taught `LdrResolveDelayLoadedAPI` to select the complete adjacent ARM64X delay-IAT/INT sibling when a native thunk address is outside the descriptor-selected hybrid table. Selection is bidirectional and requires V2 metadata, in-image tables, matching counts, terminators, aligned slot membership, and nonzero auxiliary delay-table metadata.
Why: Hollow Knight's post-EH service thread reached `sechost!svcctl_OpenSCManagerW`, but native `__delayLoadHelper2` passed `sechost+0x34210` while the AMD64-selected descriptor named the adjacent IAT at `+0x34258`; the old resolver rejected the valid native slot before lookup and the common thunk branched through NULL.
Verify: Raw PE parsing and fresh `winedump -j loadcfg` agree on `CHPEMetadataPointer` load-config offset `+0xc8`, V2 metadata size `0x74`, `AuxiliaryDelayloadIAT` offset `+0x50`, and `AuxiliaryDelayloadIATCopy` offset `+0x54`. The hybrid sechost fixture has a 9-import rpcrt4 family with exact adjacent span `0x50`. Four focused regressions pass (metadata/real-PE layout, name sibling, ordinal sibling, invalid neighbor). Unix and PE ntdll builds pass; deployed arm64ec-spike SHAs are `c7b02ab0b07f0f84d10365fa2a97a440defb8325279a1957b6a432f43ac15477` and `95356460bf1e40180f420fb98e4bd8456781fc9b2c6fb71c8966c8ef59c9cd9c`.
Status: focused-build-and-tests-pass-awaiting-SCM-and-HK-runtime-verification

## 2026-07-10 — Hollow Knight post-OMSet thread-lifecycle probe

File: `engine/wine/dlls/ntdll/unix/thread.c`
Type: DIAGNOSTIC ONLY / DEFAULT-OFF
What: When the existing `MACRUNNER_HB_PRESENT_FOLLOW_PROBE` is enabled, emit one bounded lifecycle line at `abort_thread`, `NtTerminateThread` enter/result, `exit_thread`, and the final pthread exit.
Why: A valid post-fix HK run armed the creator follow probe at UnityPlayer+0x9054ff, executed exactly 128 blocks through the WM_IME_COMPOSITION callback, then produced no normal HB exit/fault marker. A native sample proved the exact creator pthread absent while the process and 52 other threads remained. The existing top-level `present-follow-exit` path was therefore bypassed; the next evidence boundary is Wine's actual thread termination layer.
Verify: Targeted Unix ntdll build passed; marker is present; codesign verification passed. Deployed Unix SHA is `5fc46eb6476d7cdeaf481c482aef7c278ca1e2889dc66e07ff602fc06edeaa20`; PE ntdll remains the ARM64X delay-IAT build `95356460bf1e40180f420fb98e4bd8456781fc9b2c6fb71c8966c8ef59c9cd9c`.
Run validation: The lifecycle run produced 19 records but none for creator `tid=0x24`; that thread remained live through 20,250,624 sampled blocks / +159.681s after OMSet. The trace correctly captured unrelated early exits and timeout cleanup. No semantic thread-lifecycle defect is evidenced.
Status: diagnostic-complete-no-lifecycle-fix; vanished-thread hypothesis disproved/nondeterministic

## 2026-07-10 — Hollow Knight producer vfunc58 TIME_ALIGN validation

File: no engine source change; existing default-off `MACRUNNER_HB_TRACE_RENDER_TIME_ALIGN` only
Type: DIAGNOSTIC VALIDATION / NO SEMANTIC PATCH
What: Ran exactly one 440s Hollow Knight probe with only TIME_ALIGN enabled. Producer tid `0x8000` returned from vfunc58 for item `0x34008a310`, published `item+0x40: 0 -> 1`, and consumer tid `0x3000` read 1 on the identical item within 113us.
Why: The Direct Codex handoff required classifying whether PreloadManager never enters, never returns, publishes a different item, or suffers a stale consumer read before any patch.
Verify: TIME_ALIGN=186 (consumer 134, producer 16, wait 36); exact `0x586d68=2`; semantic ready latch `seq=70744/70746`; same-item consumer observations=4, all `item40=1`; runtime faults=0; real Present1=0; pixel=0. Literal `0x586d65`/`0x586da3` sites did not emit, but adjacent before/after blocks captured return and publication.
Status: no A/B/C/D/E failure branch; load completion proven; no code change justified

## 2026-07-10 — Hollow Knight bounded post-ready consumer follow

Files: `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `scripts/validate-hk-post-ready-consumer-follow.sh`
Type: DIAGNOSTIC ONLY / DEFAULT-OFF / ZERO SEMANTIC MUTATION
What: Added `MACRUNNER_HB_POST_READY_CONSUMER_FOLLOW`, armed only by the same latched manager/item at consumer `UnityPlayer+0x587249` with a successful `item+0x40==1` read. It follows only that tid, logs at most 64 sparse samples, and stops at the first D3D/DXGI boundary, thread exit, 8M blocks, or 60s.
Why: Run 145409 proved producer publication and consumer coherence, moving the unresolved boundary to the consumer's natural post-ready continuation.
Verify: Focused static validation PASS; targeted Unix ntdll build PASS; deployed/codesigned Unix SHA `b5985258eca783e38e68c2772c937e669b03ecd1a1c9ea902839a8cf0dd4c465`; ARM64X PE ntdll unchanged `95356460bf1e40180f420fb98e4bd8456781fc9b2c6fb71c8966c8ef59c9cd9c`. Single run produced arm=1/sample=64/stop=1, TIME_ALIGN=0, first PC Unity+0x587289, then active Unity/Mono integration, real Present1=0, pixel=0.
Run finding: Separate tid 0x124 later proves `CRITICAL_SECTION_EX_FORCE_DEBUG_INFO_IGNORED`: the existing semantic fastpath discards Ex flags and leaves DebugInfo=-1, causing rpcrt4's required Spare[0] write to address 0x27. No semantic fix included.
Status: diagnostic-complete; next branch is critical-section initialization fastpath family audit/fix

## 2026-07-10 — Public critical-section FORCE_DEBUG family

What: Reworked the HB public `InitializeCriticalSection` / `InitializeCriticalSectionAndSpinCount` / `InitializeCriticalSectionEx` and Delete family so Ex validates its supported public/Wine-builtin flags, FORCE allocates and publishes a guest-writable canonical debug object only after checked HB writes, and Delete frees only the exact fastpath-owned allocation. Added a bounded default-off FORCE-init evidence marker and an x64 22-assertion public/direct-Rtl fixture.

Why: The immutable HK run proved rpcrt4 legally requested FORCE, then faulted at `rpcrt4+0x24945` because the semantic import path discarded flags and supplied `DebugInfo=-1`; the terminal STORE address was otherwise correct.

Verify: ntdll all-target build PASS; focused fixture `22 passed, 0 failed`; source/object/build/deploy freshness order verified; Unix and ARM64X codesign strict PASS. Deployed SHAs: Unix `b8183a007bc3ce2e17cfe2257d36e271117618918c0ed0678bd2e16c17c5f87f`, ARM64X `91a44392e38b9e370b092f4e55622e350473aac61c932e74b7580343cfc079b3`, x86_64 `f661dcb682d07826ea4c27dab41f00d86ccd2ad7f4373996825a915f1d4dfdae`.

Status: `FAMILY_FIX_BLOCKED_BEFORE_RUNTIME`. The mandatory full HB floor is `444 passed, 28 failed`, so the brief prohibited an HK run. No Present/pixel claim; no unrelated HB repair attempted.

## 2026-07-10 — HB floor-28 provenance (no engine change)

What: Performed a read-only provenance audit of the blocking full HB floor. Captured one exact current inventory (`443/29` after the recorded `444/28`), audited the HB/Wine dependency and timestamp boundary, and produced one detached clean-checkpoint baseline (`443/26`). No production source, semantic behavior, counter, threshold, expected value, test selection, build flag, deployment, Wine prefix, or game state was changed.

Why: The critical-section family brief stopped before runtime on a summarized 28-failure HB floor; exact provenance was required before attributing or repairing any failure.

Verify: All original 28 assertions recur in the current rerun; one additional REP MOVSQ result assertion moves under the unchanged current runner. Clean checkpoint `2ddb605f` shares 24 exact assertions with the original 28. HB binaries predate the Wine lane by about 7.5 hours, and its Makefile has no dependency on the lane's `macrunner_hb.c` or fixture. Temporary worktree removed; active HB hashes unchanged.

Status: `NONDETERMINISTIC_FLOOR`; zero `LANE_REGRESSION`, 24 `IDENTICAL_PREEXISTING`, four `UNCLASSIFIED` in the original 28. `FAMILY_FIX_BLOCKED_BEFORE_RUNTIME` remains; ABZU owns the next runtime slot.

## 2026-07-10 — Conditional HB differential waiver evaluation (no engine change)

What: Evaluated the completed floor provenance against every mandatory condition in the conditional critical-section runtime brief. No build, test, deploy, Wine, game, production source, threshold, expectation, or runtime state changed.

Verify: Required verdict is `PREEXISTING_IDENTICAL_FLOOR_DEBT`; actual verdict is `NONDETERMINISTIC_FLOOR`. Exact comparison is current `444/28` then `443/29`, clean baseline `443/26`, 24 shared, four current-only, two baseline-only, one adjacent-current rerun-only. Four current-only failures remain unclassified and one baseline-only failure lacks nondeterminism evidence.

Status: `WAIVER_CONDITIONS_NOT_MET`; conditional HK attempts=0, absolute HB floor remains red, historical `FAMILY_FIX_BLOCKED_BEFORE_RUNTIME` unchanged, NOT_GOLDEN/no tag.

## 2026-07-10 — Critical-section causal-isolation runtime (no engine change)

What: After an explicit causal-isolation waiver and ABZU V2 slot release, executed exactly one 540-second-bounded HK diagnostic comparison with existing FORCE-init, post-ready, dispatch/cache/hot-block, and D3D/DXGI evidence. No source/build/deploy/test, semantic mutation, forcing, ledger, retry, or extension occurred.

Why: The same `b4690115…` HyperBridge archive was linked into both the prior `b5985258…` diagnostic ntdll and current `b8183a00…` family ntdll, isolating the reviewed critical-section lane for one comparison despite the separately red/nondeterministic absolute floor.

Verify: Sole run exited rc 253 at +44.779s. FORCE-init=0 and prior rpcrt4/0x27=0 because the family boundary was unreached. A replacement `c0000005` repeated 22 times at `kernelbase+0x3a91c`, fault `0x88`; real factory/device/swapchain/OMSet/Present1/pixel all remained zero. Triage reports ladder regression to rung 1. Own prefix and duplicate overlay removed; zero scoped runtime leftovers.

Status: `CS_FAMILY_RUNTIME_REGRESSION`; Mono signal also insufficient, but the replacement fault is decisive. Absolute HB floor remains `NONDETERMINISTIC_FLOOR`, NOT_GOLDEN/no tag; stop before any new fault fix.

## 2026-07-10 — kernelbase+0x3a91c static triage (no engine change)

What: Performed static-only PE/pdata/xdata, source, disassembly, immutable-log, hash/route, critical-family, and signal/exception analysis of the replacement HK fault. No source, build, test, deployment, runtime, ABZU, timeout, semantic, or probe state changed.

Verify: Current PC is native ARM64X kernelbase `wcstombs_dbcs` line 3131, `ldrh w16,[x9,x16,lsl #1]`; LR is the `WideCharToMultiByte` return after `wcstombs_codepage`. All 22 SEGV records share PC/LR/EA `0x88` and descend by `0x1690`; static exception setup proves nested dispatcher re-entry before terminal callback-domain stack exhaustion. The prior run explicitly used a non-ARM64X kernelbase route; current route is ARM64X SHA `0dc7ba3a…020b`.

Status: `BINARY_ROUTE_DRIFT` with secondary `EXCEPTION_REENTRY_CORRIDOR_LOCALIZED`. Exact bad `WideCharTable`/WCHAR/codepage is unproven because the immutable record lacks GPRs. No patch authorized; one default-off one-shot pre-load owner/view probe is specified only for future review.

## 2026-07-10 — dist provenance before wcstombs (no engine change after supersession)

What: Stopped the in-progress wcstombs probe on supersession and preserved its already-edited but never-deployed state as a compact NOT_GOLDEN manifest/patch/checksum set. Then performed the requested read-only build/dist/reference provenance audit. No source restoration or further source edit, build, test, install, deployment, Wine, game, ABZU, commit, or tag occurred.

Verify: The active dist was broadly reinstalled at 16:25–16:27: all 4,701 regular-file mtimes cluster there. Current versus rung13 has 2,334 changed paths, although all 76 NLS files match byte-for-byte. Prior runtime explicitly reported `not-arm64x`; current kernelbase is ARM64X SHA `0dc7ba3a…020b`. `a29dd3c6…` is only the rung13 Unix ntdll identity, and no complete capture-time kernelbase-plus-all-ntdll manifest exists.

Status: `UNEXPECTED_DIST_DRIFT_PROVEN`; `CS_FAMILY_RUNTIME_CLOSURE_PROVEN=NO`, `FULL_RESTORABLE_FLOOR=NO`, `SAFE_TO_STAGE_SIDE_BY_SIDE=YES`. No Present1 or pixel claim. Stop before probe continuation or runtime.

## 2026-07-10 — pure-kernelbase stage release aborted (no engine change)

What: Verified the approved one-file checkpoint and release preconditions, then fed its fenced zsh recipe unchanged to the authorized isolated-stage command. The recipe failed its APFS guard before temporary helper creation, forced-clone invocation, stage creation, or runtime. No source, active dist, deployment, Wine prefix, ABZU state, commit, tag, semantic behavior, or timeout changed.

Verify: The stored awk expression uses double-escaped parentheses and returns empty; a read-only control parser reports `Type (Bundle)=apfs` for `/dev/disk3s1`. Stage and recursive manifest are absent. Active/candidate/payload hashes remain `0dc7ba3a…020b` / `23b420d2…62a8f` / `23b420d2…62a8f`; result-manifest checksums pass and post-abort host isolation is unchanged.

Status: `STAGE_MATERIALIZATION_ABORTED`; runtime wrapper attempts=0, HK attempts=0. No retry or recipe fix under this release, and no Present1/pixel/closure claim.

## 2026-07-11 — pure-kernelbase full-copy diagnostic (no engine change)

What: Materialized one temporary archive-preserving copy of the active Wine dist, proved exact inventory equality, replaced only staged kernelbase, proved exactly one SHA-only delta, and executed the sole authorized diagnostic runtime. No source, active dist, checkpoint, ABZU, semantic, timeout, commit, tag, golden, or floor state changed.

Verify: The runtime reached prefix sync, then failed at `__wine_unix_call_dispatcher_arm64ec not found` before kernelbase route selection or Mono. Wrapper/game rc=1; all requested fault, Mono, graphics, Present1, and pixel signals are non-reach. Canonical run log SHA is `2b9f5335…8f25b`.

Status: `FULLCOPY_DIAGNOSTIC_NO_REACH`. Prefix, 2.1-GiB stage, and inventory scratch removed; active kernelbase remains `0dc7ba3a…020b`; host released with no retry.

## 2026-07-11 — HK post-Mono discriminator static ready (diagnostic only)

What: Preserved V5 as DIAGNOSTIC_ONLY and statically mapped its terminal Unity log to x64 `UnityPlayer.dll` RVA `0x72f887`; the logger returns at `0x72f893`, and the exact resolver slot proves the next native Mono call is `mono_set_dirs` at `0x72fa29`. Added one confined default-off/atomic-one-shot observer in Unix ntdll's ARM64-host x64 run loop. It samples a fixed 262,144 translated-block window into a fixed 65,536-PC table and records per-module uniqueness, IR-cache hits/misses, quarter growth, last-new position, and top repetition with at most three success-path lines.

Safety: The enable chain is Unix-host `macrunner_hb_cached_env_flag` → libc `getenv`, not guest PEB state. The disabled path is guarded; the observer writes no guest/context memory, changes no runtime result/cache/timeout/readiness/DXMT/Metal semantics, and frees its bounded table on summary/normal exit.

Verify: Focused no-game structural unittest 11 passed/0 failed; Python syntax and owned whitespace checks pass. No build, deploy, Wine/game run, active-dist change, ABZU access, commit, tag, Present, or pixel claim. Exact source/test delta and decision matrix are sealed in `reports/phase4-hollow-knight/HK-POST-MONO-DISCRIMINATOR-STATIC-READY-20260711-NOT_GOLDEN/`.

Status: `HK_POST_MONO_DISCRIMINATOR_STATIC_READY`; future runtime requires separate serialization and exactly one bounded attempt.

## 2026-07-11 — HK post-Mono late-load hardening (static only)

What: Applied the sole optional hardening from the independently verified Opus pre-runtime audit: while the existing one-shot discriminator is armed and `mono_base` remains NULL, it re-resolves `mono-2.0-bdwgc.dll` at post-arm sample 1 and then every 4,096 samples, revalidates AMD64, and refreshes `mono_size` before classification. The unchanged 262,144-sample window bounds this to 64 post-arm retries (65 total Mono lookups including arm time).

Safety: Default-off/host-getenv gating, atomic claim, observer count, allocation, log budget, IR-cache accounting, guest state, timeout, cache behavior, and all runtime semantics are unchanged. No new log was added; a successful late transition uses the existing `mono-latched` record. The mandatory `armed mono=0x0` → Branch E, never B, rule remains authoritative.

Verify: Original 11 static checks plus 4 focused late-load cases pass (15/15); Python syntax, source/test whitespace, and exact-delta apply-check pass. No build, deployment, Wine/HK process, active-dist change, ABZU access, commit, tag, Present, or pixel claim.

Status: `HK_POST_MONO_DISCRIMINATOR_HARDENED_STATIC_READY`; compact hardening-only checkpoint at `reports/phase4-hollow-knight/HK-POST-MONO-DISCRIMINATOR-HARDENED-STATIC-READY-20260711-NOT_GOLDEN/` requires independent verification before separate runtime authorization.

## 2026-07-11 — HK post-Mono runtime preflight route mismatch (no engine change)

What: Re-ran the hardened 15-case static gate and the authorized fail-closed runtime preflight. All source/checkpoint, disk, prior-attempt, process-isolation, and PID99890 waiver gates passed, but the binary-route gate failed before backup/build: `unix/macrunner_hb.c` is an ARM64 Unix-nativized source linked only into Mach-O `dlls/ntdll/ntdll.so`; the explicitly authorized x64 PE `x86_64-windows/ntdll.dll` target neither depends on it nor contains the observer/gate strings.

Safety: No source edit, backup, build, deploy, prefix, Wine/HK process, active-dist mutation, signal, ABZU access, runtime attempt, retry, or extension occurred. Spending the sole run with the PE-only build would leave the observer absent.

Status: `PREFLIGHT_ROUTE_MISMATCH`; build/deploy/runtime attempts `0/0/0`. Compact evidence is sealed at `reports/phase4-hollow-knight/HK-POST-MONO-ONE-RUNTIME-PREFLIGHT-ROUTE-MISMATCH-20260711-NOT_GOLDEN/`; corrected authority must name the ARM64 Unix `ntdll.so` build/deploy path while leaving x64 PE ntdll unchanged.

## 2026-07-11 — HK post-Mono one diagnostic runtime (temporary build fully restored)

What: Under corrected authority, privately backed up the entire focused ntdll build subtree and dist Unix ntdll, focus-built/deployed only ARM64 Mach-O `ntdll.so`, proved its observer schemas/host gate/architecture/codesign and one-path dist drift, then ran the single 540-second-bounded HK attempt with explicit x64 loader and only the post-Mono discriminator flag added.

Result: The attempt exited naturally rc253 at +43.699s before the observer armed. Counts `armed/mono-latched/summary=0/0/0` and Mono path/config `0/0` classify once as frozen Branch A. Kernelbase base `0x87efe7e0000` repeatedly faulted at RVA `0x3a91c`/address `0x88` 22 times, then reached one c00000fd stack exhaustion. Triage counts were dxgi IAT/real factory `7/0`, d3d11 IAT/device `1/0`, swapchain/Present/Present1/pixel `0/0/0/0`; all downstream zeros are non-reach, not closure.

Restore: All 195 build entries and all 4,732 dist entries returned byte/size/mode/mtime_ns exact to pre-build inventory hashes; build/dist Unix ntdll and immutable x64 PE identities were restored and strict codesign passes. Prefix, backup, overlay, diagnostic cache, and non-waived runtime/build processes are absent.

Status: `HK_POST_MONO_ONE_DIAGNOSTIC_BRANCH_A_COMPLETE`, attempts 1/1, retries/extensions 0/0. Result: `reports/phase4-hollow-knight/HK-POST-MONO-ONE-DIAGNOSTIC-RUNTIME-20260711-NOT_GOLDEN/RESULT.md`. No semantic edit, ABZU touch, commit/tag, Present, pixel, or golden claim.

## 2026-07-11 — ARM64X locale data-view mirror family fix

What: Changed only the `kernelbase/locale.c` ARM64X data-copy mechanism so its helper takes an explicit delta. Classified the complete 57-object family from linked symbols: 24 core/NLS/codepage objects use `0x27e8`; `unix_cp` plus 32 registry-entry objects use `0x1b00`. The pure-image CHPE guard, call order, and non-copy semantics are unchanged.

Why: Static Branch-A/V5 proof showed the old universal `0x27d8` matched none of the 57 pairs. It shifted `ansi_cpinfo` by 16 bytes and copied `geo_ids_count=301` onto its header, displacing `WideCharTable` into the `DBCSOffsets` slot and causing the native `wcstombs_dbcs` fault corridor.

Tests: New focused structural/synthetic/pure/post-link suite passes 7/7 after the targeted ARM64X kernelbase build; all 57 linked destinations are exact and unique. Python AST, whitespace, and owned-patch reverse apply-check pass. The unrelated unchanged HB floor remains nondeterministic red at `442/30`, exactly the documented failure-set union.

Status: `HK_ARM64X_LOCALE_MIRROR_FAMILY_FIX_STATIC_READY`. Build-only checkpoint: `reports/phase4-hollow-knight/HK-ARM64X-LOCALE-MIRROR-FAMILY-FIX-FORENSIC-CHECKPOINT-20260711-NOT_GOLDEN/`. Active dist is unchanged; no deploy/runtime/ABZU/commit/tag or graphics truth claim.

### 2026-07-11 — ARM64X locale mirror family, one-runtime diagnostic

What: Materialized one throwaway full-copy dist and replaced only staged `aarch64-windows/kernelbase.dll` with the verified family build `893c6359…8c84`; launched one canonical 540-second HK attempt with explicit x64 loader and only the default-off owner/view diagnostic added. No source or active-dist file was changed.

Evidence: The loaded diagnostic emitted two process-attach family syncs with `core_delta=0x27e8 entry_delta=0x1b00` and four matching verification rows. Mono path/config reached `+38.860/+38.861s`; the prior `kernelbase+0x3a91c fault=0x88` corridor and `0xc00000fd` recursion were both zero.

Limitation: `macrunner-hb-wcstombs-owner-view` was zero. The eligible bounded owner/CPTABLEINFO/WideChar/DBCS boundary did not execute, so this run does not prove the locale defect closed. DXGI real factory, D3D11 real device, swapchain, Present/Present1, and pixel counts were all zero.

Status: Attempts 1/1, no retry authorized. Cleanup restored the exact active 4,732-entry inventory and removed the scoped stage/prefix/overlay/pointer. Result is `DIAGNOSTIC_ONLY / NOT_GOLDEN` at `reports/phase4-hollow-knight/HK-ARM64X-LOCALE-MIRROR-FAMILY-ONE-DIAGNOSTIC-RUNTIME-20260711-NOT_GOLDEN/`.

## 2026-07-11 — HK run-contract telemetry summary (static only)

What: Added measurement-only HyperBridge contract telemetry under the existing
`MACRUNNER_HB_TRACE_TRANSLATION_CACHE` path. The existing
`macrunner-hb-translation-cache-summary` line now has `open_ok`, `open_fail`,
`compile_count`, `translation_count`, `distinct_translation_count`,
`dispatches`, `blocks`, and `steps` in addition to existing cache counters.

Safety: No translation, cache, dispatch, signal, timeout, graphics, Wine/game,
A/A runtime, ABZU, commit, tag, or golden-claim behavior was authorized or run.
`MACRUNNER_HB_TRACE_DISPATCH_STATS` remains independent and is not required for
the contract summary.

Tests: `make -C engine/hyperbridge contract-telemetry-test` passed 4/4 focused
tests; `make -C engine/hyperbridge tests/hb_test_runner` linked; `make -C
engine/hyperbridge all` built `libhyperbridge.a` and `libhyperbridge.dylib`.

Status: `HK_CONTRACT_TELEMETRY_READY` pending separate run-contract A/A
authorization. Static report:
`reports/phase4-hollow-knight/HK-CONTRACT-TELEMETRY-STATIC-20260711-STATIC_ONLY-NOT_GOLDEN/`.

## 2026-07-11 — HK guest-PEB control observer (static only)

- Added a default-off, one-shot PE-side scan at `loader_init` immediately before main import fixup. The scan selects the finalized native/WOW64 PEB view, validates readable bounds and exact double-NUL termination, caps input at 1 MiB/8192 records, and emits only SHA-256 digests, UTF-16 lengths, and source ordinals through a fixed-width PE/Unix ABI.
- Added host-only activation controls excluded from both the Wine-created guest environment and the run-contract modeled environment. Publication is atomic/no-overwrite per process/view; partial, collision, wrong-view, and ambiguous-main states fail closed in the C7 collector.
- Static/Python verification: G0-G31 32/32 and retained identity-ledger 78/78. No product build, sanitizer build, control child, title, runtime, game, publication enablement, or ABZU access was performed. Verdict remains `HK_GUEST_PEB_CONTROL_OBSERVER_CONFORMANT_STATIC_BUILD_REQUIRED`.

## 2026-07-11 — HK guest-PEB focused build stopped at PE link

- Verified the independent Opus authorization and static checkpoint, live source 11/11, two clean pre-build host samples, 61.06–61.11 GiB headroom, zero prior observer attempts, and the untouched PID 99890 waiver.
- The single ccache-disabled focused ntdll build compiled the Unix publisher and x86_64 PE scanner objects, but the x86_64 PE link returned rc=2: Wine basename resolution selected Homebrew GCC 15.2.0, which rejected required clang flags `--no-default-config` and `-fms-hotpatch`.
- Fail-closed/no-retry authority stopped sanitizer, regression-floor, and control-child stages. No Wine/title/game, ENABLE, shard, canonical wiring, deploy, cache mutation, PID signal, or ABZU action occurred. Dist remained unchanged; final host samples were clean. Checkpoint: `reports/phase4-hollow-knight/HK-RUN-CONTRACT-GUEST-PEB-CONTROL-OBSERVER-20260711-BUILD_CONTROL_ONLY-NOT_GOLDEN/`.

## 2026-07-11 — HK guest-PEB control-name source-boundary repair

- Replaced the `wchar_t` `L""`/runtime-`wcslen` control table in `observer_control_name` with six explicit `WCHAR[]` arrays and compile-time `ARRAY_SIZE` lengths; case folding and exact `=` boundary semantics are unchanged.
- Test-first focused regression went RED on the old source and GREEN after the repair. The post-repair matrix passed identically under Apple clang plain O0/O1/O2, ASan+UBSan, and TSan: 18/18 control variants rejected, 8/8 non-controls accepted, and 18/18 privacy checks emitted nothing.
- G0-G31 passed 32/32, the identity floor passed 78/78, and the ccache-disabled repo llvm-mingw clang 22.1.5 focused ntdll compile/link passed. No install/deploy, Wine/control/title/game, A/A, cache mutation, or ABZU action occurred; control attempts remain zero.

## 2026-07-13 — HB critical-section family native-forward (Fix A, env-gated)

- Added `MACRUNNER_HB_CS_FORWARD_NTDLL=1` in `unix/macrunner_hb.c`. It bypasses the custom HB semantic for the complete Enter/Leave/TryEnterCriticalSection family, allowing generic native PE dispatch to use the kernel32/kernelbase spec forwarders to the single ntdll Rtl implementation and wait protocol.
- Initialize/Delete/SetCriticalSectionSpinCount and all unrelated semantics are unchanged. `tools/test_hb_cs_forward_source.py` statically guards all three siblings and both spec-forwarder families.
- Main ntdll.so build passed, SHA `3d7d65c1…`. One `NOT_GOLDEN` ABZU verify did not exercise the forward branch and failed earlier with terminal `c000007b` in a winemetal thunk; no claim about ABBA removal is made. Overlay was restored and no retry occurred.

## 2026-07-13 — x64 ntdll stack-probe semantic registration + validated dynamic-IAT recovery

- Previous fix: Fix A (`MACRUNNER_HB_CS_FORWARD_NTDLL`) could not be evaluated because Main fell through an unclassified `winemetal` IAT jump to an ARM64EC `___chkstk_ms` target. The same binary reproduced with Fix A off, proving Fix A was not causal.
- Evidence targeted: historical ABZU logs recovered `ntdll.dll!___chkstk_ms registered=0` from the exact `ff 25` block and resumed correctly; Main lacked `find_import_name_by_iat_jump` and its run_x64 branch. Loader source showed the EC target returned unregistered solely because the stack-probe family was not semantic and the target was non-executable.
- Primary repair in `loader.c`: all three ntdll spellings are semantic imports, so loader-time registration no longer depends on executable-section classification.
- Runtime contract in Unix HB: registered stack probes finish the x64 import as Wine's no-op while preserving RAX. A copied and bounded PE import-table/IAT recovery provides defense for late/dynamic bindings; it requires valid image ranges, `ff 25`, bounded descriptors/thunks, and slot-target equality. No module-specific exception exists.
- Family: x64 ntdll stack probes. Members: `___chkstk_ms`, `__chkstk_ms`, `__chkstk`. Coverage: loader registration + registered dispatch + validated dynamic fallback for all three. Tests: `tools/test_hb_dynamic_iat_recovery_source.py` and `tools/test_hb_cs_forward_source.py`, both PASS. Audit completed: yes.
- Not extending to: `__chkstk_arm64ec` or unrelated CRT helpers; neither is the evidenced x64 IAT family.
- Coherent build PASS: Unix ntdll `af774f21…`, ARM64X PE ntdll `a62ea85a…`; both matching halves were deployed together. ABZU verify is PARTIAL: Fix A hits and ABBA disappears, but the stack-probe/dynamic-IAT path is not reached because the game stalls earlier at a post-ThreadInit event wait.

## 2026-07-13 — env-gated ABZU event/thread lifecycle observer

- Added diagnostic-only `MACRUNNER_HB_EVENT_LIFECYCLE_PROBE=1`: it aliases the existing bounded HB import and Wine low-level event tracers and adds a bounded 512-record thread create/server-suspend/NtSuspendThread/NtResumeThread lifecycle trace. Default behavior and event/thread semantics are unchanged.
- Source guard `tools/test_hb_event_lifecycle_probe_source.py` passes together with the Fix A and dynamic-IAT guards. Coherent build PASS: Unix ntdll `778b3e87…`, ARM64X PE ntdll `a62ea85a…`; errors and `install skipped` are zero.
- The sole observer run classified the apparent stall as a phantom: real main `0x009c` completes all `0x98` startup handshakes; TaskGraph worker `0xa4` legitimately waits on idle event `0x94`, pool worker `0xa8` polls idle event `0xa0`, and `0xc8` polls its private idle event. No event fix was made; follow-up belongs at the true main-thread post-handshake boundary.

## 2026-07-13 — env-gated ABZU main 0x009c post-handshake observer

- Added diagnostic-only `MACRUNNER_HB_MAIN_009C_PROBE=1` in `unix/macrunner_hb.c` and a low-level wait callback in `unix/sync.c`. It arms after main TID `0x009c` completes its third successful `0x98` startup handshake, then records bounded main-only waits, imports, guest block exits, and run exits. Default execution and all wait/import semantics are unchanged.
- Source guard `tools/test_hb_main_009c_probe_source.py` passes together with event-lifecycle, CS-forward, and dynamic-IAT guards. Coherent build PASS: Unix ntdll `c27cf057…`, ARM64X PE ntdll `a62ea85a…`; errors and `install skipped` are zero.
- Sole probe result: no post-handshake Wine wait. Main terminates guest progress in x64 `ws2_32!unix_call_init`, synchronously calling native `__wine_unix_call_dispatcher` with null unixlib handle for `ws_unix_gethostname`. Root cause is the existing policy that skips most AMD64 builtin DllMains while mixed-view routing can still execute their guest bodies; no runtime fix was made in this task.

## 2026-07-13 — class-level AMD64 builtin Unixlib handle resolution

- Previous fix: capability-based execution of every Unix-capable AMD64 builtin DllMain. Why it did not work: the sealed run reached ws2_32/dnsapi nonzero funcs but stalled in early loader attach, proving arbitrary DllMain side effects are not a safe publication mechanism. The candidate was removed rather than expanded.
- This fix targets the evidenced class boundary only: when an x64 `WINE_UNIX_CALL` arrives with handle zero, derive the caller module from its guest return PC, query `MemoryWineUnixFuncs`, and dispatch through that module's returned table. Wine `virtual.c` owns/caches Unixlib loading.
- Not extending to: module-name allowlists, arbitrary DllMain lifecycle, event/wait behavior, or native export routing.
- New diagnostic gate: `MACRUNNER_HB_BUILTIN_UNIXLIB_RESOLVE=1`; bounded log `macrunner-hb-builtin-unixlib-resolve`. Source guard covers 40 `__wine_init_unix_call` module families and all four prior boundary guards remain PASS.
- Coherent build PASS: Unix `b60e43c3…`, ARM64X PE `38841810…`; errors/install-skipped zero. Sole ABZU verify is PARTIAL: ws2 resolves (`status=0`, nonzero handle) and main continues with 12 additional ThreadInit successes, but a later UE4 JIT helper memory fault at `0x1405bd441` yields one `c000007b` before real graphics calls.

## 2026-07-13 — bounded JIT memory-helper fault observer

- File(s): `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `tools/test_hb_jit_fault_trace_source.py`.
- Type: DIAGNOSTIC.
- What: Added default-off `MACRUNNER_HB_JIT_FAULT_TRACE=1`. TLS records only the first failing special-read/write helper inside each JIT block; a global 128-line budget reports guest TID, block/next PC, helper, exact address/size/result, HB-region classification and x64 registers.
- Why: sealed ABZU evidence reports generic `JIT helper fault` at block `0x1405bd441`, but does not distinguish unsupported execution from the suspected later `mov rax,[rcx]` NULL read after `CoCreateInstance` returned `REGDB_E_CLASSNOTREG`.
- Semantics: wrappers return the original helper result unchanged; no recovery, retry, status mapping or interpreter/JIT routing changes. Source guard and all five prior boundary guards PASS.
- Verify: coherent build PASS: Unix `4b16eda1…`, unchanged matching PE `38841810…`; gate/log strings present; errors and `install skipped` zero (13 pre-existing-style warnings). Six source guards PASS. One 120s NOT_GOLDEN ABZU probe remains pending. The proven `b60e43c3…`/`38841810…` breakthrough dist was floored byte-exact before this edit/build.
- Run result: observer captured `special-read address=0 size=8` on worker `0xd0`; `RAX=0x80040154`/`RCX=0` binds the guest failure to ignored `REGDB_E_CLASSNOTREG` from WIC Factory2 activation. This is a fresh-prefix COM-registration defect, not a JIT semantic defect. Report: `reports/dualdata/ABZU-UE4-JIT-FAULT-5BD441-CAUSE.md`.
- Status: diagnostic-verified-root-caused; observer retained default-off.

## 2026-07-13 — opt-in Windowscodecs COM registration for throwaway DXMT prefixes

- File(s): `scripts/mr-run.sh`, ABZU verification overlay `MacRunner-abzu/scripts/mr-run.sh`, `tools/test_mr_run_windowscodecs_regsvr32_source.py`.
- Type: ROOT-FIX (env-gated diagnostic first).
- What: Added default-off `MACRUNNER_MR_RUN_REGSVR32_WINCODECS=1`. After System32 synchronization, the runner validates the payload, selects `regsvr32.exe` from the same `${MACRUNNER_PREFIX_SYSTEM32_ARCH}` view, runs bounded `/s C:\\windows\\system32\\windowscodecs.dll`, and propagates missing payload, timeout, or registration failure instead of launching the title.
- Why: the sealed ABZU WIC worker proved `REGDB_E_CLASSNOTREG`; a template-less prefix skipped wineboot and copied the AMD64 DLL without its COM registry mapping. The selected System32 payload is x86-64 (`d7c75f1d…`). The first candidate also selected x64 regsvr32, but the sealed verify proved that guest launcher exits `INVALID_ARG=1` before loading the DLL. It was corrected to the native ARM64 launcher already proven by the adjacent actxprxy stage; payload architecture remains x86-64.
- Not extending to: wineboot policy, guest/JIT fault suppression, WIC object emulation, graphics APIs, or default-on behavior before title verification.
- Verify: both Main and ABZU runners pass `bash -n`; the two-runner source guard proves default-off gating, native-launcher selection, bounded timeout paths, payload check, and nonzero rc propagation. No build required. An explicitly authorized second 300s NOT_GOLDEN run successfully registered x64 windowscodecs through the native ARM64 launcher; prior WIC HRESULT/NULL fault signatures are zero and the title survives to timeout. Graphics remains IAT-only, so the result is PARTIAL rather than end-to-end PASS.
- Status: native-launcher-runtime-verified; later pre-RHI stall remains.

## 2026-07-14 — bounded HK wait/wake boundary observer

- File(s): `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/wine/dlls/ntdll/unix/sync.c`.
- Type: DIAGNOSTIC.
- What: Added default-off `MACRUNNER_HB_WAIT_WAKE_TRACE=1`, optional marker-file delayed arming, and a default 5000-line global budget. It records semantic WaitOnAddress guest address/comparand/size, exact per-thread host alert futex and value, waiter/waker Wine TID and name, timeout, caller module/RVA, WakeByAddress mode/target, NtAlertThreadByThreadId, and NtSetEvent. The observer can snapshot already-registered semantic waiters when armed.
- Why: HK stopped after Mono with UnityGfxDeviceWorker and both DXMT workers parked, but a host sample alone could not distinguish a lost wake from correctly idle downstream queues.
- Semantics: observation only. Wait/wake/alert/event return values, address values, timeouts, and routing are unchanged. Patch C/D/E/F/G/H and EH benign no-op are untouched.
- Verify: HyperBridge and ntdll build PASS; errors 0, `install skipped` 0, build/dist byte comparison PASS, strict codesign PASS. Deployed Unix SHA is `d1d48b5b9e30d848854aa327b97875cff6ddf05e0dd702881eb19c1d2270fa7b`.
- Run result: one 600s marker-gated run plus one 120s capture prove distinct wait addresses. Unity main TID `0x3c` successfully wakes worker `0xe0` five times through UnityPlayer semaphore release, then emits no later release; DXMT encode/finish remain on separate idle alert futexes. Root boundary is upstream producer execution, not Wine wait/wake semantics. Report: `reports/phase4-hollow-knight/WAIT-WAKE-BOUNDARY-RESULT.md`.
- Status: diagnostic-verified; observer retained default-off.

## 2026-07-14 - Bounded HK main-producer observer

- File: `engine/wine/dlls/ntdll/unix/macrunner_hb.c`.
- Type: DIAGNOSTIC.
- What: Added default-off `MACRUNNER_HB_MAIN_PRODUCER_TRACE`, optional arm-file gating, and a bounded event budget for guest main TID `0x003c`.
- Semantics: reused existing block/import/wait hooks; no synchronization, Fix A, Patch C/D/E/F/G/H, or EH benign-noop behavior changed.
- Verify: build/deploy SHA-256 `c0e0fe484b2d26b1c059493ed72e60e2bc693996998596ab3135b29e9c707987`; strict codesign and build/dist comparison passed; errors and `install skipped` zero.
- Run result: immediate post-Mono park disproved. TID `0x003c` executed Mono metadata/assembly blocks and imports until the shared 5,000-event budget exhausted 87 ms after arming; no wait was captured. Late wait/exit remains unresolved and requires `native_tid`, time-stratified blocks, and an independent wait budget.
- Status: diagnostic-partial; observer retained default-off.

## 2026-07-14 - Refined HK main-producer transition observer

- Files: `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/wine/dlls/ntdll/unix/sync.c`.
- Type: DIAGNOSTIC.
- What: refined default-off `MACRUNNER_HB_MAIN_PRODUCER_TRACE` with guest/native TID mapping, a 200-block burst followed by approximately two-second PC samples, periodic top-3 import aggregation, an independent 1,024-record wait budget, exact `NtWaitForAlertByThreadId` entry/return fields, and armed `run_x64` exit records.
- Semantics: observation only. Wait/futex/event return values and scheduling are unchanged; Patch C/D/E/F/G/H, Fix A, and EH benign no-op semantics are untouched.
- Verify: HyperBridge/ntdll build PASS; Unix build/dist SHA-256 `2782f3bb16b3ca0b9d3b180203731f7d95d98f4f6fa114b9f632e53cda164df1`; byte comparison and strict codesign PASS; errors 0, `install skipped` 0, warnings 15.
- Run result: guest main `0x003c` maps to native TID `34681011`; it does not remain in Mono or block on handle `0x118`. The wait returns success immediately and ten nested x64 frames unwind normally by `+59.996s`. The unresolved boundary moves to the native caller after final `run_x64` return. Host stack remains unavailable because the sample targeted bash.
- Status: diagnostic-partial; observer retained default-off. Next evidence must instrument native post-return state, not alter synchronization.

- 10:06 · Diagnostic-only POST_RUN_X64 native observer (default off): independent outer return/native wait/re-entry/thread-exit records; mr-run exact Wine PID handoff and mandatory run-contract/final-child/flight artifacts. Built and deployed ntdll SHA256 f5f5c6c8c30c202f2ad52a51b69f39909988893a0d03f3a51a10b016447a4f87.

- 10:40 · Post-run observer follow-up (source only, not rebuilt/deployed): main thread-exit capture no longer depends on final-return pending and now records run depth/status/native caller module+RVA. mr-run now seeds/finalizes mandatory flight.jsonl. Verified run itself used deployed ntdll f5f5c6c8... and predates these follow-ups.

### 2026-07-14 11:44 +10 - post-run_x64 termination observer verification

- Built/deployed ntdll SHA-256 `da69bb793a634c9b26939ca4664eea567530563c988d055990a1cc5480fa2e77` with existing default-off post-run_x64 observer and NtTerminateThread/exit/abort/pthread coverage.
- HK diagnostic exercised natural self termination only; static observer suite 6/6 PASS. No engine source was changed after the runtime.
- Harness-only follow-up models exec `_` identically in identity-ledger and final-child capture; capture-only parity suite 6/6 PASS.

### 2026-07-14 12:07 +10 - bounded HK exit-origin observer

- Added default-off `MACRUNNER_HB_EXIT_ORIGIN_PROBE` instrumentation only: last executed x64 block and return PC at self AV termination, PE exception/vectored/SEH dispositions, and `RtlExitUserThread` caller identity.
- No exception, translation, synchronization, or termination behavior is changed. Patch C/D/E/F/G/H, EH benign no-op, and fix A remain untouched.
- 2026-07-14 12:30 · Default-off HK MACRUNNER_HB_EXIT_ORIGIN_PROBE verified in one 600s diagnostic run · linked nested x64 block state and PE exception-disposition state to RtlExitUserThread without semantic changes; runtime localized c0000005 to native run_jit_block_with_signal_guard+832 write AV, not guest EH; built/deployed ntdll SHA ebd096440b9765a6150d5ebfe8152cc7d906d2a4143791c82db9e3561e6a9500; Patch C/D/E/F/G/H, EH benign no-op, and fix A preserved.

## 2026-07-14 - x64 callback reject must not skip JIT epilogue

- Evidence: I386 callback rejection was encoded as return value zero after the signal route had already redirected PC to the callback trampoline. The trampoline returned to LR, skipping the active JIT epilogue and leaving SP low by 0x30 with x22 holding x86 bytes; guard teardown then wrote through corrupted x22.
- Fix: callback-specific signal-safe non-AMD64 preflight before ucontext mutation; dispatcher handled/result split; trampoline returns to LR only when handled. No stack-layout compensation.
- Files: `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/wine/dlls/ntdll/unix/signal_arm64.c`, `tools/hk_callback_reject_contract_static_test.py`.
- Validation: static adversarial contract 6/6 PASS; ntdll-only build PASS; deployed SHA `db8394149516458e478701eeae284c5075a814cec8aab662a64a602c6c46b4ee`.
- HK NOT_GOLDEN verify: guard AV/RtlExitUserThread(c0000005)=0, process survived 600s, Physics and CreateDXGIFactory2 rc=0 reached; swapchain/Present1 not reached. Report: `reports/phase4-hollow-knight/CALLBACK-REJECT-FIX-VERIFY.md`.
2026-07-14 · HK host-native observer · added default-off `MACRUNNER_HB_HOST_NATIVE_SAMPLER` with independent host-time Mach PC/module/RVA sampling, exact wait snapshots, and existing termination-chain correlation; harness locator selects the `run_x64` implementation rather than its declaration; static suite 8/8 PASS; ntdll SHA `f1e6a753aa5421ca9e492bbe3756ab42ccc30d4770eff9197d63c3e321993781`.
2026-07-14 · HK callback/signal loop observer · added default-off `MACRUNNER_HB_CALLBACK_LOOP_TRACE` with bounded/time-stratified outer-signal, router, assembly-trampoline, and callback-dispatch correlation; focused suites 14/14 PASS; ntdll SHA `6bc5b52cef6c0943906a334ffe974780ee9c2571070a934d49e071a1b6cba7c7`. Diagnostic run proved callbacks balanced and exposed generic PE-call `x18` clobber window before native `RtlEnterCriticalSection`; no ABI fix applied in this turn.

## 2026-07-14 - ARM64EC PE bridge x18/TEB restore family

- Root-layer change: `macrunner_hb_arm64_pe_call12` restores saved TEB in `x18` immediately before its PE `blr`; `macrunner_hb_arm64_pe_call20_direct` now receives an explicit TEB and does the same restore.
- Family audit: the import wrappers and dispatch functions are callers; the two direct `blr x20` PE bridge members in `macrunner_hb.c` are covered. Callback-frame semantics, Patch C/D/E/F/G/H, fix A, and the callback-reject fix were not changed.
- Regression floor: x18 bridge static suite plus callback-reject and callback/signal suites passed.
- Built/deployed ntdll SHA `d9b6ea94e6fdef6da47acf7827e8b256fdb7fe31190a4e58c072803c66a1ae88`.
- HK verification was `NO-HIT`: compiled restores are present, but `pc=0x87fff9e0ed8 fault=0x48` persisted. No follow-on speculative source patch was applied; transition provenance remains required.

## 2026-07-14 - x18 critical-section/dispatcher boundary diagnostics

- Files: `engine/wine/dlls/ntdll/sync.c`, `engine/wine/dlls/ntdll/unix/signal_arm64.c`, `tools/hk_x18_dispatch_return_static_test.py`.
- Added ARM64/ARM64EC x18 preservation around the existing critical-section observer family and retained bounded x18 fault provenance. Removed the redundant host C TEB helper after the dispatcher frame has already restored `x18`, so no host call remains between the frame load and PE return.
- Focused static dispatcher-return suite: 6/6 PASS. Restored syscall stub disassembly contains the original `mov/mov/ldr/ldr/blr/ret` sequence and no experimental x18/stack handoff.
- Built/deployed Unix ntdll `7b8e887b792a07b454b45cd8bbbab1901c4d590d4d675117ce3f23d03a38b274`; PE ntdll `c52154865375fcdb1cb6404ed403b775ddac566cb7f362e439f2d13b139e6a40`.
- Runtime result remains `NO-HIT`. Register, stack, seeded-stack, and dispatcher-owned syscall experiments failed or regressed and were removed. No syscall causality claim is retained.
- Patch C/D/E/F/G/H, fix A, callback-reject fix, and EH benign no-op behavior are unchanged.

## 2026-07-14 — ARM64EC x18 producer family audit

- Added bounded, default-off x18 fault stack provenance (`stack_c0`, `stack_150`) under the existing callback-loop diagnostic gate.
- Audited the full PE-target `blr` family: call12 and call20_direct are complete; no third bridge exists.
- Tested and rejected syscall/unix-call final routing through the x18 thunk: runtime no-hit, so the change was removed.
- Focused x18 suites and ntdll-only build PASS. Clean host ntdll/dist SHA: `ef5b081900965c72112fe3bba480393a4f623c175adf069e85d2add0fbbc1054`.
- No Patch C/D/E/F/G/H, fix A, callback fix, graphics source, or translation cache changes.

## 2026-07-14 — HK main termination observer coverage completion

- Reused `MACRUNNER_HB_POST_RUN_X64_OBSERVER`; no duplicate diagnostic gate.
- Added a bounded process-wide remote `NtTerminateThread` path so a non-main caller is recorded with caller TID/depth/pending/native location.
- Added explicit `natural-return/x64_thread_entry` correlation before the PE `RtlExitUserThread` chain.
- Kept `abort_thread`, `exit_thread`, and `pthread_exit_wrapper` sink records independent of outer-run pending state.
- Expanded static and capture-only fixtures; no Hollow Knight/Wine runtime authorized or executed.

- 2026-07-15: Implemented default-off `MACRUNNER_HB_JIT_SIGBUS_INVALIDATE`: post-signal native-PC owner mapping, persistent per-runtime guest-block quarantine, exact snapshot-entry interpreter resume, and fail-closed JIT disable when mapping/allocation is unavailable. Build/verify pending.

- 2026-07-15 verification: ntdll SHA `dd8866c27cc395550ac5826b002ad7ee6e2e8a2682d49f91784b6bdcc7038867`; 600 s HK run was PARTIAL/no-exercise (`SIGBUS=0`, invalidation records=0). The prior single-PC loop was absent, but no causal claim is permitted and D3D11/swapchain/Present1 remain unreached.

- 2026-07-15: Extended the default-off host-native sampler with a race-free atomic guest-progress channel (`guest_pc`, block-update sequence, PC-change count). This distinguishes normal per-block module classification from a fixed guest loop without changing classifier semantics. Build/verify pending.

- 2026-07-15 host sampler guest-progress verification: ntdll SHA `8e5323d20e18ee8eb49d7f1647de74f51e6fa0c219e96c6154cb44dcdc5e6948`. Probe established that module predicate RVA `+0x144` is a bounded LDR-list walk, while the actual terminal boundary is outer `run_x64` status `0xc000007b` at import index 951. Diagnostic counters are default-off with the existing host sampler.

## 2026-07-15 04:34 · Default-off terminal import-thunk fault probe
- File: engine/wine/dlls/ntdll/unix/macrunner_hb.c.
- Gate: MACRUNNER_HB_IMPORT_THUNK_FAULT_PROBE=1 (default off).
- Records bounded terminal failure stage, DLL/API/index, resolved target/module/RVA, stack return, guest/native PCs/TIDs; import dispatch behavior is unchanged.
- Built/deployed ntdll.so SHA-256: 0b9b509e8d00101f48c3a73c6a6ea1988525a32b05a059a635df5c839e7c021e.
2026-07-15 09:17 · A/A-only Hollow Knight diagnostics (default-off): `MACRUNNER_HB_AA_SIGBUS_PROBE` captures JIT block-entry x64 context, signal ucontext, guest/native bytes, source/destination pre/post windows and Mach VM protections before interpreter replay; `MACRUNNER_HB_AA_RBP_PROBE` checks Mono RVA 0x425870..0x425c8e frame invariant; `MACRUNNER_HB_AA_MONO_PATCH_PROBE` records patch-site/target/16 bytes at assertion RVAs. Signal fault API now receives read-only host ucontext. Build/relink PASS; no execution semantics changed when probes are disabled.
2026-07-15 09:27 · A/B-only Hollow Knight diagnostic gate (default-off): `MACRUNNER_HB_AA_FORCE_MONO_SIMD_COPY_INTERP` recognizes the Mono SIMD-copy block entry byte signature (`0f1f440000f30f6f0af30f6f5210f30f`) and returns through the existing interpreter fallback before native entry. Disabled in A/A; intended as the single B variable after deterministic A/A.
2026-07-15 09:29 · Rebuilt/relinked/deployed A/A+A/B diagnostics: `libhyperbridge.a` SHA-256 `cbeb54216d596d989383ee04d0cc5296e947d2897e194eed807a8ff067917557`; build `ntdll.so` `9e80a02fb72209068566250a5908a22717cc1c55d18cbca6763c43dadcc077ee`; ad-hoc-signed deployed `ntdll.so` `4cbef8c3c11a92bbfb6499668a1e31ded3d1ab03fd4833cc1de11b3c7ab8770f`.
2026-07-15 10:12 · Narrowed default-off Mono SIMD-copy A/B diagnostics. `MACRUNNER_HB_AA_FORCE_MONO_SIMD_COPY_INTERP` now requires the existing Mono module discriminator plus `codegen_module_base` RVA `0x4ee14b` or `0x4ee150` and matching SIMD bytes; UnityPlayer/HK byte-signature matches are rejected. Added read-only `MACRUNNER_HB_AA_MONO_SIMD_COPY_GATE_PROBE` to log narrow hits without forcing interpreter. Build/relink pending.
2026-07-15 10:13 · Rebuilt/relinked/deployed narrowed Mono SIMD-copy diagnostics: `libhyperbridge.a` SHA-256 `814ae6e96452b7931836619d57f249332a10597f9648b9b3b8cefb83041aced5`; build `ntdll.so` `cafe0dea1310866a4f79bb8a9e6bbf6b20f28fd6f390de1ae65ad5ba9924daae`; ad-hoc-signed deployed `ntdll.so` `558eaef2a0533ee1a57422758e6403dd9ac140e3243aabc6899318caf9a6c190`.
2026-07-15 11:03 · Added default-off Mono `0x4ee14b` transparency and inline-force diagnostics. `MACRUNNER_HB_AA_MONO_4EE14B_TRANSPARENCY_PROBE` shadow-runs the same pre-state through native JIT and the IR helper, compares PC, selected registers, x64 context, XMM hash, and source/destination windows, then restores state before normal execution. `MACRUNNER_HB_AA_FORCE_MONO_SIMD_COPY_INTERP` now invokes `hb_jit_helper_exec_ir_block()` inline for cached Mono/RVA blocks instead of requesting the outer unsupported-feature fallback. Built/deployed inline artifact SHAs: `libhyperbridge.a=98d57662edaf34582c06a7bdc5134c3fa8605b9714ea014be47952ff2c964f91`, build `ntdll.so=5f7d946e1a5855fafbe657e42793309c37369f630418d2d1d8965bd153eb0478`, ad-hoc-signed deployed `ntdll.so=7cc24c214446ddc17d7d3319c5f50e837198d23c2d99f05752926c6bf7ada341`.

2026-07-15 12:11 · Implemented default-off `MACRUNNER_HB_JIT_SIGILL_OWNERSHIP`. An active HyperBridge JIT-slab PC is claimed before ARM64X thunk/module/Mach-VM detection; non-owned SIGILL preserves the existing ARM64X route. Generalized the persistent recovery quarantine across SIGBUS/SIGILL, restores the exact block-entry snapshot for interpreter replay, and records the owner guest/native range, native offset/word, and 16 guest bytes. Signal-family source contracts: 2/2 PASS. Built/deployed SHAs: `libhyperbridge.a=aa58efa395ca19ce810c08eb8d3220e32e62a4cda14e0f125f4bb3a5120bddc9`, build `ntdll.so=aa9ce8e1cbcb6c9bceea505ccab03bb3d38f4c16c7f3cdcde6aa94b3846ac569`, signed deployed `ntdll.so=2ff51d32c6f7db90def39bba1cad6b25263e5847d69fa1c1fd91eb4c1eb3829e`. Runtime verification pending.

2026-07-15 12:40 · Corrected SIGILL ownership after the first 600 s runtime exposed an out-of-slab JIT transfer. LLDB byte truth: guarded Mono RVA `0x4ee150` native entry `0x11e55a1f0..+0x334` reached zero RX `0x11c264000` (`UDF #0`), so slab membership was not a sufficient ownership predicate. The default-off pre-ARM64X path now claims any active TLS JIT guard, carries ntdll's signal-safe native word and full fault ucontext across `siglongjmp`, quarantines the guarded source block when native-PC lookup misses, and decodes only source branch edges resolving exactly to the fault PC. Non-owned SIGILL and range-owned SIGBUS behavior remain separate. Persistent cache byte scan found neither the current-ASLR block nor the stale target literal, so no speculative cache/codegen patch was made. Targeted signal-family tests: 2/2 PASS; build had no `install skipped`. SHAs: `libhyperbridge.a=190a287273ac3647b0e069730e6658e8d32b2d2e1551a860213e8f4924b53e0c`, build `ntdll.so=cb37a0b993fca2619cbbca00a273ce7d5469da537b0400451b06d940e4ce726d`, signed deployed `ntdll.so=79b7a98469af8944caeb0d1764f6cf7bc6dabde61e1275421603eaac9869e6a0`. Runtime re-verification pending.

2026-07-15 14:02 · Fixed the evidenced executable-page 128-bit JIT store family. Fault provenance disproved null code emission: Mono RVA `0x4ee150` contained eight valid raw `DMB ISHST; STR X20; STR X22` sequences, while fault PC/X21 was an external R-X destination page and no branch among 64,685 live cache entries targeted it. `emit_direct_mem128_store_from_x20_x22()` now calls checked `hb_jit_helper_store_u128()`; executable writes use the existing W^X-aware memory path and bump generation, while writable memory retains its fast path. Persistent cache version is 21. Tests `jit_xmm_store_wx` 2/2 and signal ownership 2/2 PASS. Build/deploy SHAs: `libhyperbridge.a=62048b1204b89192fa130cdabefe5415677cbf377c777daff2a93245d588db53`, build `ntdll.so=01d57e88833abbd4c45c3fe2e86c7ee1cc0694930766948dc5410ab27f387c07`, signed deployed `ntdll.so=38a24ce6f66be276c62d8eacb2a837975a2fbb4dd02f0265170d7a38f70d63f7`; no warnings/errors/`install skipped`.
2026-07-15 17:22 · Added default-off object-aware DXGI diagnostics. Factory and swapchain COM pointers are tracked separately, so factory slot 8 labels `MakeWindowAssociation` and only a registered swapchain slot 8 labels `Present`; unknown slot-8 candidates remain unlabeled. `MACRUNNER_HB_MAIN_POST_MAKEASSOC_PROBE` arms only after successful factory `MakeWindowAssociation` and emits bounded same-thread block/import telemetry plus periodic guest progress samples. Build/runtime verification pending.
2026-07-15 17:27 · Rebuilt and deployed object-aware DXGI/post-MakeAssociation diagnostics. Build completed with rc=0 and no `install skipped`; existing unrelated warnings only. Build `ntdll.so` SHA-256 `c05f386f4ba39bd3561d455d2477c5fb186c750af2384c4374c6580ef7845d5f`; ad-hoc-signed deployed SHA-256 `397d2e4fcad6265acfb6f79a55e3514222627e666e5372f606ab137454dbc446` (previous deployed artifact preserved as raw evidence).
2026-07-15 17:56 · Runtime-verified object-aware DXGI diagnostics: factory slot 8 now reports only `MakeWindowAssociation`, swapchain slot 8 only `Present`, and false Present count is zero. The 600 s READY run found the next root cause rather than a graphics wait: `hb_jit_helper_write_bytes_tso` uses `hb_jit_live_host_ptr` plus raw memcpy on metadata-RWX/current-RX pages, so the current 128-bit W^X fix is bypassed and main loops in signal callback/ARM64X routing at Mono `0x2791bc`. No source fix applied in this trace task; see `POST-MAKEASSOC-TRACE-RESULT.md` for the evidenced helper-family fix candidate.
2026-07-16 10:05 · Added default-off cross-thread `MACRUNNER_HB_CALLBACK_4784D0_PROBE`. The bounded observer records exact Unity `+0x51b4b0` plan, `+0x51e840` enqueue, returned task handle, callback `+0x4784d0` entry on any HB guest thread, coordinator `+0x18d0/+0x18d8/+0x18e8` state, pending clear, tail reschedule, and scheduler return. No guest execution semantics change. READY 600 s runtime disproved callback loss: Gfx tid 00e8 enqueued it, scheduler tid 0078 executed it before enqueue returned, cleared pending 1→0, and returned to `+0x51dc03`; 114 traced edges were Unity-only. Run build/signed SHAs `aa78de275e2d…`/`7385c190b98f…`. Removed noisy intermediate predicate logs post-run while preserving exact plan proof; rebuilt/deployed `1e4942c01226…`/`389c7bb86c1f…`. Verdict: no callback/scheduler fix; graphics wall is downstream. Report: `CALLBACK-4784D0-CAUSE.md`.
2026-07-16 11:33 · Added default-off, bounded `MACRUNNER_HB_WORK_OBJECT_PROBE` for the exact Unity coordinator lifecycle: Gfx-owner handoff, 32-byte descriptor construction, nested backing allocation, `+0x18d0` publication, `+0x2b6fc0` consumer return, and capacity decision. Event reads are limited to 32 descriptor and 64 backing bytes; disabled behavior is unchanged. READY runtime proved the tracked value is a Unity 4 MiB buffer/arena descriptor, immediately consumed on scheduler tid 0078, not a DXMT frame/Present work item. One post-epilogue observer snapshot used restored RDI and is excluded; source now reloads stable coordinator/descriptor state. Run build/signed SHAs `37d3d7c22248…`/`956d83ccef29…`; corrected observer rebuild/deploy `74d0b32d2d36…`/`305c471c9eeb…`, build rc=0, errors=0, `install skipped`=0, codesign verify PASS. No execution-semantic fix is warranted at this object.

2026-07-16 12:43 · Added default-off, bounded `MACRUNNER_HB_GFX_OWNER_VTABLE_688_PROBE` in `macrunner_hb.c`. Exact Unity block gates capture the wrapper receiver, owner/coordinator, primary queue pop, slots `+0x680/+0x688/+0x690/+0x698`, target module/RVA/bytes, target entry/return, and the empty-queue branch without changing guest state. READY 600 s runtime resolved slot `+0x688` to Unity callback adapter `UnityPlayer+0x6c6960`; primary queue `coordinator+0x18` popped NULL in both owner cycles, so the callsite was correctly skipped toward the alternate queue. Evidence artifact build/signed SHAs `8895f374b8c7e86cca1a3bb11285546d6d0b1417557f0b6c1306c2f4052f9296`/`0b8f6ea005283f5c732ac5cf8efcd0506a65126b3a507666d6f027b1659cc7cd`; run log duplicated bounded empty-gate observations until budget exhaustion, so current source adds a per-owner gate latch. Final rebuild/deploy SHAs `b28c7afae5e5477757d4f7f14d4e4611bd258db45a316ebc6f855ef91d804fdc`/`bd6a0669dc61229605415a53c5e117ebe0cc3e6230c6a27e5da1cd04a50cd9a2`, build rc=0, errors=0, `install skipped`=0, codesign verify PASS. Probe-only change; no execution-semantic fix is warranted at `+0x688`.

2026-07-16 13:27 · Added default-off, bounded `MACRUNNER_HB_ALTERNATE_QUEUE_478ABF_PROBE` in `macrunner_hb.c`. The event observer records coordinator queue pointers and `queue+0x40` states, alternate pop, item/payload bytes, dynamic `[payload+0x68]` target, callback descriptor/entry/return, and registers without changing guest execution. READY 600 s runtime proved `Unity+0x478abf` is an unreached indirect callsite: both primary `+0x18` and alternate `+0x10` queues had tagged empty state `0x2`, alternate pop returned NULL, and execution skipped to `+0x478b88`; real Present remained zero and strict pixel BLACK. Build/signed SHAs `792ad5e1d806d869276d247ed55059b1f9725e31c0eb0a79b39cf25f58d7acfd`/`716c2a3ef66d39e4a896d46720b497c4f0d02a9342ffaf03585261c703714275`, build rc=0, errors=0, existing warnings=14, `install skipped`=0, codesign PASS. Probe-only change; no consumer semantic fix is justified. Report: `ALTERNATE-QUEUE-478ABF-RESULT.md`.

2026-07-16 14:36 · Added default-off, bounded `MACRUNNER_HB_PRODUCER_PUBLISH_PROBE` in `macrunner_hb.c`. Exact Unity events distinguish initial item construction, scheduler `+0x478650` and direct `+0x47893a` publication, atomic head/tag/tail transitions for coordinator queues `+0x10/+0x18/+0x20`, wake, and consumer-only requeue/recycle; item/payload bytes are captured without changing guest state. Build rc=0, errors=0, existing warnings=14, `install skipped`=0; build/signed deployed SHAs `107001b97df3f3635fc650806f07b17bb298a4148c58ef346119576e147ff1b0`/`5d784c2516a9c840ca601c6e34bfc9555f8e50d792975107016b07fda2e214a7`, codesign verify PASS. READY 600 s runtime caught two scheduler publications of one TextCore/resource item to `coordinator+0x10` and immediate drain; the MPSC producer is working, but no Present-bearing frame item was created. Probe-only change; no queue semantic fix is justified. Report: `PRODUCER-PUBLISH-RESULT.md`.
## 2026-07-16 — Present-item lifecycle diagnostic correction

- Added default-off `MACRUNNER_HB_PRESENT_ITEM_CREATION_PROBE` and a read-only owner-thread `hb_jit_runtime_native_block_info()` lookup for mapping a native PC into the live HB block cache. No guest state, control flow, queue, focus, or graphics behavior is changed.
- The sealed runtime proved the assumed PlayerLoop table offsets hold RW self-linked data sentinels, not callbacks. Removed the impossible guest-PC comparisons and renamed the affected records to neutral table-value/profiler/generic-scheduler terminology.
- Current-wall verdict: two valid 600 s runs reached swapchain creation and `MakeWindowAssociation`, but no Clear/GetBuffer/Present frame lifecycle; main remained in Mono/managed initialization through timeout. DXMT swapchain Present slot is non-null; strict pixel remains BLACK.
- Evidence artifact SHAs: HyperBridge `7834420049dda4b889f3acb4484facbcd8cde29fc84cf4d2ac73c465666a3fac`, runtime ntdll build/signed `fc12b057d34fc024d5148558fe103314b531185cfb0891e254312099143d57de` / `5048abd3ff2b492d06099f5a90cd0331d1049d227c26ea71fc68428e7dd73118`. Final terminology-only build/signed `09c5ed4931dbf268d8a567d20d45a83c319f779cfe665d259cfb3dec4439f817` / `9c763fe3a5a421bc0f743a7bf85baf34ea25db70c61913b0ce940c6b075c7d22`.
- Build/relink had errors 0 and `install skipped` 0. Full legacy HB suite is not green: 449 passed / 26 unrelated existing failures. Report: `reports/phase4-hollow-knight/PRESENT-ITEM-NOT-CREATED-CAUSE.md`.

## 2026-07-16 — Host-only Mono-init stall sampler

- Added default-off `MACRUNNER_HB_MONO_INIT_STALL_PROBE`. It reuses the independent host-native main-thread sampler with a 5-second default interval, relaxed-atomic guest PC/RSP/RBP snapshots, Mach thread state, run/wait/exit state, and a bounded 12-frame native FP walk. It does not inspect guest modules or guest memory from the executing guest thread.
- An initial guest-thread inspection implementation deterministically introduced main `c0000005`; an identical flag-off control survived. That observer was removed before the valid runtime. This is recorded as observer-effect evidence, not a target failure.
- Static host-sampler contract: 12/12 PASS. Build rc=0, ccache active, no `install skipped`; build `ntdll.so` SHA-256 `124a92f2d5eacfcd21ff7f7633e85046ff45ed06978d00ce72a30201633b98e3`, signed deployed SHA-256 `62c4d0a1607de93cee46a5b127364ae995b5716af777a73660baa4cf7fad8789`.
- READY 600-second runtime classified the main as active/slow rather than deadlocked and exposed a separate existing hot-path defect: disabled A/A probes perform uncached environment lookup on every block. No performance semantic fix is included in this probe-only change. Report: `reports/phase4-hollow-knight/MONO-INIT-STALL-CAUSE.md`.

## 2026-07-16 — Zero-cost-disabled HB diagnostics and loader classification caches

- Cached the A/A RBP and Mono-patch env flags once per run and bypassed their helpers entirely
  while disabled; added a four-entry TLS LDR range cache and a separate four-entry TLS Mono
  classification cache. Mono classification now returns the module base in the same lookup.
- Added a correctness-preserving no-op path for an exact, unchanged Wine-owned live HB region;
  the Mach reserved-tail commit check still runs. Static minimal-overhead + host-sampler contracts:
  18/18 PASS.
- Build rc=0, ccache active, errors=0, existing warnings=14, `install skipped`=0. Build/signed
  ntdll SHAs: `aac647534ad125c07bf5abc6869475bcd1a1217e5109fb5cd00109b269394d58` /
  `09874c37027e14cc8c83d9f896bf3789335771652c7a3f4cc1faa86dbf4fa1fe`; codesign PASS.
- Final host profile removed `__findenv_locked` (0/118 vs 28/119) and module-name matching
  (0/118) from samples. Remaining `hb_memory_protect` cost is 24/118 and was not hidden by a
  broader W^X bypass.
- The probe-free 1800 s endpoint reached managed scene lifecycle by +416.118 s, disproving the
  all-timeout Mono-init wall, but stayed pixel-black and exposed a managed JIT-helper EXEC_FAULT
  on one worker at +633.070 s. Report:
  `reports/phase4-hollow-knight/MINIMAL-OVERHEAD-1800S-RESULT.md`.

## 2026-07-16 — x64 x87 state isolation and FST/FSTP masked-underflow family

- Rooted the +633 s managed JIT-helper `EXEC_FAULT` in x87 state aliasing: x64 helpers used the
  x86 member of the register union, overlaying RDI/RSP/RBP/R8…R14. Added an appended x64 x87
  state and mode-aware accessor; existing context offsets remain stable.
- Completed the evidenced FST/FSTP family: register `ST(i)` destinations, empty destination
  stores, masked empty-source IE|SF/C1/indefinite response, and FSTP pop after masked underflow;
  memory and register forms share the mechanism.
- Exact x86_64 oracle for `DD D8 DD D9` produced SW/TOP `0000/0→0841/1→1041/2`.
  Targeted interpreter+JIT x87 suite: 19/19 PASS (new tests failed 2/2 before the semantic fix).
- Explicit archive rebuild and forced ntdll relink: lib `846f471e…`, build ntdll `c049b32b…`,
  signed deployed `487c69ec…`; errors/warnings/`install skipped` all zero, codesign PASS.
- Final 1800 s runtime crossed the former fault and stayed active through +1843.898 with all
  runtime/signal fault gates zero. It entered a WineMetal render loop, but pixel stayed BLACK
  because no render PSO was set. Report: `reports/phase4-hollow-knight/JIT-FAULT-633-CAUSE.md`.

## 2026-07-17 — Bounded render-PSO compiler/command-stream diagnostic

- Added default-off `MACRUNNER_HB_RENDER_PIPELINE_PROBE` instrumentation at the two exact
  missing-PSO boundaries: DXMT shader dependency/Metal PSO compilation and WineMetal render
  command decoding. The probe records whether a non-null PSO exists and whether `SetPSO`
  precedes each Draw, without changing draw or pipeline semantics.
- Probe output is event-driven and globally bounded (8192 records by default, override with
  `MACRUNNER_HB_RENDER_PIPELINE_PROBE_MAX`). Existing D3D11 entry instrumentation will be routed
  to the run directory for exact `D3D11CreateDevice` evidence.
- Targeted x86_64 `d3d11.dll` and arm64 `winemetal.so` builds PASS. Deployed SHAs:
  `1e580a96701b3834ccdbabe947076e063aaff63f304dcc04f29ca73e4dac8d1d` and
  `061b42cba4d093acd29ad66ef4abefe8fddb609ffc386898e1dbd9be7af66a5c`.
- The 1800 s diagnostic disproved PSO compilation failure: five observed PSOs reached
  `metal-ok`, non-null PSOs reached WineMetal, and real Present/Present1 returned `S_OK`.
- Root cause is WineMetal state lifetime: `_MTLRenderCommandEncoder_encodeCommands` resets local
  `has_pso` on every call, but DXMT sends SetPSO and Draw in separate calls on the same encoder.
  The later Draw is incorrectly discarded. No semantic fix is included in this diagnostic change.
- Verdict: `reports/phase4-hollow-knight/RENDER-PSO-CAUSE.md`.

## 2026-07-17 — Persist WineMetal PSO state for the render-encoder lifetime

- Fixed the proven state-lifetime bug in `winemetal_unix.c`: `has_pso` is now associated with the
  native `MTLRenderCommandEncoder`, survives split `encodeCommands` calls, is set by
  `WMTRenderCommandSetPSO`, and is cleared at `endEncoding`. This is an unconditional semantic
  fix, not a probe or game-specific workaround.
- Arm64 WineMetal build/deploy PASS. Source SHA `90cbe39d…`; deployed `winemetal.so`
  `6b47b2c…`. Requested ntdll rebuild and strict codesign verification PASS; no
  `install skipped` was emitted.
- The mandated minimal-probe 120 s Hollow Knight run was a runtime **NO-HIT**: timeout rc 124
  before real swapchain/render command decoding, with Draw/Present/pixel all unexercised and all
  CPU/JIT fault gates zero. The missing-PSO warning being absent cannot validate the fix because
  the Draw boundary was not reached.
- A sealed probes-off 1800 s follow-up reached 1,563 Unity frame-phase iterations. The prior
  no-probe artifact emitted the unconditional missing-PSO Draw-skip diagnostic 1,648 times;
  the fixed artifact emitted it zero times, so the proven state-lifetime rejection is gone.
  All CPU/JIT fault gates remained zero.
- Runtime classification remains **PARTIAL**, not pixel PASS: all 146 on-screen strict captures
  were exactly black, and successful Draw/Present method counts are not observable with the
  mandated probes-off environment.
- Verification: `reports/phase4-hollow-knight/PSO-PERSIST-FIX-VERIFY.md`.

### 2026-07-17 — Persistent-PSO method-level runtime verification

- No new engine mutation in this step. A sealed 1800 s diagnostic enabled only the bounded
  WineMetal render-pipeline and object-aware DXMT method counters.
- Runtime proof: 3,046 Draw commands, zero missing-PSO rejections, and 96 Draw-bearing
  `encodeCommands` calls that relied on PSO state set by a prior call. This directly verifies the
  encoder-level persistence fix.
- GetBuffer/RTV/OMSet succeeded; 1,390 Present1 calls returned `S_OK`; nil drawable,
  skipped presentDrawable, Metal command errors, and CPU/JIT fault gates were all zero.
- Pixel is still blocked: 147 strict captures were exactly `#000000`. The remaining boundary is
  backbuffer fragment/output content, not PSO lifetime or submission.
- Verification: `reports/phase4-hollow-knight/DRAW-EXECUTED-BLACK-CAUSE.md`.

### 2026-07-17 — Native bounded GPU backbuffer readback diagnostic

- Added default-off `MACRUNNER_HB_GPU_READBACK_PROBE` instrumentation in native WineMetal.
  It records the first bounded Draw states and asynchronously blits render targets after Clear,
  after Draws, and before presentation, reporting hashes and RGBA histograms only after the
  owning Metal command buffer completes. Disabled cost is one startup-cached env branch.
- The first design added a new PE Unix-call export; runtime proved that transport invalid because
  its lazy import target was not materialized before HB dispatch. That attempt is labelled
  `INVALID_PROBE_TRANSPORT`. The export/import was removed and the probe moved behind the existing
  native `endEncoding` boundary; no new PE ABI remains.
- Arm64 WineMetal and x86_64 D3D11/WineMetal builds PASS. Deployed SHAs are
  `1abd31841fb43e345ccbadb5e4c587acc250a38023419c094e02c0052b6a80f9`,
  `7a89602849920f4126f6c64de6be7c7b60b26ff4f93072642b635469b93d10ab`, and
  `3077596a83bd5d9535f2cae2b06749eb56102a7d2f8c386b8cd10ab693480c20`;
  strict codesign PASS and no `install skipped` was emitted.
- The sealed 1800 s runtime completed 2 after-Clear, 4 after-Draw, and 4 pre-Present readbacks.
  Clear is all-zero; every after-Draw/pre-Present sample is RGB-black with identical hash
  `d69a0ed1d32d0383`, while alpha changes to max 5 / mean 4.167. Real Draw work therefore occurs,
  but the presenter receives zero visible-colour output from upstream.
- Verification: `reports/phase4-hollow-knight/BACKBUFFER-READBACK-RESULT.md`.

### 2026-07-17 — Bounded shader-input and live texture-content diagnostic

- Added default-off `MACRUNNER_HB_SHADER_INPUTS_PROBE` instrumentation at the DXMT argument
  encoder and native WineMetal Draw boundary. It records bounded CB byte hashes/raw IEEE-754
  matrix windows, sampler/SRV metadata, native slot-29/30 table qwords, and resolves bound
  resource IDs to live Metal textures for at most 16 unique readbacks. Disabled env checks are
  startup-cached; no PE/Unix-call ABI was added.
- Arm64 WineMetal and x86_64 D3D11/WineMetal builds PASS. Deployed/codesigned SHAs:
  WineMetal `b392eee839d880070d8c60263ea378ffb42ce665727fbcd8b970e005b7081552`,
  D3D11 `016ea0b6cc417219a71bd200425a5cd0e89f77989fe1120d5f82a811656bc8f0`;
  ntdll remained `487c69ec5e10dd3f8deab861463039811068106d8a5063106aa95013e36b415c`.
  No `install skipped` line was emitted.
- Sealed 1800 s verification: 344/344 CBs are CPU-visible/nonzero; active VS data contains
  finite projection/transform matrices; PS data contains nonzero/white values; 31 bounded scene
  Draws have nonzero Metal argument tables. Two actually bound RGBA8 textures read back 100%
  and 98.05% nonblack RGB. All CPU/JIT/signal fault gates are zero.
- Pixel remains exactly black. The camera/CB/texture/argument-buffer hypotheses are disproved;
  the first open boundary is translated fragment return versus per-PSO blend/color-attachment
  state. The D3D11→Metal color-write-mask static mapping is explicitly correct, but runtime PSO
  values were not captured.
- Verification: `reports/phase4-hollow-knight/SHADER-INPUTS-CAUSE.md`.

### 2026-07-17 — Exact-PSO fragment-output transparency diagnostic (build-only)

- Added default-off, bounded D3D11/WineMetal instrumentation with a shared pointer-free
  PSO identity derived from original stable shader names plus finalized semantic pipeline
  fields. It correlates `PSValidRenderTargets`, actual D3D RT0 format, D3D blend state,
  final native Metal color attachment state, and the bound Draw under one ID.
- Added a fail-closed exact-ID control that replaces only the native fragment function with
  opaque magenta. Blend/write mask, RTV, depth, viewport, VS, and Draw remain unchanged.
  Missing/invalid/nonmatching target IDs never activate the override.
- Added source-contract checks, a separately compiled Metal magenta fixture, and a D3D11
  blend/write-mask fixture covering four sibling state cases. Final target-clean arm64 and
  x86_64 builds PASS; artifact SHAs are recorded in the handoff. No game, fixture runtime,
  install, or deploy was performed, so causality remains `UNKNOWN` pending sealed 1800 s A/B.
- Handoff: `reports/phase4-hollow-knight/FRAGMENT-OUTPUT-MAGENTA-AB-BUILD.md`.

### 2026-07-17 — Fragment-output magenta A/B localizes black RGB

- Added the authorized cached, default-off `MACRUNNER_HB_FORCE_FRAGMENT_MAGENTA`
  diagnostic transport so a sealed A/B can change one child-env variable without a
  silent mismatch against the older exact-ID control. Original PSO IDs and all attachment,
  blend, depth, viewport, VS, and Draw state remain observable and unchanged.
- Source contract and native rebuild/deploy/codesign PASS. This is diagnostic scaffolding,
  not a production workaround.
- Two valid 1800 s runs obtained the exact same six original PSOs and 64 bounded Draws.
  A GPU readback was RGB zero with alpha max 5. B was exactly RGBA 255,0,255,255 for all
  786,432 pixels after Draw and pre-Present. All checked CPU/JIT/signal fault classes were 0.
- Conclusion: runtime blend/write-mask/RTV propagation carries nonzero RGB correctly;
  the open defect is in the original translated fragment result path (output semantics/ABI
  or its consumed shader inputs). No production semantic patch is justified until that
  sub-boundary is measured.
- Verdict: `reports/phase4-hollow-knight/FRAGMENT-OUTPUT-MAGENTA-AB-RESULT.md`.

### 2026-07-17 — Bounded fragment translation byte capture

- Added default-off, bounded `MACRUNNER_HB_FRAGMENT_SHADER_TRANSLATION_PROBE`
  diagnostics. Claimed pixel variants bypass the shader cache only while the probe is
  enabled and preserve exact DXBC, pre/post optimization AIR and metallib artifacts.
  Added a native DXBC parser dump utility and source-contract coverage; shader semantics
  are unchanged.
- Clean arm64/x86_64 builds and deployed checksums passed. One valid requested 1800 s
  timeout captured five real scene fragment variants at the render boundary with zero
  target CPU/JIT fault markers.
- All five declare float `SV_Target0/o0.xyzw` and return a computed packed float4 as AIR
  render target 0. The identity shader `mov o0.xyzw,v1.xyzw` survives as fragment input
  plus the existing tiny UNORM bias; output mapping, missing return and Metal return ABI
  are disproved as the RGB-zero mechanism.
- No production semantic fix was applied. The next evidence boundary is paired vertex
  output versus fragment `user(reg1_0)` interpolation; previous input probes established
  CB/texture transport, not rasterized interpolant values.
- Verdict: `reports/phase4-hollow-knight/FRAGMENT-TRANSLATION-CAUSE.md`.

### 2026-07-17 — Exact interpolant causal ladder fails closed at readback isolation

- Added default-off exact-identity C0–C3 diagnostic variants for logical PSO
  `0x9d2b46c27af732f4`: unchanged draw, final fragment magenta, fragment-input
  magenta, and paired vertex `user(reg1_0)` magenta. Diagnostic physical PSOs do not
  alter the original logical PSO key.
- Focused contracts, diff checks, clean arm64 WineMetal/D3D11 and x86_64 D3D11 builds
  passed. The sealed 1800 s run compiled all variants and matched the exact draw.
- Runtime proved the target is draw 2 of 8 in one render encoder. Encoder-end readback
  is therefore not an immediate result of that draw; C0 rejected the sample and C1–C3
  were intentionally suppressed. Verdict is `UNKNOWN`, and no shader semantic fix was
  applied.
- Next diagnostic must mirror the exact draw result into a dedicated per-draw side
  channel; repeating the same encoder-end backbuffer copy cannot establish causality.
- Verdict: `reports/phase4-hollow-knight/CAUSAL-LADDER-PS-BEF43B20-RESULT.md`.

## 2026-07-17 — DXMT exact-draw fragment side-channel diagnostic (default off)

- Evidence problem: encoder-end RTV readback for `ps_bef43b20` sampled draw 2
  only after draws 3–8, so it could not support a causal shader claim.
- Added default-off `MACRUNNER_HB_CAUSAL_LADDER_BEF43B20` storage
  side-channel: instrumented C0–C3 fragment variants write final `float4` to a
  dedicated shared buffer at Metal fragment slot 28. The exact draw alone binds
  it; prior binding/PSO are restored immediately, with no encoder split or draw
  reorder. Empty/mixed/nonfinite/GPU-error cases fail closed.
- Logical PSO hashing excludes physical diagnostic handles. C0–C3 remain four
  physical PSOs over one original logical PSO/descriptors/draw contract.
- Validation: clean arm64 WineMetal+D3D11 and clean x86_64 D3D11 builds, strict
  codesign/build-dist equality, blend fixture build, three focused source
  contracts, and diff check PASS.
- Runtime: C0 immediate side-channel PASS/BLACK (`655360` viewport pixels), but
  C1 failed `logical-draw-signature-drift`; C2/C3 did not run. Verdict UNKNOWN,
  and no production shader-semantic fix was made. See
  `reports/phase4-hollow-knight/CAUSAL-LADDER-SIDE-CHANNEL-RESULT.md`.

## 2026-07-18 — DXMT exact-draw selector drift telemetry and skip

- Added bounded field-wise expected/observed telemetry for `ps_bef43b20`
  exact-draw signature mismatches.
- Evidence showed the C1 rejection was an over-broad same-PSO selector:
  `index_buffer_offset`, `base_vertex`, `vertex_bindings_hash`, and
  `color_load_action` differed from the C0 draw.
- WineMetal now skips nonmatching same-PSO candidates after C0 and waits for the
  saved exact draw signature. Missing baseline and null physical PSO remain
  fail-closed invalid states.
- Validation: focused source contract PASS, ARM WineMetal rebuild PASS, dist
  artifact SHA-256
  `16d82e06bd7923bf64ff905b6c7569fce7f524360deca8d583b99fad1ef8331f`.
- Runtime: C0 BLACK reproduced, four C1 candidates were skipped, but the exact
  C0 draw did not recur before 1800s timeout. C1/C2/C3 did not run; shader
  causality remains UNKNOWN. See
  `reports/phase4-hollow-knight/CAUSAL-LADDER-DRIFT-FIX-RESULT.md`.

## 2026-07-18 — DXMT ps_bef43b20 per-process causal controls (build only)

- Replaced the one-process boolean ladder gate with the exact default-off selector
  `MACRUNNER_HB_CAUSAL_CONTROL=C0|C1|C2|C3`. All shader variants remain available
  from one binary, but one fresh process can claim only its selected control once.
- Replaced the mutable C0 baseline and pointer-bearing comparator with a canonical
  54-field logical signature. It covers stable shader hashes, draw arguments,
  viewport/scissor bits, exact index-slice contents, and render/pipeline semantics.
  Control PSO selection occurs only after checksum and full-field equality.
- Sealed artifact:
  `reports/phase4-hollow-knight/PS-BEF43B20-C0-LOGICAL-SIGNATURE.json`, FNV-1a64
  checksum `0x07194c46f282d32d`, source run-log SHA-256 `f5dcf159...a66350`.
- Focused source/artifact/binary selector contracts PASS. Clean ARM64 WineMetal +
  ARM64/x86_64 D3D11 builds PASS with zero errors, zero `install skipped`, and no
  warnings in modified files. Build-to-dist byte equality and strict WineMetal
  codesign PASS; staged SHAs are `edb9ea8a...e5ef`, `cefb3e3f...4f24`, and
  `faef00f6...cd61`.
- Two target-clean passes were not byte-reproducible, so deterministic rebuild is
  explicitly not claimed. No Hollow Knight process ran. C0-C3 remain runtime
  `UNKNOWN`; forced magenta is a `NOT_GOLDEN` forensic checkpoint only.

## 2026-07-18 - Default-off DXMT vertex-data provenance probe

- Added `MACRUNNER_HB_VERTEX_DATA_PROBE=1` as a bounded, default-off diagnostic.
  It records D3D11 Map/Unmap and UpdateSubresource source bytes, IA layout and
  vertex binding, the actual DXMT allocation encoded into slot 16, and semantic
  WineMetal draw/index fields. It does not modify vertex data or render state.
- Exact draw selection remains pointer-free. Buffer/resource pointers are emitted
  only for the intra-process upload-to-binding provenance join.
- The first diagnostic process was rejected for exact identity because the new
  gate retained render state but not `MacRunnerPipelineProbeInfo`. Added the
  vertex-only gate to pipeline metadata creation and a regression assertion.
- The draw index hash initially reused a legacy noncanonical FNV helper. The final
  source uses the canonical causal-signature hash; raw recorded indices from the
  runtime recompute to the sealed C0 value.
- Added `tools/analyze_dxmt_vertex_data_probe.py` and
  `tools/test_dxmt_vertex_data_probe_source.py`. The analyzer fails closed when
  the semantic shader/PSO/draw identity is unavailable.
- Valid 1800-second evidence proves Unity writes `(0,0,0,5/255)` through Map/Unmap
  and DXMT binds the same allocation as stride 88 with `reg1` at byte offset 24.
  Colored vertex buffers exist globally, so no vertex upload/binding semantic fix
  is warranted. Remaining content boundary is Unity scene/bootstrap.
- Final clean build: errors 0, `install skipped` 0, modified-during-build warnings
  0; ARM64 WineMetal/D3D11 and x86_64 D3D11 build-to-dist `cmp` PASS, strict
  WineMetal codesign PASS. Full evidence:
  `reports/phase4-hollow-knight/VERTEX-DATA-BLACK-CAUSE.md`.

## 2026-07-19 — HK language observer admission and diagnostic fallback

- Extended the x64 DLL callback corridor with a generic `arg0` and made exact registered original AMD64 executable sections precede broad builtin/LDR module ranges. Overlap resolution chooses the narrowest section and uses a bounded generation-aware cache. This fixed observer callbacks being rejected as `c000007b` when nested inside `dynamic-builtin.dll` ranges.
- Focused source tests PASS. Unix ntdll deterministic A/B build, build-to-selected-dist equality and deploy PASS at SHA-256 `e1037bfae9e41108946aeb96c970c943853c46bb55ba25ce179564033f138123`; errors 0, `install skipped` 0, changed-region warnings 0.
- The read-only language observer now has a separate default-off `MACRUNNER_HB_LANGUAGE_DIAGNOSTIC_DIRECT_CONFIRM=1` path. It captures the allocated StartManager object with a pinned GC handle, resolves `ConfirmLanguage` once, removes the allocation callback immediately, and invokes only after exact `HighlightDefault`. Every diagnostic record is marked `NOT_GOLDEN`; normal observer runs never enable allocation profiling or managed invocation.
- Observer/input focused tests PASS. Deterministic fallback builds E/F and immutable/selected dist are byte-identical SHA-256 `289c02b858bc7af289e65025b5623e678e8ccf76c09df50cfb5d5a39deff7e6f`; PE entrypoint is zero and imports are KERNEL32-only.
- Runtime A/A PASS. Sealed real-input B posted the first Return to the proven HK PID but Unity did not execute `SetLanguage`; the remaining two events were withheld and the strict result is `UNKNOWN` at input delivery. Separate direct fallback invoked `ConfirmLanguage` exactly once and returned `ok`, but activation/managers/pixel remained absent/BLACK. No production fix or snapshot was made. Evidence: `reports/phase4-hollow-knight/LANGUAGE-INPUT-AB-VERIFY.md`.

## 2026-07-19 — Return-only host-to-Unity route observer

- Added default-off `MACRUNNER_HB_RETURN_ROUTE_OBSERVER=1`, bounded to 64 records by default (hard max 256), across macdrv NSEvent receipt/dequeue/vkey mapping, win32u send and WM_INPUT dequeue, Wine server raw-input enqueue, Unity raw data/buffer and async/key-state queries, plus managed SetLanguage entry. The selector suppresses the legacy direct Confirm diagnostic and emits zero Return telemetry while disabled.
- Static UnityPlayer analysis proved keyboard Raw Input registration `{1,6}` and its GetRawInputData/GetRawInputBuffer callsites; DirectInput is not the keyboard route. Added a sealed host controller that can publish only one Return down/up pair after exact `HighlightDefault` readiness.
- Focused Return controller/observer and language observer regression tests PASS. Canonical build completed with errors 0 and `install skipped` 0. Six selected Wine build/dist pairs remain byte-identical; profiler build/dist/sealed SHA-256 is `982151aba788ef21ee6669f0a374f8cd5d0fae229182fe7cc21129a48f8e719a`.
- Runtime A/A reproduced readiness with zero input. Sealed B posted keycode 36 down/up once to HK PID 37712, but `target_frontmost=false` and macdrv observed zero NSEvents; all downstream input and SetLanguage counters were zero. Because host-side target HWND/key-window/native-tid coverage is incomplete, verdict is UNKNOWN. No ConfirmLanguage call, activation change, production fix, or snapshot was made. Evidence: `reports/phase4-hollow-knight/RETURN-TO-SETLANGUAGE-SEALED-AB.md`.

## 2026-07-19 — Return focus-activation boundary telemetry

- Extended bounded Return telemetry in winemac.drv with CG/Cocoa window IDs,
  Cocoa-to-HWND mapping, foreground/active/focus HWNDs, `NSApp` active,
  key/main window identity, and macdrv native tid. Managed readiness now records
  PID, Wine/native tid join data, and GUI HWND state.
- Added a sealed controller mode using `NSRunningApplication` activation for the
  proven HK PID. B waits for frontmost + exact key NSWindow + exact focus HWND;
  no proof means no Return. No AX/HID/global-CG/Wine-focus or managed-confirm
  path was added.
- Four focused source tests PASS. Targeted macdrv build completed with errors=0
  and `install skipped`=0. Selected PE, Unix and observer build/dist pairs are
  byte-identical at `a43181d…`, `82c6777…`, and `ca5a6e…` respectively.
- Runtime could not admit the comparison: A1 at 1350s and the authorized 1800s
  fallback both missed `HighlightDefault` and exact target mapping. Zero Return
  events were posted; A2/B were stopped. Verdict UNKNOWN, not a focus/activation
  finding. Evidence:
  `reports/phase4-hollow-knight/FOCUS-ACTIVATION-SEALED-AB.md`.

## 2026-07-19 — Default-off Return focus-milestone observer selector

- Added cached, default-off
  `MACRUNNER_HB_RETURN_ROUTE_FOCUS_MILESTONES`. It gates exactly one
  `macdrv-window-map` and two `macdrv-focus-state` observer calls before their
  Cocoa/Win32 state-query path; all three call sites are covered.
- ON preserves the existing call positions/arguments. Common Return stages and
  passive `TRACE_WINSHOW` markers are unchanged. The helper has no activation,
  focus, input, managed-state, or timing side effects.
- Focused selector, Return observer, and language observer tests PASS. Two
  isolated snapshot builds are byte-identical: PE `3b506d3…`, Unix `65bf6bf…`;
  errors=0 and `install skipped`=0. Working dist was not modified.
- Sealed as a 2.0 MiB NOT_GOLDEN bundle with a replayable minimal diff,
  toolchain/env manifest, two build sets/logs, and standalone verifier. Runtime
  is not authorized or executed. Evidence:
  `reports/phase4-hollow-knight/NEW-FOCUS-OBSERVER-BASELINE-BUILD-CONTRACT.md`.

## 2026-07-25 - HK Ring Ledger V3 and DXMT draw trace

- `macrunner_hb.c`: retain V2 ledger records while gating the four capture slots
  on observed Gfx watchlist opcodes (`10007`, `10028`, `10035`, `10057`, `10123`)
  and counting skipped fallback records without consuming capture budget.
- `d3d11_context_impl.cpp`: add logging-only counters and bounded entry traces
  for Draw/DrawIndexed/DrawInstanced/DrawIndexedInstanced/ClearRenderTargetView.
- Built successfully, not installed: ntdll SHA-256 `b908d24d462aede71c480a3fe360038eaff1a2ed1a17ca38c9a8737170e697fd`;
  DXMT d3d11 SHA-256 `496e9846aa8d6677f3bd2cd0fafb0398e23cb31fcb18eda9b8e70d3ee4a4a34b`.
- The live V2 game process (PID `54774`) was preserved; no `dist` mutation,
  game run, test, or commit occurred.

## 2026-07-25 - HK Ring Ledger V4 recovered-layout witness

- `macrunner_hb.c`: fallback ledger now derives stream state from `R14` and
  dispatcher last-command from `RCX+0x48`; it records a fresh, bound-checked
  `base + (cursor_after - 4)` read alongside guest `EDX` and an equality witness.
- Reads of the next words are constrained by the recovered `stream+0x10c`
  limit. Existing arming, watchlist, four-slot cap, and DXMT trace are retained.
- `ntdll.so` built successfully as
  `85022a049f2ce8c486f9ccd30b409b4d07dac74659a0ef07a06085c652ed7b0f`.
  Coherent-dist installation was blocked before mutation because an external HK
  run was live; no game was launched and no commit was made.

## 2026-07-25 - DXMT bounded Present frame dump

- `d3d11_swapchain.cpp` selects only the first five real Presents and every
  200th Present when `MACRUNNER_DXMT_FRAME_DUMP=1` is set; it otherwise has no
  effect on the present path.
- `dxmt_context` carries that selected frame to the real Metal command buffer.
  `winemetal` encodes a texture-to-shared-buffer blit before the presenter and
  writes raw RGBA plus RGB statistics from the completion handler, without a
  synchronous GPU wait.
- Published artifacts: x86 `d3d11.dll`
  `172ddbdba215a66d4974b07ccd9d6403400bf330d3cc68d112d1c33099113f41`,
  x86 `winemetal.dll`
  `80a6dc36d65468ed047bac45bd27b775f896f868866793a1f484c3521b0b1440`,
  and arm64 Unix bridge `winemetal.so`
  `7da877915020038c2304732e802999d02d546e4e02defddcf14dd85388ee28fa`.
- Build-only: no HK/Wine runtime, cache mutation, commit, or GOLDEN claim.

## 2026-07-26 - C0 sidechannel full-grid statistics

- Extended only the proven C0 causal sidechannel in
  `engine/dxmt/src/winemetal/unix/winemetal_unix.c`. With the default-off
  `MACRUNNER_HB_CAUSAL_SIDECHANNEL_GRID=1` gate, its existing shared-buffer
  completion handler emits one full-coverage 1024x768 RGB summary: sample
  counts, min/max/mean, and nonzero fraction.
- No texture-to-buffer blit, new command-buffer completion path, file output,
  or synchronization was introduced. The optional work is post-completion CPU
  analysis of the same C0 buffer already used by the BLACK sidechannel result.
- Published runtime Unix bridge:
  `engine/graphics/dist/dxmt/aarch64-unix/winemetal.so`, SHA-256
  `1b2ba54a9f3580097dc15931de24bb12575b192d0707d6e92ba98a21ad807203`.
  The source contract passed and each gate/log-format marker is present once.
- Build-only: no HK/Wine runtime, cache mutation, commit, or GOLDEN claim.

## 2026-07-27 09:05 — Interpreter honors is_locked (LOCK-prefix fence parity with codegen)
File(s): engine/hyperbridge/src/hb_interpreter.c (exec_instr wrapper, ~line 3245)
Type: ROOT-FIX (correctness parity; proven INERT for the HK stall)
What: exec_instr now brackets every is_locked IR instr with __atomic_thread_fence(SEQ_CST)
  before+after, mirroring codegen's DMB ISH bracket (hb_arm64_codegen.c); adds bounded
  aggregate trace MACRUNNER_HB_TRACE_INTERP_LOCK=1 (armed line + first-8/2^20 counts).
Why: lifter marks LOCK-prefixed ops is_locked; codegen fenced them; interpreter consulted
  the flag nowhere — generic locked RMW (`lock or [rsp],r`, Mono hazard-pointer idiom)
  ran fence-free through the live helper path. Suspected HK stall candidate.
Verify: control tools/hb_interp_lock_control.c PASS trace+notrace (semantics 0x30|7=0x37,
  exactly 1 count line, unlocked twin adds none); suite 27 fails == reverted-baseline
  (1 flaky slot rotates without the fix too); live run9: armed=1, ZERO locked instrs in
  25 min — refutes it as the stall mechanism, fix stays as parity.
Status: applied

## 2026-07-27 09:47 — winemac client surface parenting when GA_ROOT is NULL (compositor gap)
File(s): engine/wine/dlls/winemac.drv/window.c (macdrv_client_surface_update)
Type: ROOT-FIX
What: toplevel fallback — if NtUserGetAncestor(hwnd, GA_ROOT) returns NULL, use hwnd
  itself for the get_win_data lookup (semantically identical for top-level windows);
  trace line macrunner-winrealize: client_surface_update toplevel_fallback.
Why: Unity creates the DXGI swapchain while the win32 window is mid-realization;
  GA_ROOT=NULL made client_surface_update silently no-op, so the swapchain client
  surface's Cocoa view was NEVER parented while present() unhid it. Every Present
  rendered into a detached view; the visible window stayed black forever
  (HK run8: readback magenta vs window black; lldb: bound view superview=nil).
  Retroactively voids all window-level "black screen" observations to date.
Verify: run10 live — fallback fired 2x at swapchain time; lldb: view parented into
  the visible window; SCK window capture nonblack=13924 (was 0); menu visible on
  screen; ordinal-200 readback byte-identical to run5/6 (detector parity).
Status: applied

## 2026-07-28 12:20 — HK input root: NSApp is stock NSApplication; winemac recovery fix + thief hook
File(s): engine/wine/dlls/winemac.drv/cocoa_main.m (run_cocoa_app recovery branch + sharedApplication hook + constructor probe);
  trace helpers + 12 stage points in cocoa_window.m/keyboard.c/window.c/cocoa_app.m; gate MACRUNNER_TRACE_WINEMAC_INPUT
  added to all 8 trace_ui_input_enabled helpers (event.c, cocoa_event.m, mouse.c, macdrv_main.c, cocoa_app.m,
  cocoa_window.m, keyboard.c, window.c, cocoa_main.m)
Type: ROOT-FIX (pending run verification) + DIAGNOSTIC
What:
  1) run_cocoa_app: if NSApp pre-exists and is NOT WineApplication, install WineApplicationController as
     NSApp delegate + swizzle -[NSApplication sendEvent:] with macrunner_sendEvent_stock_recovery
     (same logic as -[WineApplication sendEvent:] but via sharedController singleton), then enter [NSApp run].
  2) Thief hook: constructor swizzles +[NSApplication sharedApplication] to log first caller class + stack.
  3) Input-chain trace: macrunner-ui-input stages across keyDown/postKey/focus/activation (env MACRUNNER_TRACE_WINEMAC_INPUT).
Why: Live lldb on try5 (laneA-INPUT-TEST-try5-112123, pid 15041): NSApp class=NSApplication, superclass=NSResponder,
  delegate=nil, no setWineController → upstream run_cocoa_app then skips controller install and never enters
  [NSApp run] → sendEvent/handleEvent dead → keyboard+mouse never enter Wine (operator's menu dead-input root).
  Wine-side queue proven alive (lldb [win keyDown:] → postKey_posted KEY_PRESS in run.log); consumer never
  dequeues (JIT spin, MONO lane territory). Also live on run35A pid 34444: winemac.so loaded, driver init
  pending, NSApp ALREADY stock → thief acts between winemac.so dlopen and run_cocoa_app.
Verify: try5 evidence above; fix+hook to be verified in try7 (queued behind run35A): expect
  stage=first_sharedApplication (thief identity) + stage=run_cocoa_app_stock_recovery + input flow.
Status: applied (dist-arm64ec-spike winemac.so, adhoc re-signed, strings-verified), needs-verify
