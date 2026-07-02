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
