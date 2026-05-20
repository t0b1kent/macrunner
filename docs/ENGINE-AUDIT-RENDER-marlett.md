## Engine Rendering Audit: Empty Window Control Buttons (Minimize / Maximize / Close)

**Status**: Read-only audit — findings to be forwarded to Codex for implementation  
**Date**: 2026-05-21  
**Auditor**: Kimi  
**Confidence**: Medium-High

---

### 1. Symptom Description

The non-client area caption buttons (minimize, maximize/restore, close) on Notepad++ windows render as **empty squares** — the 3D button frame is drawn, but the glyph inside (the min/max/close symbol) is missing. This is a purely visual defect; the buttons remain clickable.

---

### 2. Code Path Traced (file:line)

| Step | File:Line | Action |
|---|---|---|
| NC paint entry | `win32u/defwnd.c:1568` | `draw_nc_caption` calls `draw_close_button`, `draw_max_button`, `draw_min_button` |
| Button draw | `win32u/defwnd.c:1483` | `draw_close_button` computes rect and calls `draw_frame_caption` |
| Font creation | `win32u/defwnd.c:1363-1368` | `draw_frame_caption` builds a `LOGFONTW` with `lfCharSet = SYMBOL_CHARSET`, face name `"Marlett"`, and calls `NtGdiHfontCreate` |
| Text extent | `win32u/defwnd.c:1373` | `NtGdiGetTextExtentExW` queries glyph size |
| Text output | `win32u/defwnd.c:1386` | `NtGdiExtTextOutW` draws the glyph (e.g. `0x30` for minimize, `0x31` for maximize, `0x32` for restore, `0x72` for close) |
| TextOut dispatcher | `win32u/font.c:5962` | `NtGdiExtTextOutW` resolves the DC physdev and calls `pExtTextOut` |
| Null driver text | `win32u/font.c:5779` | `nulldrv_ExtTextOut` handles the actual bitmap rendering |
| Glyph bitmap fetch | `win32u/font.c:5642` | `get_glyph_bitmap` calls `NtGdiGetGlyphOutline` to get the monochrome glyph bitmap |
| Glyph outline syscall | `win32u/font.c:6399` | `NtGdiGetGlyphOutline` forwards to `font_GetGlyphOutline` |
| Font driver glyph | `win32u/font.c:4215` | `font_GetGlyphOutline` calls `get_glyph_outline` |
| Glyph index lookup | `win32u/font.c:3864` | `get_glyph_index` calls `freetype_get_glyph_index` |
| Symbol glyph map | `win32u/freetype.c:2192` | `get_glyph_index_symbol` adds `0xf000` to low glyphs and queries FreeType via `FT_Get_Char_Index` |
| FreeType load | `win32u/freetype.c:3123` | `freetype_get_glyph_outline` calls `FT_Load_Glyph`; if this fails it returns `GDI_ERROR` |
| Bitmap render | `win32u/font.c:5727` | `draw_glyph` converts the monochrome bitmap to `NtGdiPolyPolyDraw` line segments |
| **Font init** | `win32u/font.c:6899` | `font_init` calls `load_file_system_fonts` which scans `C:\Windows\Fonts` and the Wine data-dir `share/wine/fonts` |
| **macOS font load** | `win32u/freetype.c:1568` | `freetype_load_fonts` on `__APPLE__` calls `load_mac_fonts` (CoreText enumeration of all macOS system fonts) |
| **Data dir resolve** | `win32u/font.c:540` | `get_fonts_data_dir_path` uses `ntdll_get_build_dir()` first, falls back to `ntdll_get_data_dir()` |

---

### 3. Root Cause Analysis with Evidence

**Exact failure point:** The glyph is never rendered inside `nulldrv_ExtTextOut` because `get_glyph_bitmap` returns an error (`ERROR_NOT_FOUND`) or produces an empty bitmap, causing the drawing loop to `continue` without calling `draw_glyph`.

**Why Marlett specifically:**
1. Windows draws caption button symbols using the **Marlett** symbol font (glyphs `0x30`, `0x31`, `0x32`, `0x72`).
2. General text rendering in Notepad++ works fine (menus, editor, dialogs), proving the overall `ExtTextOutW` → `nulldrv_ExtTextOut` → `get_glyph_bitmap` → `draw_glyph` pipeline is functional.
3. The defect is **isolated to Marlett**, indicating either:
   - The Marlett font file is **not present in the active font list** at runtime, so `find_matching_face` selects a fallback font that lacks these symbols.
   - Marlett **is** present, but `get_glyph_index_symbol` → `FT_Get_Char_Index` returns 0 for the requested glyphs, causing `get_glyph_outline` to load glyph 0 (`.notdef`). If `.notdef` is empty or also fails to load, the button interior is blank.

**Evidence:**
- `draw_frame_caption` at `defwnd.c:1333` is the sole code path for standard NC caption buttons. It unconditionally uses Marlett.
- `get_glyph_bitmap` at `font.c:5642` implements a fallback chain: `{requested_glyph, 0, 0x20}`. If all three fail, it returns `ERROR_NOT_FOUND`. The `nulldrv_ExtTextOut` loop (`font.c:5887-5892`) does `if (err) continue;`, producing **no output** for that character.
- The builtin `marlett.ttf` exists at `engine/wine/dist-pure-arm64/share/wine/fonts/marlett.ttf` (verified on disk), but its presence in the build tree does not guarantee it is successfully enumerated by `load_directory_fonts` at runtime.
- On macOS, `freetype_load_fonts` (`freetype.c:1568`) only calls `load_mac_fonts`. Builtin fonts rely on `load_file_system_fonts` (`font.c:6710`) which derives its path from `ntdll_get_build_dir()` / `ntdll_get_data_dir()`. If the MacRunner launch environment causes `build_dir` or `data_dir` to resolve incorrectly (e.g., pointing to the Wine source tree rather than the `dist-pure-arm64` install tree), `load_directory_fonts` silently fails and Marlett is never added to the font list.
- Codex independently identified the same hypothesis: *"Marlett font не загружен"* (`reports/agent-prompts/CODEX-NEXT-NUDGE.txt:6`).
- The visual-regression lab patch (`0031-visual-regression-lab-v1.patch`) catalogs this under toolbar/placeholder square artifacts but does not yet instrument the Marlett-specific path.

**Why macOS specifically:**
- On Linux, Wine uses `fontconfig` (`load_fontconfig_fonts`) which indexes the system and Wine builtin fonts. Marlett is reliably discoverable.
- On macOS, `load_mac_fonts` enumerates native CoreText fonts. There is no fontconfig fallback. Builtin Wine fonts are loaded only via `load_file_system_fonts`. If the data-directory path resolution is off by even one level (e.g., `build_dir` vs `data_dir` mismatch in a non-standard install layout), Marlett — a font with **no macOS equivalent** — simply disappears from the font list. Other text looks fine because macOS fonts (SF Pro, Helvetica, etc.) substitute for common UI faces like Tahoma or MS Sans Serif via `find_any_face` fallback.

---

### 4. Suggested Fix Scope

**Option A — Diagnostic (highest priority):**
Add a one-line trace in `win32u/font.c:540` or `freetype.c:1103` to print the resolved data-dir path and whether `marlett.ttf` is successfully opened by `unix_face_create`. Also trace `get_glyph_index` at `font.c:3864` for face name `"Marlett"` to confirm whether glyph lookup returns 0. This will distinguish between:
1. Font not loaded (file path wrong / `load_directory_fonts` skipped it).
2. Font loaded but glyph index lookup returns 0 (cmap / encoding issue).
3. Glyph index valid but `FT_Load_Glyph` fails (FreeType-specific).

**Option B — Short-term workaround:**
If the diagnosis confirms the font file is missing from the runtime font list, ensure the MacRunner launcher sets `WINEDATADIR` (or patches `ntdll_get_data_dir()`) to point to `dist-pure-arm64/share/wine` so `load_file_system_fonts` finds `marlett.ttf`. Alternatively, copy `marlett.ttf` into the prefix `C:\Windows\Fonts` during prefix creation.

**Option C — Robust fallback:**
If Marlett cannot be guaranteed at runtime, modify `draw_frame_caption` (`defwnd.c:1333`) to detect when `GetTextExtentExPointW` returns zero size or `ExtTextOutW` produces no pixels, and fall back to drawing simple geometric shapes (line/cross/triangle) using `PatBlt` / `LineTo`. This matches the fallback some Windows themes use when Marlett is corrupted.

**Option D — Data fix:**
If `FT_Get_Char_Index` for Marlett's Symbol cmap returns 0 on macOS due to a FreeType/char-map quirk, instrument `pick_charmap` (`freetype.c:1965`) and `select_charmap` (`freetype.c:1914`) to ensure `FT_ENCODING_MS_SYMBOL` is actually selected for the Marlett face. The fallback to `FT_ENCODING_UNICODE` for a Symbol-only font will always produce zero glyph indices.

---

### 5. Confidence Level

**Medium-High**

- The full pipeline from `draw_frame_caption` through `ExtTextOutW` to `get_glyph_bitmap` and `draw_glyph` is traced with exact file:line references.
- The macOS-specific font loading path (`load_mac_fonts` + `load_file_system_fonts` with `build_dir`/`data_dir` resolution) is a concrete, testable hypothesis that explains why Marlett vanishes while other text renders correctly.
- Independent confirmation from Codex briefing (`CODEX-NEXT-NUDGE.txt`) supports the same root cause.
- Absolute proof requires a runtime trace confirming whether `find_matching_face` returns Marlett and whether `get_glyph_index` yields a non-zero glyph index. The audit recommends exactly this diagnostic as the first implementation step.
