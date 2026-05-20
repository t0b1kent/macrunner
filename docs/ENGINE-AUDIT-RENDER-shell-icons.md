## Engine Rendering Audit — Shell32 Folder/Drive Icons (Save As Dialog Tree View)

**Status**: Read-only audit — findings to be forwarded to Codex for implementation  
**Date**: 2026-05-21  
**Auditor**: Kimi  
**Confidence**: Medium-High

---

### 1. Symptom Description

Open/Save dialog tree-view items for shell namespaces (My Computer, Documents, Favorites) display corrupted folder/drive icons. User reports them as **white/blank**; internal visual-regression artifacts characterize the same failure as **black silhouettes** against the tree-view background. Both descriptions point to the same root cause: the icon's color bitmap contains only zero-value pixels, so the rendered shape is either solid black (if the mask is opaque) or fully transparent/invisible (if the mask is transparent), letting the white tree-view background show through.

---

### 2. Code Path Traced (file:line)

| Step | File:Line | Action |
|---|---|---|
| Treeview initialization | `shell32/brsfolder.c:182` | `InitializeTreeView` calls `Shell_GetImageLists` |
| Insert item | `shell32/brsfolder.c:353` | `InsertTreeViewItem` calls `GetNormalAndSelectedIcons` |
| Icon index lookup | `shell32/brsfolder.c:285` | `GetNormalAndSelectedIcons` calls `SHGetFileInfoW` with `SHGFI_PIDL \| SHGFI_SYSICONINDEX \| SHGFI_SMALLICON` |
| File info | `shell32/shell32_main.c:154` | `SHGetFileInfoW` -> `SHGetImageList` -> `PidlToSicIndex` -> `SIC_GetIconIndex` |
| Icon cache load | `shell32/iconcache.c:504` | `SIC_LoadIcon` extracts icon via `PrivateExtractIconsW` (`shell32/iconcache.c:515`) |
| ICO extraction | `user32/exticon.c:249` | `ICO_ExtractIconExW` maps PE resource, calls `CreateIconFromResourceEx` |
| Icon creation | `user32/cursoricon.c:1250` | `CreateIconFromResourceEx` -> `create_icon_from_bmi` (`user32/cursoricon.c:982`) |
| Frame creation | `user32/cursoricon.c:768` | `create_icon_frame` allocates `frame->color` via `create_color_bitmap` (display-compatible 32bpp DDB, zero-initialized by `calloc` in `NtGdiCreateBitmap`) |
| **Critical copy** | **`user32/cursoricon.c:879`** | **`StretchDIBits` copies 4bpp/8bpp ICO color bits into the 32bpp DDB. Return value is ignored.** |
| Mask copy | `user32/cursoricon.c:929` | `StretchDIBits` copies mask bits into 1bpp `frame->mask` |
| Imagelist insertion | `comctl32/imagelist.c:2772` | `ImageList_ReplaceIcon` calls `CopyImage(..., LR_COPYFROMRESOURCE)` |
| CopyImage resource | `user32/cursoricon.c:2233` | `CopyImage` attempts `LR_COPYFROMRESOURCE`, but the icon was created with `module=NULL` by `CreateIconFromResourceEx`, so `GetIconInfoExW` returns empty `szModName` and the resource-reload branch is **skipped** |
| Fallback stretch | `user32/cursoricon.c:2454` | Fallback path stretches the original (already black) `hbmColor` via `stretch_bitmap` (`user32/cursoricon.c:2192`) |
| Add to imagelist | `comctl32/imagelist.c:2819` | `ImageList_ReplaceIcon` enters `add_with_alpha`; `GetDIBits` reads the black pixels and inserts them |
| Tree draw | `comctl32/imagelist.c:1572` | `ImageList_DrawIndirect` draws the black/zeroed entry, producing a black silhouette or blank transparent region |

---

### 3. Root Cause Analysis with Evidence

**Exact failure point:** `user32/cursoricon.c:879` — `StretchDIBits` inside `create_icon_frame`.

**Why it breaks:**
- `folder.ico`, `drive.ico`, `mycomputer.ico`, and `desktop.ico` are **low-color resources** (4bpp/8bpp indexed DIBs). They contain no 32bpp alpha channel.
- `create_icon_frame` creates a 32bpp display-compatible DDB for `frame->color`. `NtGdiCreateBitmap` zero-initializes the buffer (`calloc`).
- `StretchDIBits` is called to convert the 4bpp/8bpp indexed DIB into that 32bpp DDB. On macOS ARM64, this call **silently fails or produces all-black/zero pixels**, leaving the bitmap uninitialized.
- The return value of `StretchDIBits` is **not checked** in `create_icon_frame`, so the corruption is undetected.
- Because the icon was created via `CreateIconFromResourceEx` with `module=NULL`, `CopyImage(..., LR_COPYFROMRESOURCE)` cannot reload the icon from the original resource; it can only stretch the already-corrupted bitmap.
- `ImageList_ReplaceIcon` reads the black pixels via `GetDIBits` and stores them in the imagelist.

**Evidence:**
- `file` on `shell32/resources/folder.ico` reports: `MS Windows icon resource - 10 icons, 16x16, 16 colors, 4 bits/pixel, 16x16, 8 bits/pixel` — confirming low-color, no 32bpp alpha.
- Visual-regression lab patch (`0031-visual-regression-lab-v1.patch`) records: *"Open/Save dialog folder icons render as black silhouettes"* and *"GetIconInfo returns a black hbmColor bitmap for a valid color icon."*
- `user32/cursoricon.c:879` ignores `StretchDIBits` return.
- `win32u/bitmap.c:201` initializes DDBs with `calloc(1, size)`, so any copy failure leaves black (all-zero) pixels.

**Why macOS specifically?**
The DIB driver path for `nulldrv_StretchDIBits` -> `convert_bits` -> `convert_to_8888` should handle 4bpp/8bpp conversion. However, the macOS display DC (`winemac.drv`) does not implement `pStretchDIBits` directly; execution falls through the null-driver fallback chain. On macOS ARM64, the PE-to-Unix syscall transition for `NtGdiStretchDIBitsInternal` may mis-handle the `BITMAPINFO` color-table pointer or `bits` pointer for indexed formats, or the DIB driver's `pPutImage` may return `ERROR_BAD_FORMAT` and the `convert_bits` fallback may fail under a specific MacRunner GDI configuration. Runtime `macrunner-gdi-dibits` trace logs would be required to distinguish between `ERROR_BAD_FORMAT`, `ERROR_TRANSFORM_NOT_SUPPORTED`, or silent `pPutImage` failure.

---

### 4. Suggested Fix Scope

1. **Immediate workaround (highest confidence):** Replace the low-color shell32 ICO resources (`folder.ico`, `drive.ico`, `mycomputer.ico`, `desktop.ico`) with **32bpp ARGB versions** where opaque pixels have alpha `0xFF`. This bypasses the problematic `StretchDIBits` indexed-color path entirely and forces the `add_with_alpha` / alpha-blend path, which is known to work for 32bpp sources.

2. **Defensive fix:** In `user32/cursoricon.c:create_icon_frame`, check the return value of `StretchDIBits` (line 879) and the mask `StretchDIBits` (line 929). If either returns 0, abort icon creation or at least log a clear error.

3. **Root fix:** Instrument `macrunner-gdi-dibits` traces around `nulldrv_StretchDIBits` (`win32u/dib.c:604`) for `src_bpp=4/8` and `dst_bpp=32` to determine whether `convert_bits` or `pPutImage` is the failing step. If `convert_to_8888` (`win32u/dibdrv/primitives.c:2222`) is producing alpha=0 RGB values, ensure the alpha byte is set to `0xFF` for opaque indexed-color conversions.

---

### 5. Confidence Level

**Medium-High**

- The full pipeline from `SHGetFileInfoW` down to `StretchDIBits` in `create_icon_frame` is traced with exact file:line references.
- The visual-regression lab provides independent confirmation that `hbmColor` is black after icon creation.
- The exact mechanism inside `StretchDIBits` that fails on macOS (syscall marshalling vs. DIB driver `convert_bits`) would require a runtime `macrunner-gdi-dibits` trace to prove with absolute certainty.
