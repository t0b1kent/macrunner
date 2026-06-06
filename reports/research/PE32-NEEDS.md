# PE32 Needs

## 2026-06-07 08:20 - Lane C loader/server handoff blocker

Owner: Lane C (`ntdll` Unix loader/server). PE32 lane must not edit this directly.

Evidence:
- `reports/phase-h/pe32-handoff-proof-skipboot-20260607-080556/stderr.log`
  - env: `MACRUNNER_HB_X64_LOADER=0`, `MACRUNNER_HB_TRACE_PE32_LOADER=1`
  - `notepad++.exe` PE32 maps first, but `build_main_module` is `C:\windows\system32\start.exe` with `machine=aa64`.
  - counts: `wow64=0`, `wow64cpu=0`, `xtajit=0`, `BTCpu=0`.
- `reports/phase-h/pe32-handoff-proof-loader1-20260607-080917/stderr.log`
  - same behavior with `MACRUNNER_HB_X64_LOADER=1`; no `PE32 builtin using i386 lane`, no WOW64 CPU provider.
- `reports/phase-h/pe32-handoff-proof-winearch-wow64-20260607-081214/stderr.log`
  - same behavior with `WINEARCH=wow64`.

Root path:
- `engine/wine/dlls/ntdll/unix/env.c:1960-1988`
  - `load_main_exe()` opens/maps the PE32 image.
  - a non-success status from mapping triggers `load_start_exe()`.
  - `load_start_exe()` replaces the original PE32 main image with native `start.exe`.
- `engine/wine/dlls/ntdll/unix/loader.c:2368-2379`
  - `load_start_exe()` uses `get_machine_wow64_dir(current_machine)`, so on ARM64 it loads `C:\windows\system32\start.exe`.
- likely server-side reject:
  - `engine/wine/server/mapping.c:1697-1715` keeps `current->process->machine` native/ARM64 and raises `STATUS_IMAGE_MACHINE_TYPE_MISMATCH` when the PE32 main image (`req->machine == IMAGE_FILE_MACHINE_I386`) maps into the ARM64 loader process.

Requested fix:
- For ARM64 MacRunner PE32/WOW64 launch, allow the first PE32 main image to become the process guest machine (`IMAGE_FILE_MACHINE_I386`) instead of returning mismatch and falling back to `start.exe`.
- Keep the existing native-host allowance for later ARM64 support images: once process machine is I386, native ARM64 mappings should remain allowed by the existing "32-bit process may map native machine" rule at `mapping.c:1714`.
- After fix, validation gate is:
  - `build_main_module ... notepad++.exe ... machine=014c`
  - `wow64.dll` / `wow64cpu.dll` / `xtajit.dll` load attempt
  - `BTCpuProcessInit` or `BTCpuThreadInit` marker appears.
