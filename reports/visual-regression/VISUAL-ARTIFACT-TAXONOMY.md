# Visual Artifact Taxonomy

## Philosophy
Every visual defect is either a real runtime bug or a false-negative from the capture/analysis path. The goal is to classify which before any runtime patch.

---

## 1. CLIENT_BOTTOM_BLACK_BAND

**Description:** A contiguous black or near-black band at the bottom edge of the client area, taller than normal window borders.
**Likely root layer:** WINE (statusbar paint, background erase, dirty rect handling) or WINE_LAYOUT (client rect mismatch).
**Required probe:** `main_window_cg_capture` + `ui_metrics`
**Required evidence:** CG capture showing bottom band > 16 px contiguous black pixels.
**Safe Codex next action:** Compare client rect / editor rect / statusbar rect against CG band. Trace `WM_ERASEBKGND`, `DrawThemeBackground` for statusbar part/state.
**What not to fix:** Do not shrink window size blindly; verify the band is not a normal macOS shadow or Wine desktop border.

---

## 2. CLIENT_RIGHT_BLACK_BAND

**Description:** A contiguous black band at the right edge of the client area.
**Likely root layer:** WINE (scrollbar background, right-edge dirty rect) or LAYOUT (editor not filling full client width).
**Required probe:** `main_window_cg_capture` + `ui_metrics`
**Required evidence:** CG capture showing right band > 12 px contiguous black pixels.
**Safe Codex next action:** Compare editor width against client width minus scrollbar width. Trace scrollbar `DrawThemeBackground` calls.
**What not to fix:** Do not assume the band is a scrollbar issue if the app does not use a right scrollbar.

---

## 3. SCROLLBAR_BLACK_ARTIFACT

**Description:** Scrollbar thumb or track renders as solid black or near-black instead of native gradient/texture.
**Likely root layer:** WINE_UXTHEME (scrollbar part/state drawing) or WINE_GDI (mask/alpha handling in `DrawThemeBackground` → `pPatBlt`).
**Required probe:** `main_window_cg_capture` zoomed on scrollbar region.
**Required evidence:** CG capture zoom shows scrollbar area with zero colorful pixels and high black ratio.
**Safe Codex next action:** Trace `uxtheme` scrollbar part `DrawThemeBackground` destination HDC. Check `pPatBlt` with `ROP=0` (black fill) in `gdi32/bitblt.c`.
**What not to fix:** Do not patch `uxtheme` blindly; verify the part/state ID is correct and the destination DIB is writable.

---

## 4. FILE_DIALOG_FOLDER_ICONS_BLACK

**Description:** Open/Save dialog folder icons render as black silhouettes instead of colored/yellow folder icons.
**Likely root layer:** WINE_COMCTL32 (ImageList_Draw with mask loss) or WINE_GDI (`GetIconInfo` returning black `hbmColor`, alpha mask incorrectly applied).
**Required probe:** `icon_geticoninfo_trace` + CG capture of dialog.
**Required evidence:** CG capture shows dialog with ListView/TreeView containing black icons; `GetIconInfo` trace shows `hbmColor` black or mask inverted.
**Safe Codex next action:** Trace `HICON` → `GetIconInfo` → `DrawIconEx` → ImageList `_Draw` → ListView/TreeView custom draw. Check mask inversion and alpha premultiplication.
**What not to fix:** Do not patch `shell32` icon loading; the icon resource itself may be correct and the loss happens at draw time.

---

## 5. TOOLBAR_BLACK_OR_LOW_COLOR

**Description:** Toolbar buttons show gray/black placeholder squares or very low colorful pixel count instead of full-color icons.
**Likely root layer:** WINE_COMCTL32 (ImageList checked fill / toolbar background) or HELPER (bitmap readback false-negative).
**Required probe:** `toolbar_visual_cg` + `win32_helper_capture`
**Required evidence:** CG capture shows toolbar region with `colorful_pixels` below `colorful_floor` threshold; OR CG shows colored icons but helper capture shows black.
**Safe Codex next action:** If CG < floor → fix `comctl32` fill / ImageList / `GetIconInfo` path. If CG ≥ floor but helper shows black → fix helper readback, not runtime.
**What not to fix:** Do NOT instrument `comctl32/toolbar.c` directly (caused c000007b regressions).

---

## 6. STATUSBAR_BAD_BACKGROUND

**Description:** Statusbar background is black or wrong color instead of matching the window chrome / theme.
**Likely root layer:** WINE_UXTHEME (statusbar part background) or WINE_GDI (background erase before paint).
**Required probe:** `main_window_cg_capture`
**Required evidence:** Bottom ~20 px band shows solid black or non-matching color compared to menu/toolbar chrome.
**Safe Codex next action:** Trace `WM_ERASEBKGND` → `DefWindowProc` → `FillRect` / `DrawThemeBackground` for statusbar part. Compare against Windows baseline.
**What not to fix:** Do not change the statusbar height blindly; verify the color mismatch is not a theme difference between macOS and Windows.

---

## 7. STATUSBAR_BAD_TEXT_BASELINE

**Description:** Statusbar text is misaligned, clipped, or invisible because baseline/height metrics are wrong.
**Likely root layer:** WINE_GDI (`ExtTextOutW` with wrong `y` coordinate, `GetTextMetricsW` returning incorrect `tmHeight`/`tmAscent`).
**Required probe:** `ui_metrics` + manual CG inspection.
**Required evidence:** Text characters partially visible or entirely missing in statusbar region while background renders.
**Safe Codex next action:** Compare `GetTextMetricsW` output against Windows baseline. Trace `ExtTextOutW` `x,y` coordinates for statusbar text.
**What not to fix:** Do not assume font substitution is the problem; check the text baseline first.

---

## 8. MENU_TOO_TALL

**Description:** Menu bar or top-level chrome is taller than native Windows baseline, pushing editor area down.
**Likely root layer:** WINE_LAYOUT (non-client metrics) or WINE_GDI (menu font metrics).
**Required probe:** `ui_metrics`
**Required evidence:** Top bar height from CG capture exceeds known Windows baseline by > 20%.
**Safe Codex next action:** Measure non-client metrics (`GetSystemMetrics` for `SM_CYMENU`, `SM_CYCAPTION`). Compare against Windows baseline.
**What not to fix:** Do not reduce menu height without a baseline; some apps intentionally use taller menus.

---

## 9. TOOLBAR_TOO_TALL

**Description:** Toolbar band is excessively tall, consuming editor real estate.
**Likely root layer:** WINE_LAYOUT (toolbar sizing message not honored) or WINE_COMCTL32 (toolbar auto-sizing with wrong metrics).
**Required probe:** `ui_metrics`
**Required evidence:** Toolbar band height from CG exceeds baseline by > 20% or shows large empty vertical padding.
**Safe Codex next action:** Trace `TB_SETBUTTONSIZE`, `TB_SETBITMAPSIZE`, `TB_AUTOSIZE` messages. Compare against Windows baseline.
**What not to fix:** Do not override toolbar size from helper; trace the actual message path.

---

## 10. EDITOR_NOT_FILLING_CLIENT

**Description:** Editor surface does not extend to the full available client area, leaving empty bands on any edge.
**Likely root layer:** WINE_LAYOUT (child window positioning) or PRODUCT (app intentionally reserves space for docking panels).
**Required probe:** `main_window_cg_capture` + `win32_helper_capture`
**Required evidence:** Editor HWND rect is smaller than client rect minus known chrome (menu, toolbar, statusbar, scrollbars).
**Safe Codex next action:** Compare `GetClientRect(main)` with `GetWindowRect(scintilla)`. Check if the gap is intentional (docking, sidebars) or a layout bug.
**What not to fix:** Do not resize the editor from outside the app; trace the app's own layout messages.

---

## 11. HELPER_CAPTURE_FALSE_NEGATIVE

**Description:** Helper-side bitmap readback or pixel analysis reports failure while CG capture shows correct visual.
**Likely root layer:** HELPER (capture timing, format mismatch, readback path).
**Required probe:** `toolbar_visual_cg` + `win32_helper_capture`
**Required evidence:** CG colorful_pixels ≥ floor but helper reports FAIL or low colorful count.
**Safe Codex next action:** Replace or fix helper bitmap readback. Use CG as ground truth. Do NOT patch runtime.
**What not to fix:** Do not change Wine or engine based on helper-only evidence.

---

## 12. CG_CAPTURE_UNAVAILABLE

**Description:** CoreGraphics screenshot failed or returned no image.
**Likely root layer:** HELPER / INFRA (sandbox, permissions, window not yet visible).
**Required probe:** `main_window_cg_capture`
**Required evidence:** `screencapture` returns error or output file missing / zero bytes.
**Safe Codex next action:** Check sandbox permissions, window visibility, and timing. Add retry with longer readiness delay.
**What not to fix:** Do not classify as product failure when CG capture is missing.

---

## 13. WINEMAC_PRESENT_DIRTY_RECT_BUG

**Description:** `winemac` driver leaves unpainted black areas because dirty rect updates are incomplete.
**Likely root layer:** WINE (macOS display driver `winemac.drv`)
**Required probe:** `main_window_cg_capture` over time.
**Required evidence:** Black bands appear or disappear after window resize/minimize/restore. Not reproducible on Windows.
**Safe Codex next action:** Trace `WM_PAINT` → `BeginPaint` → `GetUpdateRect` in `winemac.drv`. Compare dirty rect against actual invalid region.
**What not to fix:** Do not force full-window repaint from app code; fix the driver.

---

## 14. UTHEME_BLACK_BACKGROUND

**Description:** Theme-drawn backgrounds (dialogs, buttons, panels) render black instead of themed gradient/color.
**Likely root layer:** WINE_UXTHEME (part/state background drawing) or WINE_GDI (destination DIB color table).
**Required probe:** `main_window_cg_capture`
**Required evidence:** Dialog or panel background is solid black where Windows shows a textured/colored background.
**Safe Codex next action:** Trace `DrawThemeBackground` for the specific part/state. Check `pPutImage` / `StretchDIBits` color table and mask.
**What not to fix:** Do not replace themed drawing with solid fills; fix the theme rendering path.

---

## 15. GETICONINFO_COLOR_LOSS

**Description:** `GetIconInfo` returns a black `hbmColor` bitmap for a valid color icon.
**Likely root layer:** WINE_GDI (icon extraction, mask handling) or WINE_COMCTL32 (ImageList icon creation).
**Required probe:** `icon_geticoninfo_trace`
**Required evidence:** Trace shows `GetIconInfo` called with a valid HICON but `hbmColor` is solid black or mask-only.
**Safe Codex next action:** Trace `CreateIconIndirect`, `CopyImage`, `GetIconInfo` in `user32`. Check if the icon was created from a 32bpp DIB with alpha.
**What not to fix:** Do not assume the icon file is corrupted; the loss happens at API level.

---

## 16. IMAGELIST_MASK_ALPHA_BUG

**Description:** `ImageList_Draw` or `ImageList_GetIcon` loses color because the mask is applied incorrectly or alpha is treated as mask.
**Likely root layer:** WINE_COMCTL32 (ImageList internal mask/alpha logic) or WINE_GDI (BitBlt with `SRCAND`/`SRCPAINT` on 32bpp).
**Required probe:** `icon_geticoninfo_trace` + `gdi_blit_trace`
**Required evidence:** ImageList operations on 32bpp icons produce black where color should be visible.
**Safe Codex next action:** Trace `ImageList_Draw` → `_Draw` → `BitBlt` with `SRCAND` then `SRCPAINT`. Check if alpha channel is treated as mask.
**What not to fix:** Do not replace ImageList with custom drawing; fix the mask/alpha interpretation.
