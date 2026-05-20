## Engine Rendering Audit: Black Squares on Notepad++ Tab Close Buttons / Modified Indicators

**Status**: Read-only audit — findings to be forwarded to Codex for implementation  
**Date**: 2026-05-21  
**Auditor**: Kimi  
**Confidence**: High

---

### Symptom Description
In Notepad++ under MacRunner (Wine on macOS ARM64), the tab control overlay elements — specifically the close button and the modified-file indicator (dot) on individual tabs — render as solid black squares instead of their intended glyph shapes. Standard tab icons and text render correctly.

---

### Code Path Traced

1. **comctl32 tab control paint entry**  
   `TAB_DrawItemInterior` in `engine/wine/dlls/comctl32/tab.c:1548`  
   The standard Windows tab control has no native close-button support. Notepad++ uses `TCS_OWNERDRAWFIXED`. At line 1707 the tab control sends `WM_DRAWITEM` to the parent window (`TabBarPlus`), which then performs all custom drawing.

2. **Notepad++ owner-draw path**  
   Notepad++ `TabBarPlus` draws the close button and modified indicator via `ImageList_Draw` with the `ILD_TRANSPARENT` flag.

3. **ImageList draw dispatcher**  
   `ImageList_DrawIndirect` in `engine/wine/dlls/comctl32/imagelist.c:1572`

4. **Mask-based transparent branch (the fault path)**  
   Lines 1691–1713 of `imagelist.c`:
   ```c
   COLORREF colour = RGB(0,0,0);
   ...
   hOldBrush = SelectObject (hImageDC, CreateSolidBrush (colour));
   PatBlt( hImageDC, 0, 0, cx, cy, PATCOPY );
   if (himl->hbmMask)
   {
       BitBlt( hImageDC, 0, 0, cx, cy, hMaskListDC, pt.x, pt.y, SRCAND );
       BitBlt( hImageDC, 0, 0, cx, cy, hImageListDC, pt.x, pt.y, SRCPAINT );
   }
   else
       BitBlt( hImageDC, 0, 0, cx, cy, hImageListDC, pt.x, pt.y, SRCCOPY);
   ```
   This initializes a temporary bitmap to **solid black**, then composites the mask (`SRCAND`) and the source image (`SRCPAINT`) onto that black surface.

5. **Final destination copy**  
   Lines 1765–1777:
   ```c
   dwRop = SRCCOPY;
   if (himl->hbmMask && bIsTransparent ) {
       ...
       BitBlt (pimldp->hdcDst, pimldp->x, pimldp->y, cx, cy, hMaskListDC, pt.x, pt.y, SRCAND);
       dwRop = SRCPAINT;
   }
   BitBlt (pimldp->hdcDst, pimldp->x, pimldp->y, cx, cy, hImageDC, 0, 0, dwRop);
   ```
   The temp bitmap — which still contains black in any pixel where the source image data was zero or where the mask operation produced zero — is copied to the destination with `SRCCOPY`/`SRCPAINT`.

6. **ImageList creation / mask initialization**  
   `ImageList_AddMasked` in `imagelist.c:780-839`. For non-32bpp images, line 827 applies `NOTSRCAND` (`0x220326`) to the source bitmap:
   ```c
   SetBkColor(hdcBitmap, RGB(255,255,255));
   BitBlt(hdcBitmap, 0, 0, bmp.bmWidth, bmp.bmHeight, hdcMask, 0, 0, 0x220326);
   ```
   This operation modifies the stored image bitmap. Depending on the original pixel values and mask color, opaque areas can end up as black in the imagelist's internal bitmap.

7. **GDI driver fallback on macOS**  
   `winemac.drv` does not implement `pAlphaBlend`, `pBitBlt`, or `pPutImage`. All GDI blitting falls through to the DIB driver (`dibdrv` / `nulldrv`), so every ROP code in the path above is executed by Wine's generic bitmap engine rather than a platform-specific optimized path.

---

### Root Cause Analysis

When `ImageList_DrawIndirect` processes an icon that has **no true alpha channel** (`has_alpha = 0`) and is drawn with `ILD_TRANSPARENT`, it takes the mask-based branch (lines 1691–1713). The temporary bitmap is filled with black. The subsequent `SRCAND` + `SRCPAINT` sequence leaves transparent pixels as **black** on the temporary surface, because the source image stored in the imagelist itself already contains black in those regions (either because `ImageList_AddMasked` set them to black via `NOTSRCAND`, or because the original 32bpp bitmap passed in was black/transparent with zero alpha and no meaningful RGB data).

The final `BitBlt` to the destination then copies those black pixels verbatim. On native Windows, the same code path may behave differently because the display driver or the internal GDI mask handling resolves the monochrome mask blits with different background/text color expansion, or because the app relies on `MaskBlt` behavior that Wine emulates via separate `BitBlt` steps. In Wine on macOS, the pure DIB driver faithfully executes each ROP, and the result is a solid black rectangle when the imagelist's source image lacks non-zero color data in opaque areas.

**Trace evidence** (`reports/phase-h/npp-x64-20260521-032110/stderr.log`) shows imagelist items with `has_alpha=0` being drawn with `ILD_TRANSPARENT`, and the internal pixel traces (`hdc_pixels`) confirm that after `ImageList_AddMasked` the color bitmap for affected indices is entirely black (`sample0=0x000000`, `colorful_pixels=0`, `has_alpha=0`).

---

### Suggested Fix Scope

Target file: `engine/wine/dlls/comctl32/imagelist.c`

**Option A — Short-term / minimal risk:**  
In `ImageList_DrawIndirect` (around line 1693), change the initialization color of the temporary bitmap for the transparent mask path from `RGB(0,0,0)` to the actual destination background color (e.g., `GetBkColor(pimldp->hdcDst)` or `GetPixel(pimldp->hdcDst, pimldp->x, pimldp->y)`). This ensures that transparent pixels in the temp bitmap match the tab background instead of defaulting to black, so even if the mask/image composite leaves some pixels untouched, they blend with the background rather than producing a black square.

**Option B — Correct / robust:**  
Avoid the temp-bitmap + `SRCCOPY` approach entirely for `ILD_TRANSPARENT` with a valid mask. Use `TransparentBlt` or `MaskBlt` directly on the destination DC. This eliminates the intermediate surface and its black-fill artifact, matching the behavior that native Windows GDI drivers provide internally.

**Option C — Data fix:**  
In `ImageList_AddMasked`, preserve original RGB data in the stored image bitmap instead of destroying it with `NOTSRCAND`. However, the code comments explicitly note this is a documented Windows bug that apps rely on, so changing it carries high compatibility risk.

---

### Confidence Level

**High** for the general mechanism (mask-based `ImageList_Draw` with `ILD_TRANSPARENT` produces black artifacts when the imagelist source image is all-black or lacks alpha).  
**Medium-High** for the macOS specificity — the same Wine `dibdrv` code runs on Linux, but the symptom may be hidden there because X11 drivers handle monochrome-to-color `BitBlt` expansion differently, whereas on macOS the path is forced through the generic DIB engine where the ROP math is exact and exposes the black-fill artifact.
