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

## 2026-06-08 — i386 exception-dispatch recursion fix needed (Lane A domain)

Owner: Lane A (`macrunner_hb.c`, `signal_arm64.c`). DO NOT edit from PE32 lane.

Evidence from PE32 run (exit_code_x86.exe proxy, Jun 8):
- `macrunner-xtajit: btcpusimulate publish-exception-context status=00000000 exception=c0000005 eip=7bdc7890`
- eip=7bdc7890 = ntdll32 LdrInitializeThunk (offset 0x57890)
- The i386 exception at 7bdc7890 triggers SEH dispatch into the handler at 7bdc6da8
- Handler at 7bdc6da8 itself faults → recursion (confirmed infinite loop in previous runs)

Root cause (in Lane A's domain):
- When an i386 exception can't be handled (pass_guest_exception → Wow64PassExceptionToGuest → ntdll32 SEH dispatch at 7bdc6da8), the dispatch handler at 7bdc6da8 faults again, re-entering the same path
- `signal_arm64.c` / `macrunner_hb.c` exception-dispatch-to-i386 redirect must bound recursion (e.g., max depth guard or detect recursive dispatch and terminate cleanly)
- Previously Lane A fixed the "host-boundary c0000026" via CFI/unwind thunk metadata; the i386 recursion was the next blocker that surfaced

Requested fix:
- In `signal_arm64.c` (or wherever i386 exception dispatch is redirected), add a per-thread recursion counter
- If depth > 1, abort the exception chain with a clean STATUS_NONCONTINUABLE_EXCEPTION rather than recursing infinitely
- After fix, validation gate: run Diablo i386 and confirm no infinite recursion; should see a single c0000005 at eip=7bdc7890, then either progress or a new distinct error
03:51 · PE32→LaneA need: ntdll macrunner_hb_pc_is_x64_guest_code_no_lock faults at pc=0x104e23b0c fault=0x300000018 while xtajit active syscall svc=0x36 x18=0x3001f0000, after wow64 NtQuerySystemInformation basic put-before. Runs: reports/pe32/run-20260610-034821-hb-jit-buffer-guard and run-20260610-034128-xtajit-x18-restore. PE32 probing field write; do not edit ntdll from PE32.

## 2026-06-10 07:15 — first win32u syscall faults in native init_user (Lane D + Lane A x18)

Frontier (run-20260610-041803, hl.exe i386): i386 reaches its FIRST win32u call
**svc=0x147a = NtUserInitializeClientPfnArrays** → native win32u **`init_user()`** (dlls/win32u/class.c:259)
faults c0000005 dereferencing an **unrebased 32-bit guest pointer ~0x201000+0x94** (should be host
0x3_00201094; guest32_base=0x3_00000000). **x18(host TEB)=0** in the fault context; native pc≈0x10774fe20.
macrunner_hb x64-callback router correctly REJECTS (i386 fault) → unhandled c0000005 → exit=5.

PE32 scope verified CLEAN (not the cause): wow64/wow64win pointer thunks rebase correctly
(guest32_host_ptr in both private headers); win32u table wired (sdwhwin32); ctx->guest32_base set
per-thread (hb_wow64cpu.c:547); JIT direct-mem/direct-stack arch-gated OFF for HB_ARCH_X86
(codegen.c:371). So the raw-pointer deref is NOT a JIT-rebasing or thunk bug.

Owner A — Lane A (`signal_arm64.c` / native `__wine_syscall_dispatcher` / macrunner_hb): **x18 (host TEB)
is lost INSIDE the native win32u call** invoked from the xtajit bridge syscall (asm thunk sets x18=teb
at cpu.c:68 before blr, but it's 0 by the time init_user derefs). Either the native syscall dispatcher
does not restore x18 for a bridge-context (xtajit synthetic syscall_frame at teb+0x378, frame->x18 at
+0x90) syscall, or a KeUserModeCallback path drops it. NtCurrentTeb() in native win32u then mis-resolves
→ reads the guest PEB/ProcessParameters (a 32-bit ptr ~0x201000) → fault.

Owner B — Lane A (bootstrap CFI/unwind): cold-prefix runs hit a **c0000026 SEH unwind-metadata-missing
recursion** in kernelbase.dll (image base 0x87FFF7A0000, `virtual_unwind exception data not found`) during
wineboot/services, blocking cold-prefix repro of the win32u fault. The "c0000026 PASSED" milestone may not
cover the wineboot/services thunk path, or regressed. Workaround for PE32 iteration: warm-prefix template
(MACRUNNER_MR_RUN_PREFIX_TEMPLATE) to skip wineboot.

Validation gate after fix: hl.exe/Diablo.exe i386 — x18 stays = host teb through NtUserInitializeClientPfnArrays;
init_user completes; no c0000005 at ~0x201xxx; next distinct gate or a window.

## 2026-06-10 21:35 — LANE A ANSWER to the 06-10 07:15 item: x18 EXONERATED, real root = unrebased syscall-arg table
Evidence re-read (run-20260610-041803-xtajit-teb32-cache, run.log:9095-9124):
- xtajit traces show x18=0x3001F0000 (correct host TEB) at every stage through `before-process`;
  fault context has **x0=0x3001f0000 = correct host TEB** alive in the register file.
- x18=0 at fault pc=0x10774fe20 is NORMAL: pc is unix .so code; unix-side NtCurrentTeb() is
  `pthread_getspecific(teb_key)` (thread.c:1767), x18-independent. Nothing in unix code reads x18.
- Real fault: `fault=0x201094` = `client_procsA` itself. win32u/class.c NtUserInitializeClientPfnArrays
  does `winproc_array[i].procA = client_procsA[i][0]` — deref of the GUEST32-unrebased table pointer
  (host should be 0x300201094, guest32_base=0x3_00000000) at i=0.
- Dispatcher paths audited on Lane A side and CLEAN: `__wine_syscall_dispatcher` saves TEB (not raw x18)
  into frame->x18 (+0x90) via WINE_LOAD_TEB_IN_X17; return path = `ldp x18,[sp,#0x90]` + APPLE
  WINE_RESTORE_X18_FROM_TEB; call_user_mode_callback sets `mov x18, x4` (teb). No x18 hole found.
REQUESTED FIX (PE32 scope): rebase the 3 ntuser_client_func_ptr table pointers (client_procsA/W/workers)
in the wow64win thunk for NtUserInitializeClientPfnArrays (or in the xtajit pending-cross arg marshaling
that bypasses it) — zero-extension is identity on Windows but NOT under MacRunner guest32 rebase.
Validation: hl.exe i386 — no c0000005 at 0x201xxx in init_user; svc=147a returns 0; next gate.
Note: Owner B (cold-prefix wineboot c0000026 unwind storm) IS Lane A — bulk-CFI family re-applied
2026-06-10 21:20 (callback_trampoline + pe_call12 + x18 thunks CFI restored); validation run in flight.
