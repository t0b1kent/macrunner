# LANE AUDIO+INPUT — NEEDS & COORDINATION

This file logs changes required in core/ntdll/loader files that are owned by other lanes (e.g., Lane A or Lane C). Local changes to these files must be reverted before final commits, and the coordinator/responsible lanes will integrate them.

## Active Needs

### 1. ntdll/loader.c — Add ucrtbase.dll to needs_native_entry allowlist
- **File:** `engine/wine/dlls/ntdll/loader.c` (around line 5074)
- **Change:**
  ```diff
  -                BOOL needs_native_entry = !wcsicmp( wm->ldr.BaseDllName.Buffer, L"win32u.dll" ) ||
  -                                           !wcsicmp( wm->ldr.BaseDllName.Buffer, L"kernelbase.dll" );
  +                BOOL needs_native_entry = !wcsicmp( wm->ldr.BaseDllName.Buffer, L"win32u.dll" ) ||
  +                                           !wcsicmp( wm->ldr.BaseDllName.Buffer, L"kernelbase.dll" ) ||
  +                                           !wcsicmp( wm->ldr.BaseDllName.Buffer, L"ucrtbase.dll" );
  ```
- **Reason:** `ucrtbase.dll` (C Runtime) requires native initialization under ARM64EC to set up essential locks, I/O structures, and thread-local state. Skipping its entry point entirely causes downstream crashes when guest x64 code calls C runtime functions.
