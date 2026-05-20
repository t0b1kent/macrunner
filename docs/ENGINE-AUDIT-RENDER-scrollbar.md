## Engine Rendering Audit: Black Scrollbar Artifacts in Notepad++

**Status**: Read-only audit — findings to be forwarded to Codex for implementation  
**Date**: 2026-05-21  
**Auditor**: Kimi  
**Confidence**: High

---

### Symptom
When clicking in the scrollbar track (above or below the thumb), the pressed track area renders with black pixels or black streaks along the right/bottom edge. The artifacts persist or flicker during interaction. This is especially visible on MacRunner because Wine on macOS falls back to classic (non-themed) scrollbar drawing.

---

### Code Path Traced

1. **Scrollbar draw initiated**
   - `win32u/scroll.c:361` — `draw_scroll_bar()` calls `KeUserModeCallback( NtUserDrawScrollBar, ... )`

2. **User-mode callback dispatches to themed or classic drawer**
   - `user32/user_main.c:54` — `User32DrawScrollBar` calls `user_api->pScrollBarDraw()`
   - `uxtheme/scrollbar.c:54` — `OpenThemeDataForDpi(NULL, WC_SCROLLBARW, ...)` attempts to open a theme
   - `uxtheme/system.c:634` — `bThemeActive` is FALSE by default on Wine/macOS (no msstyles theme loaded), so `open_theme_data` returns `NULL`
   - `uxtheme/scrollbar.c:56-64` — Fallback to `user_api.pScrollBarDraw` = `USER_ScrollBarDraw` in `user32/scroll.c`

3. **Classic scrollbar interior drawing**
   - `user32/scroll.c:177` — `USER_ScrollBarDraw` calls `SCROLL_DrawInterior()`
   - `user32/scroll.c:96-105` — `COLOR_3DHILIGHT` == `COLOR_WINDOW` (both white), so `hBrush = SYSCOLOR_Get55AABrush()` (1bpp checkerboard pattern brush)
   - `user32/scroll.c:113` — Black pen `COLOR_WINDOWFRAME` selected for frame/ thumb border

4. **The critical bug: invalid ROP code on track click**
   - `user32/scroll.c:148` — `PatBlt( ..., top_selected ? 0x0f0000 : PATCOPY )`
   - `user32/scroll.c:152` — Same for `bottom_selected`
   - `user32/scroll.c:159` — Horizontal variant
   - `user32/scroll.c:162` — Horizontal variant

5. **DIB engine handling of `0x0f0000`**
   - `win32u/bitblt.c:611` — `NtGdiPatBlt` forwards to `physdev->funcs->pPatBlt`
   - `win32u/dibdrv/graphics.c:1178` — `dibdrv_PatBlt` computes `rop2 = get_rop2_from_rop(0x0f0000)`
   - `win32u/dibdrv/graphics.c:1170` — `get_rop2_from_rop` extracts `rop2 = 4` = `R2_NOTCOPYPEN`

6. **Pattern brush mask creation for `R2_NOTCOPYPEN`**
   - `win32u/dibdrv/objects.c:1841` — `create_pattern_brush_bits` does NOT short-circuit (unlike `R2_COPYPEN`)
   - `win32u/dibdrv/objects.c:1853` — Allocates XOR + AND mask buffer (`2 * size` bytes)
   - `win32u/dibdrv/objects.c:1860` — `calc_and_xor_masks(4, color, and, xor)` computes `and = 0`, `xor = ~color`
   - The AND mask buffer is filled with zeros

7. **Pattern application on 32bpp surface — buffer overread**
   - `win32u/dibdrv/primitives.c:868-887` — `pattern_rects_32` uses `memcpy(start + x, start_xor + brush_x, len * 4)`
   - For a 1bpp pattern brush, `start_xor` points to one DWORD per row. `brush_x` is a pixel offset, but it is used as a **DWORD array index**
   - When `offset.y > 0`, `start_xor` advances by 1 DWORD per row. The `memcpy` copies up to 8 DWORDs (for an 8-pixel-wide brush). This **overreads** into subsequent rows' XOR masks and then into the **AND mask buffer**
   - The AND mask contains zeros, so the overread copies `0x00000000` (black) into the destination pixels

---

### Root Cause Analysis

**Primary cause:** `user32/scroll.c` uses an invalid GDI ROP code `0x0f0000` in four places (`:148`, `:152`, `:159`, `:162`) for the selected scrollbar track state. `0x0f0000` is not a documented `PatBlt` ROP. Wine's DIB engine translates it to `R2_NOTCOPYPEN`, which triggers mask allocation for the 55AA checkerboard pattern brush. The DIB engine's `pattern_rects_32` function then overreads into the zero-initialized AND mask buffer, producing black pixels at the right edge of the clicked track area.

**Why this is visible on macOS / MacRunner but less so on Windows:**
- On Windows with themes enabled, `OpenThemeDataForDpi` returns a valid theme handle. The uxtheme path (`UXTHEME_ScrollBarDraw`) uses `DrawThemeBackground` and never executes the classic `SCROLL_DrawInterior` code.
- On Wine/macOS, `bThemeActive` defaults to `FALSE` because no msstyles theme is loaded. `OpenThemeDataForDpi` returns `NULL`, forcing fallback to the classic `USER_ScrollBarDraw` path which contains the `0x0f0000` bug.
- Additionally, `COLOR_3DHILIGHT == COLOR_WINDOW` on Wine defaults, so the 55AA pattern brush is selected. This brush is 1bpp, which exposes the `pattern_rects_32` overread bug.

**Evidence:**
- ReactOS bug CORE-10279: "Scrollbar shows black background" — identical symptom, clicking above/below thumb leaves black area
- ReactOS bug CORE-3923: "Scrollbar graphic glitches" — clicking scrollbar area leaves it black when button is depressed
- Microsoft ternary ROP documentation: boolean function `0F` maps to `0x000F0001` (`Pn`), not `0x0f0000`. `0x0f0000` has a truth table byte of `0x00`, which would yield all zeros (black) if interpreted literally.
- `user32/scroll.c:148` — exact line of invalid ROP

---

### Suggested Fix Scope

1. **Immediate fix:** Replace `0x0f0000` with a valid ROP in `user32/scroll.c`. The intended behavior for selected track areas is likely inversion or dithering. The correct documented ROP for inverting with the pattern is `PATINVERT` (`0x5A0049`). Alternatively, simply use `PATCOPY` (same as the non-selected state) if visual feedback is handled elsewhere.

2. **Defensive fix:** Fix the `pattern_rects_32` / `pattern_rects_24` / etc. buffer overread for 1bpp pattern brushes in `win32u/dibdrv/primitives.c`. The inner loop should not use `brush_x` as a direct DWORD index when the source pattern is 1bpp. This is a latent bug that could affect any 1bpp pattern brush with non-COPY ROPs.

3. **Optional:** Ensure `bThemeActive` can be enabled on macOS or provide a fallback msstyles theme so that scrollbars use the themed path, bypassing the classic code entirely.

---

### Confidence Level

**High**

- The invalid ROP `0x0f0000` is an objective code bug with no documented Windows equivalent.
- The DIB engine `pattern_rects_32` overread for 1bpp brushes is a concrete memory safety issue that produces exactly black pixels from the zero-filled AND mask.
- The fallback from themed to classic scrollbars on theme-less Wine installs is a well-established code path.
- ReactOS independently documented the same "black scrollbar background" symptom (CORE-10279, CORE-3923).
