# ACTIVE INVESTIGATION — живое состояние

Last update: 2026-05-28 18:59 VLAT, Codex.

Читайте этот файл первым после compaction. Не перепроверять DISPROVED без нового
контр-факта. Не возвращать Wine-render патчи.

## Current Task

Update 2026-05-28 09:46 VLAT: evidence-first PC=0/null-PC classification
completed. The old status-fix follow-up was not a real `PC=0`: fresh child lldb
capture in `reports/phase-h/npp-x86-nullpc-vmmap-20260528-093235` shows thread
#2 at native `pc=0x7ffd06b7bb0`, `fault=0x7ffd06b7bb0`, `x4=x17=pc`,
`lr=0x7ffd0ab90e4`; vmmap proves the target is an r-- AMD64 guest view
(`win32u.dll`, module base `0x7ffd06a0000`, rva `0x17bb0`), while caller is in
ARM64 `ntdll.dll` generated code. This matches the x64 golden return-target
class: native ARM64 code branched to x64 guest code and must be routed through
HyperBridge.

Fix applied:
- `engine/wine/dlls/ntdll/unix/signal_arm64.c`: allow the existing x64 guest
  fault route in PE32/WOW64 only when x64 loader is enabled, current native
  machine is ARM64, main image is I386, and raw/fault/x4 passes the existing
  registered AMD64 guest-code test. This mirrors x64 behavior without trusting
  arbitrary stale `x4`.
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c`: dispatch x64 callback execution
  with `module_from_pc(target)` as the image base, so PE32 callbacks into 64-bit
  side DLLs mark the correct AMD64 module exec sections instead of the I386 main
  image.

Validation: rebuilt/installed/codesigned `aarch64-unix/ntdll.so` from
`artifacts/phase-h/build-ntdll-wow64-x64-route-20260528-093708.log` (`rc=0`;
only pre-existing warnings). `scripts/verify-build-freshness.sh` is PASS in
`reports/phase-h/freshness-wow64-x64-route-20260528-094552.log`. Direct scoped
Wine run `reports/phase-h/npp-x86-direct-after-wow64-x64-route-20260528-094245`
confirms the former fault now routes: `target=0x7ffd06b7bb0`, module
`win32u.dll`, callback returns `ret=0x1 blocks=12 steps=0x4c`; no
`UNSUPPORTED`, no `MEMORY_FAULT`, no reset-context.

Current next blocker: PE32 no longer sits in the old branch-to-r-- SIGBUS loop,
but still does not reach a Notepad++ window. Direct runs now either return
`rc=53` or spend time in repeated WOW64 `NtMapViewOfSection` activity without an
opcode/memory-fault signature. Next step is to classify that mapping loop/exit
with scoped-prefix runs only. Do not use `run-windows-app.sh` for long runs until
its global cleanup is fixed; it calls `cleanup-wine-runtime.py`, which can kill
parallel Wine work. Use direct `CompatibilityPlan.command` with
`WINEPREFIX=bottles/generic-x86` and stop via that prefix's `WINESERVER -k`.

Update 2026-05-28 13:03 VLAT: resumed in the canonical internal worktree
(`/Users/timurtoby/Documents/MacRunner/Main/MacRunner`); ignore the external
Kimi graphics worktree at `/Volumes/MacOS 1/...` except read-only if explicitly
needed. Latest post-route PE32 reports:

- `reports/phase-h/npp-x86-process-debug-long-20260528-124348`: child spins
  with no `UNSUPPORTED`, no `MEMORY_FAULT`, no process-exit trace; stderr shows
  repeated WOW64 `NtMapViewOfSection` returns `STATUS_IMAGE_NOT_AT_BASE`
  (`40000003`) while mapping PE modules.
- `reports/phase-h/npp-x86-npp-window-poll-20260528-125241`: no Notepad++ CG
  window in 180s. lldb attached to the child at `_sigtramp` with Wine exception
  code in `x24=0xc0000005`; this is not a fresh `PC=0` branch-to-r-- signature.
- `reports/phase-h/npp-x86-normal-window-poll-20260528-125200` is a false
  positive: it matched an unrelated external `notepad.exe` window, not our
  `notepad++.exe`. Future window probes must match `notepad++` and verify the
  owner PID belongs to this worktree/prefix.

Current next step: scoped direct run from the saved compatibility command,
capture a short hot-spin sample/lldb snapshot plus stderr markers, then classify
whether the loop is normal loader rebasing, repeated handled AV, or a WOW64
mapping/state regression. Cleanup remains prefix-scoped only.

Update 2026-05-28 13:16 VLAT: current no-window blocker classified as prefix
deployment, not CPU opcode/status. `reports/phase-h/npp-x86-pgid-long-sample-20260528-131053`
shows real PE32 child `notepad++.exe` running ~97% CPU while loader maps
dependencies, then exiting with `0xc0000135`; `macrunner-start-exit` propagates
that to `start.exe` (`rc=53`). Prefix audit: `bottles/generic-x86/.../syswow64`
had only 11 i386 DLLs and was missing 14 Notepad++ imports (`shlwapi`,
`dbghelp`, `version`, `crypt32`, `wintrust`, `sensapi`, `wininet`, `uxtheme`,
`dwmapi`, `advapi32`, `ole32`, `oleaut32`, `imm32`, `ucrtbase`), while x64
golden/current system32 has a full DLL set. Root fix started in
`scripts/sync-prefix-from-dist.sh`: sync all arch DLLs into system32 and all
i386 DLLs into syswow64, not just the old core subset. Next: run that sync on
`bottles/generic-x86` with `--system32-arch=x86_64-windows`, verify syswow64
imports, then rerun PE32 Notepad++.

Update 2026-05-28 13:29 VLAT: full prefix sync was applied:
`reports/phase-h/sync-generic-x86-full-dlls-20260528-1316.log`; syswow64 now
has 604 i386 DLLs and all Notepad++ imports are present. The next blocker is a
real signal/VM classifier fault, not missing DLLs: post-sync run
`reports/phase-h/npp-x86-postsync-hotspin-lldb-20260528-132501` keeps PE32
`notepad++.exe` alive at ~99% CPU with no window. No `UNSUPPORTED` or
`MEMORY_FAULT` marker appears. Fault trace
`reports/phase-h/npp-x86-postsync-faulttrace-20260528-132350` shows the handler
recursing while classifying an invalid address: `virtual_handle_fault` calls
`get_host_page_vprot()` and faults reading the vprot bucket near
`fault=0x30617009c`; enabling `MACRUNNER_HB_TRACE_FAULTS=1` amplifies this into
recursive `macrunner_hb_native_fault_dump_qwords()` faults. Root fix applied in
`engine/wine/dlls/ntdll/unix/virtual.c`: `get_host_page_vprot()` now unions
host-page protections via bounded `get_page_vprot()` per guest page, avoiding a
cross-bucket read during signal handling. Next: rebuild/install/codesign
`aarch64-unix/ntdll.so`, run freshness, rerun PE32 Notepad++.

Build/validation 2026-05-28 13:30 VLAT: rebuilt `ntdll.so` with the vprot
boundary fix in
`artifacts/phase-h/build-ntdll-vprot-boundary-20260528-1329.log` (`rc=0`, only
pre-existing warnings), installed/codesigned
`engine/wine/dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so`, and freshness is
PASS in `reports/phase-h/freshness-ntdll-vprot-boundary-20260528-1329.log`.

Update 2026-05-28 13:38 VLAT: post-vprot rerun
`reports/phase-h/npp-x86-after-vprot-fix-20260528-133052` no longer shows the
recursive vprot trace fault, but still produces no Notepad++ CG window after
240s. The PE32 child stays hot (~99% CPU), stderr has no `UNSUPPORTED` or
`MEMORY_FAULT`, and lldb catches thread #2 in `_sigtramp` with
`x13=0xc000001d` (`STATUS_ILLEGAL_INSTRUCTION`), `x19=x2=0x5467438b6880b41f`,
`x23=0`, `x24=0`, `x28=0x160616f294`; sample top is `_sigtramp` plus an unknown
native target around `0x438b6880b19f`. Current next step: enable the narrow
xtajit/BTCpuSimulate exception-status trace (not broad recursive fault qword
dumping), capture guest EIP/status/bytes for the `c000001d` path, then mirror
the working x64 route if this is return/BOP target corruption, or apply opcode
family audit if it is a real unsupported PE32 instruction.

Update 2026-05-28 13:53 VLAT: narrow xtajit trace knobs identified in
`engine/wine/dlls/xtajit/cpu.c`: `MACRUNNER_XTAJIT_TRACE_ALL_SIMULATE`,
`MACRUNNER_XTAJIT_TRACE_SYSCALLS`, and `MACRUNNER_XTAJIT_TRACE_STACK`. Short
trace run `reports/phase-h/npp-x86-xtajit-trace-all-20260528-133941` was
intentionally killed after 20s; with all-simulate enabled it had not yet reached
the `_sigtramp`/`c000001d` state and logged no `publish-exception-context`.
Evidence gathered: PE32 execution is running through normal WOW64 BOP syscalls,
with repeated returns through `NtUserCallOneParam` (`svc=0x133d`, `ret=7a9304ac`)
near the latest trace tail. Next probe must be low-overhead and attach to the
actual child executable line (`.../notepad++.exe Z:\...`), not the parent
`wine start.exe` line.

Update 2026-05-28 18:07 VLAT: low-overhead callback/signal trace
`reports/phase-h/npp-x86-callback-signal-trace-20260528-174647` classifies the
current hot spin as guest32 execute control-flow, not an opcode or missing DLL.
The old x64 callback route still works first (`win32u.dll` target
`0x7ffd06b7bb0`, `ret=0x1`), then WOW64 starts normally
(`BTCpuGetBopCode -> ... guest=00270000`). After loader mapping, the process
enters a tight signal loop at `pc=fault=0xb68000`; sample shows thread #2 stuck
under `_sigtramp` for the whole sample. This is the PE32 analogue of the x64
return-target/callback route: ARM64 native control reached a 32-bit guest code
address and Wine's native signal path keeps re-raising it instead of re-entering
the i386 CPU. Next: mirror the proven x64 signal-route shape, but target the
WOW64/i386 CPU entry path; do not add status-pointer patches.

Patch 2026-05-28 18:14 VLAT: implemented the PE32 mirror route, scoped to
WOW64/I386-on-ARM64 execute faults. `signal_arm64.c` now refuses to let
`virtual_handle_fault()` "fix" a low 32-bit execute fault by mprotect/retry;
instead it raises the normal native exception dispatcher. `wow64/syscall.c`
then recognizes that dispatcher case in `Wow64PrepareForException`, restores
the I386 `Eip` to the low guest PC, and re-enters the existing `cpu_simulate()`
loop. This mirrors the x64 callback route concept while using the established
WOW64 CPU entry instead of inventing a direct ntdll->xtajit path. Next:
rebuild/install/codesign `ntdll.so` and `wow64.dll`, sync prefix, freshness,
then rerun PE32 Notepad++.

Build attempt 2026-05-28 18:16 VLAT:
`artifacts/phase-h/build-wow64-i386-exec-route-20260528-1816.log` rebuilt
`ntdll.so` but failed at `wow64.dll` because the manual make environment did not
prepend `engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin`
(`aarch64-w64-mingw32-clang` not found). This is an environment error, not code
evidence. Next: rerun build with the same LLVM_MINGW exports used by
`scripts/build-wine-pure-arm64-experiment.sh`, then install/sync/freshness and
rerun PE32.

Build/install 2026-05-28 18:18 VLAT: reran with the LLVM_MINGW toolchain env.
`artifacts/phase-h/build-wow64-i386-exec-route-20260528-1817.log` built
`ntdll.so` and `wow64.dll` (`rc=0`, only existing warnings plus one pre-existing
`MESSAGE` format warning in `wow64/syscall.c`). Installed/codesigned
`aarch64-unix/ntdll.so`, installed `aarch64-windows/wow64.dll`, synced
`bottles/generic-x86` via
`reports/phase-h/install-wow64-i386-exec-route-20260528-1818.log`, and freshness
is PASS in
`reports/phase-h/freshness-wow64-i386-exec-route-20260528-1818.log`. Next: direct
scoped PE32 Notepad++ run with callback-route trace enabled, then classify
whether the low-guest execute fault now re-enters `cpu_simulate()`.

Run 2026-05-28 18:20 VLAT:
`reports/phase-h/npp-x86-after-i386-exec-route-20260528-182055` did not reach a
window, but it also did not reproduce the low `pc=fault=0xb68000` spin: the
child exited after ~53s with `0xc0000135`, and markers show
`hb_i386_exec_route=0`, `wow64_i386_exec_route=0`, `signal_low_b68000=0`,
`UNSUPPORTED=0`, `MEMORY_FAULT=0`. The earlier x64 callback route still fires
and returns once. This means the new route was not exercised in this run; next
step is loader evidence for the renewed `STATUS_DLL_NOT_FOUND` exit before
touching CPU/status code.

Rerun 2026-05-28 18:29 VLAT without signal-chain trace:
`reports/phase-h/npp-x86-no-signalchain-20260528-182917` proves the new signal
route does fire, but the target is not code. `Wow64PrepareForException` routed
`pc=00b68000`, then xtajit returned `MEMORY_FAULT` with
`reason=unable to fetch executable i386 code`; memory probe says
`00b60000-01b60000 perm=3` and `can_x=0` (RW stack-like region containing ESP/EBP).
This mirrors the x64 oracle rule more closely: only registered/executable guest
targets are callback/return targets. Patch refined in `wow64/syscall.c` so the
WOW64 native-execute route calls `cpu_simulate()` only when `NtQueryVirtualMemory`
shows an executable guest32 page; non-exec low PCs are left to the normal WOW64
exception reset/dispatch path. Next: rebuild/install `wow64.dll`, sync prefix,
freshness, rerun PE32.

Build/install 2026-05-28 18:33 VLAT: rebuilt the refined `wow64.dll` in
`artifacts/phase-h/build-wow64-i386-exec-nonexec-filter-20260528-1832.log`
(`rc=0`, one pre-existing `MESSAGE` format warning), installed/synced prefix in
`reports/phase-h/install-wow64-i386-exec-nonexec-filter-20260528-1833.log`, and
freshness is PASS in
`reports/phase-h/freshness-wow64-i386-exec-nonexec-filter-20260528-1833.log`.
Next: rerun PE32 Notepad++ and verify the stack/non-exec low-PC case no longer
enters `cpu_simulate()`.

Run 2026-05-28 18:34 VLAT:
`reports/phase-h/npp-x86-after-nonexec-filter-20260528-183425` confirms the
filter prevented the bogus `cpu_simulate()` MEMORY_FAULT (`memory_fault=0`), but
it exposed the next root: `Wow64PrepareForException` now spins on the same
non-exec stack PC, logging `native guest32 execute skip nonexec pc=00b68000
state=MEM_COMMIT protect=PAGE_READWRITE` 31k times. `BTCpuResetToConsistentState`
in our xtajit backend is a stub, so returning to Wine's native dispatcher cannot
consume a low guest32 execute fault. Next: mirror x64's evidence rule further and
classify the BOP/return target source for `pc=00b68000`; do not add status-pointer
patches.

Trace 2026-05-28 18:41 VLAT:
`reports/phase-h/npp-x86-module-until-nonexec-20260528-184109` shows another
root-cause clue before the low-PC spin: xtajit publishes exceptions for valid
guest `ntdll` EIPs (`7bde7e06`, `7bde1458`) with host addresses under
`0x27bde...` and all-zero bytes, while module mapping in the same run maps PE32
system DLLs under `0x3....`. This means xtajit's local `guest32_host_ptr()`
derives the guest32 base from `teb + WowTebOffset`, unlike the fixed WOW64 helper
path that uses `MACRUNNER_WOW64_TLS_GUEST32_BASE`. Patch applied in
`xtajit/cpu.c`: use the same TLS guest32 base for xtajit host-pointer
translation, falling back to the old WowTebOffset derivation only if the TLS slot
is absent. Next: rebuild/install/codesign `xtajit.dll`/`xtajit.so`, freshness,
rerun PE32.

Build/install 2026-05-28 18:46 VLAT: rebuilt `xtajit.so` and `xtajit.dll`
cleanly after moving declarations above code:
`artifacts/phase-h/build-xtajit-guest32-tls-base-clean-20260528-1846.log` has no
warnings/errors. Installed/codesigned/synced prefix in
`reports/phase-h/install-xtajit-guest32-tls-base-clean-20260528-1846.log`; build
freshness PASS in
`reports/phase-h/freshness-xtajit-guest32-tls-base-clean-20260528-1846.log`.
Next: rerun PE32 Notepad++ and classify the next boundary.

Run 2026-05-28 18:47 VLAT:
`reports/phase-h/npp-x86-after-xtajit-tlsbase-20260528-184739` no longer enters
the non-exec low-PC route (`nonexec_skip=0`, `memory_fault=0`), but exits after
~53s with `0xc0000135`. Loader trace
`reports/phase-h/npp-x86-loader-after-tlsbase-20260528-184901` timed out under
heavy `+module,+loaddll` before exit, but shows the status already held in
native x64-side state during callback-route rejects (`x19=0xc0000135` at
`raw_pc=0x7ffd0ada578`, `fault=0x48`). Next: find the exact failing
loader/LdrLoadDll return status/name with targeted instrumentation or narrower
trace; do not infer a DLL name from static imports.

Probe patch 2026-05-28 18:56 VLAT: added capped `ERR` instrumentation in
`ntdll/loader.c` (`macrunner-ldr-load-dll-fail` and
`macrunner-ldr-LdrLoadDll-fail`) to print the exact failing loader status/name
under `WINEDEBUG=-all`. This is diagnostic-only and evidence-first for the
current `0xc0000135`; next rebuild PE `ntdll.dll` for the relevant arches, sync
prefix, rerun PE32, then either fix the named dependency/root or remove/narrow
the probe once it has served.

Build/install 2026-05-28 18:54 VLAT: PE `ntdll.dll` probe build for `aarch64`
and `i386` succeeded, but `x86_64-windows/ntdll.dll` still fails to link with
pre-existing `___chkstk_ms` unresolved in the x64 PE ntdll link
(`artifacts/phase-h/build-ntdll-loader-status-probe-fullenv-20260528-1853.log`).
Installed only the successful `aarch64` and `i386` probed DLLs, then synced
prefix:
`reports/phase-h/install-ntdll-loader-status-probe-partial-20260528-1854.log`;
freshness remains PASS in
`reports/phase-h/freshness-ntdll-loader-status-probe-partial-20260528-1854.log`.
Next run may identify failures from the 32-bit loader; if the status originates
only in the x64 PE loader, solve the x64 ntdll link or instrument another layer.

Run 2026-05-28 18:55 VLAT:
`reports/phase-h/npp-x86-ldrprobe-i386-20260528-185537` did not reach the
previous `0xc0000135` exit within 80s; child stayed hot at ~98% after the first
two `NtMapViewOfSection` mappings and produced no loader-fail probe lines. This
means the partial i386/aarch64 loader probe changed timing/control enough to
reveal a hot spin before the prior exit. Next: capture a proper sample/lldb on
this hot-spin shape, then decide whether to keep the diagnostic probe or remove
it after extracting evidence.

Update 2026-05-27 22:18 VLAT: continued PE32/WOW64 only. Do not touch the x64
golden snapshot except as read-only reference. Current local x64 `signal_arm64.c`
diff belongs to earlier x64 work; do not extend it for PE32 unless new evidence
requires the signal layer.

PE32 Notepad++ current exact env still launches a child `notepad++.exe`, but no
Notepad++ CG window appears. Important reports:

- `reports/phase-h/npp-x86-child-spin-probe-20260527-214609`: child PID spins
  ~99% CPU; lldb caught `EXC_BAD_ACCESS address=0x17` at ARM64 PE `xtajit.dll`
  `handle_bop_context`, instruction `str wzr, [x20]` after
  `xtajit_arm64_call_wow64_syscall`. Objdump maps it to `*status =
  STATUS_SUCCESS`. Classification: host `NTSTATUS *status` pointer was kept in
  ARM64 callee-saved `x20` across the WOW64 syscall boundary; that boundary can
  restore guest ARM64 state into `x20`.
- `reports/phase-h/npp-x86-after-statusptr-volatile-20260527-220110`: attempted
  stack reload changed the fault to `address=0x0` at the reloaded status pointer.
  This disproves "save status pointer in host stack slot" as sufficient; the
  stack slot is not reliable after this boundary either.
- Fix now in `engine/wine/dlls/xtajit/cpu.c`: for syscall/unix BOP handling,
  set `*status = STATUS_SUCCESS` before cross-boundary dispatch, and do not store
  success status after dispatch on the hot path. Rebuilt/copied
  `xtajit.dll`/`xtajit.so`; build log
  `artifacts/phase-h/build-xtajit-status-before-dispatch-20260527-220411.log`
  (`rc=0`, one existing C89 declaration-after-statement warning in
  `BTCpuSimulate`).
- `reports/phase-h/npp-x86-after-status-before-dispatch-20260527-220526`:
  old `x20/status` store fault is gone. New current blocker is a separate
  control-flow family: child still spins ~99% CPU, lldb sees thread #2 at
  `PC=0x0` / `EXC_BAD_ACCESS address=0x0`; no `UNSUPPORTED_OPCODE`, no
  `c000001d`, no `MEMORY_FAULT`, no unhandled Wine exception.
- `reports/phase-h/npp-x86-syscall-trace-after-status-20260527-221130`:
  do not misclassify `NtQuerySystemInformation class=102` as terminal. With
  syscall tracing, execution logs later BOP syscalls and `svc=00000004` wait
  returns `0x102`; the next blocker is the null-PC/control-flow path, not a
  missing NtQSI leave log.

Next step: evidence-first classify the `PC=0` return target path. Start from
the latest null-PC reports above; do not add more status-pointer patches, and do
not broaden into x64 callback routing without a trace proving the signal layer is
the active route.

Update 2026-05-27 04:34 VLAT: PE32 Notepad++ HyperBridge boundary progressed
past the x87 environment-control blocker and two later CPU-family gaps. Fixed:

- x87 environment-control family `FWAIT/FNCLEX/FCLEX/FNINIT/FINIT` through
  decode -> IR -> lift -> interpreter -> tests.
- x86 SSE scalar/convert family copied from the already-working x64 path for
  the Notepad++ block at `0x005455d9`: `CVTDQ2PD`, `CVTPD2PS`, `ADDSD`,
  `DIVSS`, `COMISS` and sibling packed/scalar forms.
- x87 `FRNDINT` (`D9 FC`) for the later Notepad++ helper at `0x00782860`.

Validation:

- `engine/hyperbridge/tests/hb_test_runner`: `292 passed, 0 failed`.
- Rebuilt `engine/hyperbridge/libhyperbridge.a`.
- Force-relinked and copied
  `engine/wine/dist-pure-arm64/lib/wine/aarch64-unix/xtajit.so`.
- Bounded 120s PE32 Notepad++ run:
  `reports/phase-h/npp-x86-post-x87-frndint-20260527-043102`.
  Result: `UNSUPPORTED_OPCODE=0`, `c000001d=0`, old `01b5fd90` stack-execute
  loop gone, only one handled early AV remains, process reaches wait/message
  loop (`svc=00000004` returned `0x102`).

Do not touch Wine-render hacks for the remaining visual/window question. The
CPU boundary is now past the known Notepad++ PE32 unsupported opcodes; if the
window is still not visible, keep that separated from HyperBridge opcode
coverage unless a new CPU fault appears.

Update 2026-05-26 21:24 VLAT: PE32 x87 environment-control family is
implemented through decode -> IR -> lift -> interpreter -> tests for
`FWAIT/FNCLEX/FCLEX/FNINIT/FINIT`; HyperBridge validation is green
(`hb_test_runner`: 289 passed, 0 failed; Python x86/decode/IR: 26 passed).
PE32 safety probe after the x64 detour reached `process_ready=1`; the only
error scan hit was the pre-existing `non-application-target pc=0x7ffd02000b0`
safeprobe tail, not a new x86 HyperBridge memory fault.

Temporary x64 detour status: Obsidian notes confirm x64 Notepad++ really did
work (`109-milestone-real-onscreen-window-after-bootstrap-marathon.md`,
`95-achievements-and-multi-agent-state.md`), and report
`reports/phase-h/npp-x64-20260523-234453` reached `FULL_UI_READY`. Current
regression root was a stale ARM64 `x4` register being trusted as an x64 callback
target after `RtlRunOnceExecuteOnce` / `kernelbase!init_current_version`; this
misrouted an ordinary native ntdll fault to Notepad++ thunk `0x140004930`, then
crashed at `0x1400059b0` with `rcx=0x208`. Fixed in
`engine/wine/dlls/ntdll/unix/signal_arm64.c`: trust `x4` as a normalized x64
callback target only when the fault PC/address already identifies x64 guest
execution. Rebuilt/installed/codesigned `aarch64-unix/ntdll.so`; build
freshness is PASS. Post-fix x64 probe no longer reproduces the
`0x140004930 -> 0x1400059b0` crash and stays alive ~17s, but current manual
readiness still fails `CG_WINDOW_READY`; next x64 step is compare against the
2026-05-23 milestone/canonical window path rather than change Wine-render hacks.

Update 2026-05-26 08:34 VLAT: current session stopped to restart Codex after a
`context-mode` MCP transport failure. Resume from
`reports/phase-h/CODEX-RESUME-PE32-X87-CONTEXTMODE-20260526.md`.

Context-mode fix applied outside the repo: killed runaway
`node /opt/homebrew/bin/context-mode` pid `6151` (~96% CPU, ~3.1 GB RSS),
checkpointed/truncated the stale
`~/.claude/context-mode/content/4746962d34e0be46.db-wal` from ~638 MB to 0 B,
and pinned Codex MCP storage in `~/.codex/config.toml`:
`CONTEXT_MODE_DIR=/Users/timurtoby/.codex/context-mode`. New Codex session is
required because the current MCP transport is closed.

PE32 Notepad++ HyperBridge status: `NtContinue(context, TRUE)` small-sentinel
bug is fixed in `wow64_NtContinueEx`, and xtajit reset-context handling resumes
at i386 `RtlUserThreadStart` (`7bde146c`, then `7bde147c`). Fresh blocker is
`UNSUPPORTED_OPCODE` in Notepad++ PE32 at guest `0x0075709d`, bytes `db e2`,
decoded as `fnclex`. Required next implementation is x87 environment-control
family audit/fix: `FWAIT 9B` as NOP, `FNCLEX DB E2` / `FCLEX 9B DB E2`,
`FNINIT DB E3` / `FINIT 9B DB E3`, through x86 decode -> IR -> lift ->
interpreter -> tests. Do not patch around Notepad++; fix the opcode family.

Update 2026-05-25 15:36 VLAT: active repo is now the internal SSD copy
`/Users/timurtoby/Documents/MacRunner/Main/MacRunner`; `/Volumes/MacOS/MacRunner`
is stale/archive. `AGENTS.md`, `config/env.sh`, active docs, runner scripts,
Control Center defaults/tests, generated Wine build metadata, and current prefix
registry were moved to the internal path. `ccache` is active under
`artifacts/ccache`; `scripts/verify-build-freshness.sh` is PASS.

Implemented the reboot handoff fix in
`engine/wine/dlls/wow64/virtual.c:wow64_NtQueryVirtualMemory`: current-process
WOW64 queries now keep low `addr32` for the 32-bit contract/range checks and use
`guest32_host_ptr(addr32)` only for native `NtQueryVirtualMemory()`. Rebuilt and
installed `wow64.dll`; rebuilt/installed/codesigned `ntdll.so` against current
`libhyperbridge.a`. Evidence: in
`reports/phase-h/npp-x86-queryvm-fix-lane-20260525152549/run.json`,
`svc=00000023` returns `status=00000000`; previous `alloc_module`/`EDI=0`
boundary is gone.

Also fixed the launcher routing needed for this PE32 HyperBridge work:
`scripts/run-windows-app.sh --lane arm64-hyperbridge` now passes the lane to
configurator, and `config/engines.json` allows explicit `arm64-hyperbridge` for
`x86`. Default PE32 launch remains `x86-rosetta-wow64`; pure HyperBridge is
explicit.

Latest PE32 run after TEB mirror fix:
`reports/phase-h/npp-x86-teb32-actctx-20260525153236/run.json`, exits `rc=123`.
The `TEB32+0x1a8` memory fault is gone (`MEMORY_FAULT=0`). Current fresh
boundary is loader returning `STATUS_INVALID_IMAGE_FORMAT (c000007b)` later in
the PE32 load path, around mapped image addresses `6fe5/6fe6...`. Next exact
step: source-search the i386 loader path that sets `c000007b` around those image
validation branches, then add a focused trace if the static path is ambiguous.
Do not revert to Rosetta and do not patch loader symptoms.

Update 2026-05-25 06:46 VLAT: `NtAllocateVirtualMemory` current-process
guest32 translation is now installed in `wow64.dll`; rerun proved the previous
`c0000019` heap commit boundary is fixed. New evidence from
`MACRUNNER_HB_TRACE_FAULTS=1`: HyperBridge stopped at i386
`guest=0x7bd81113 bytes=66 89`, i.e. operand-size-prefixed
`MOV r/m16,r16` in i386 `ntdll`. Fixed the MOV operand16 family in
`hb_decode_x86.c` (`66 89`, `66 8b`, `66 b8+rw`, `66 a1/a3`, plus LEA size
selection) and the shared i386 partial-register mechanism in
`hb_interpreter.c` (`write_reg_sized`, sized LOAD/STORE register operands).
Validation: `engine/hyperbridge make test` PASS, then `xtajit.dll`/`xtajit.so`
rebuilt, installed, and `xtajit.so` codesigned. Next: rerun PE32 Notepad++ and
classify only the next fresh boundary.

Update 2026-05-25 06:21 VLAT: incorporated the new ultra-native strategy as
validation direction, but kept the active scope narrow: PE32 Notepad++ under
pure arm64 Wine + `xtajit`/HyperBridge. Latest evidence after the size128 and
TEB-mirror fixes: `NtAllocateVirtualMemory` reserve succeeds, but the following
commit of the same heap reservation fails with `STATUS_CONFLICTING_ADDRESSES`
(`c0000019`). DISPROVED: BOP syscall stack offset is wrong; raw BOP stack proves
`args = ESP+8` is correct. Current boundary: direct guest32 window requires
current-process WOW64 virtual-memory base-address APIs to pass host pointers to
native `Nt*VirtualMemory`, while returning low 32-bit guest VAs to i386 code.
Patch in progress: map current-process `NtAllocateVirtualMemory`,
`NtAllocateVirtualMemoryEx`, `NtFreeVirtualMemory`, and
`NtProtectVirtualMemory` base addresses through `guest32_host_ptr()`.

Update 2026-05-25 04:48 VLAT: PE32 Notepad++ advanced past the WOW64 syscall
guest-pointer boundary and the stack-as-code failure. Root for the latter was
near `RET imm16` / stdcall cleanup being dropped; fixed the full RET family path
(`decode -> IR -> interp -> JIT-helper`) and added decoder/interpreter tests for
x86/x64 `C3` and `C2 iw`. Current concrete boundary is TEB32 addressing for
guest i386 `fs:` loads. Probe at `7bdb06a0` showed `64 8b 0d 18 00 ...`
(`mov ecx, fs:[0x18]`); first mirror attempt using low32(host TEB32) was
DISPROVED because it collided with i386 `ntdll` code around `0x7bdb6917` and
overwrote executable bytes with TEB data. Active fix: allocate a dedicated safe
guest32 TEB mirror in `0x70000000..0x7f000000`, copy host TEB32 into it before
`BTCpuSimulate`, patch guest `TIB.Self` to the guest base, use that as
`fs_base`, and copy back only `Tib.ExceptionList` after successful simulation.
Next: rebuild/install `xtajit`, rerun PE32 NPP with byte probe, and classify the
next boundary by fresh evidence only.

Update 2026-05-25 00:16 VLAT: PE32 Notepad++ advanced past the x86 decoder
family blockers (`POP r/m32`, operand16 Group1 `80/81/83`, operand16 `C7`) and
the `BTCpuSetContext` low-pointer crash. Current concrete boundary is WOW64
syscall pointer aliasing: `wow64_NtRaiseException()` received guest32
`EXCEPTION_RECORD32`/`I386_CONTEXT` pointers and `exception_record_32to64()`
dereferenced the low guest address natively, crashing in `wow64.dll` at
`exception_record_32to64` (`ldr d0, [x19]`, fault around `0x00d5f874`). Applied
the boundary fix in `dlls/wow64/syscall.c`: map i386 syscall context/exception
pointers through `guest32_host_ptr()` before native dereference in
`NtRaiseException`, `NtContinueEx`, `NtGetContextThread`, and
`NtSetContextThread`. Next: rebuild/install `wow64.dll`, rerun clean PE32 NPP,
then classify the next boundary by fresh evidence only.

Update 2026-05-24 23:19 VLAT: completed the first build integration for the new
PE32 CPU module. Reconfigured `engine/wine/build-pure-arm64`, built
`dlls/xtajit/aarch64-windows/xtajit.dll` plus `dlls/xtajit/xtajit.so`, installed
them into `engine/wine/dist-pure-arm64/lib/wine/aarch64-windows/` and
`aarch64-unix/`, and signed `xtajit.so`. Artifact check: `xtajit.dll` is PE32+
AArch64, `xtajit.so` is Mach-O arm64, codesign verifies, and BTCpu exports
include `BTCpuGetBopCode`, `BTCpuProcessInit`, `BTCpuThreadInit`,
`BTCpuSetContext`, `BTCpuSimulate`, `BTCpuNotifyMemoryAlloc`, and
`__wine_get_unix_opcode`. Remaining blocker before real PE32 launch: Wine's
WOW64 virtual/image mapping must copy/map PE32 bytes into `guest32_window`
backing, not merely notify VMA metadata.

Update 2026-05-24 23:06 VLAT: started the Wine WOW64 CPU DLL layer at the
correct module boundary. Evidence from `dlls/wow64/syscall.c` and
`wine.inf.in`: on native ARM64 with i386 guest, Wine loads `xtajit.dll`
(`Software\\Microsoft\\Wow64\\x86`), not `wow64cpu.dll`; existing tree only had
`xtajit64.dll` for amd64/ARM64EC. Added new `dlls/xtajit` source: PE `cpu.c`
exports the `BTCpu*` surface and calls a Unixlib; Unix `unixlib.c` owns the
HyperBridge-backed `hb_wow64_process_t`/thread state, context import/export,
`hb_wow64cpu_simulate()`, and initial memory notify wiring. Added configure
entries for `enable_xtajit`/`dlls/xtajit`. Local syntax check for both new
sources is clean; HyperBridge validation remains PASS (`202 passed, 0 failed`,
Python `41 tests OK`). Remaining before runnable PE32: regenerate/build Wine
target and fix real winebuild/makedep issues, then wire low-VA image copy into
guest32 mappings.

Update 2026-05-24 22:49 VLAT: added the internal WOW64 execution contract
slice. `hb_wow64cpu_simulate()` now validates the i386 thread/process contract,
fetches executable bytes from the direct `guest32_window` at `EIP`, decodes via
`hb_decode_x86()`, lifts via `hb_lift_func_x86()`, and runs the bounded block via
the selected HyperBridge backend (AOT currently falls back to interpreter). Added
regression `wow64cpu_simulate_runs_i386_guest32_block`, proving a Win32-context
`mov eax, imm32` block executes from guest VA `0x00400000` and exports updated
`EAX/EIP`. Validation: `engine/hyperbridge make test` PASS; C runner
`202 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 22:35 VLAT: added the third x87 batch: ST(i)/pop-register
forms. Covered decode -> lift -> interpreter for `FADD`/`FMUL`/`FSUB`/`FSUBR`/
`FDIV`/`FDIVR` against `ST(i)`, pop variants `FADDP`/`FMULP`/`FSUBP`/
`FSUBRP`/`FDIVP`/`FDIVRP`, `FCOM`/`FCOMP` register forms, `FCOMPP`, `FLD ST(i)`,
and `FXCH`. Added regression `interp_x86_x87_stack_register_pop_core` for
ST-stack arithmetic, `FXCH`, `FADDP`, `FCOMPP`, final empty tag word, and C3
status export. Validation: `engine/hyperbridge make test` PASS; C runner
`201 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 22:27 VLAT: added the second x87 batch: memory
arithmetic/compare (`D8/DC /0..7`) through decode -> lift -> interpreter. Covered
`FADD`/`FMUL`/`FSUB`/`FSUBR`/`FDIV`/`FDIVR` against `m32real/m64real`, plus
`FCOM`/`FCOMP` status-word semantics (`C0`/`C2`/`C3`, pop on `FCOMP`). Added
`hb_x87_set_st_f64()` and `hb_x87_fcom()` helpers. Regression
`interp_x86_x87_memory_arithmetic_compare_core` verifies arithmetic result,
`FCOM equal => C3`, and final empty tag word. Remaining x87 P0 gap is the
separate ST(i)/pop-register variant batch (`FADDP`/`FSUBP`/`FMULP`/`FDIVP`,
`FCOMPP`, `FXCH`). Validation: `engine/hyperbridge make test` PASS; C runner
`200 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 22:21 VLAT: added the first x87 decode/lift/interpreter
family batch: load/store/control/conversion (`FLD m32/m64`, `FSTP m32/m64`,
`FILD m16/m32/m64`, `FISTP m16/m32/m64`, `FLDCW`, `FNSTCW`, `FNSTSW AX`).
This deliberately does not mix in the separate arithmetic/compare batch
(`FADD`/`FSUB`/`FMUL`/`FDIV`/`FCOM*`), which remains next x87 work. Also fixed a
root decoder-contract bug: `hb_decode_next()`/`hb_decode_at()` now dispatch to
`hb_decode_x86()` for `HB_ARCH_X86`; before this they always used the x64
decoder. Regression `interp_x86_x87_load_store_control_conversion_core` verifies
guest32-backed x87 CW load/store, integer conversion, real64 load/store, and
status-word export. Validation: `engine/hyperbridge make test` PASS; C runner
`199 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 22:01 VLAT: added the first PE32 ABI contract slice. The x86
ABI helpers now validate `HB_ARCH_X86`/32-bit mode/memory presence and propagate
guest-stack write faults instead of silently ignoring them. Added fastcall and
thiscall setup APIs (`ECX`/`EDX` and `ECX=this`, respectively) beside existing
cdecl/stdcall. Regression `x86_abi_calling_convention_stack_contracts` verifies
argument order, return sentinel placement, register arguments, target `EIP/pc`,
and fault behavior on an unmapped guest stack. Validation:
`engine/hyperbridge make test` PASS; C runner `198 passed, 0 failed`, Python
suite `41 tests OK`.

Update 2026-05-24 21:53 VLAT: closed the first real x86 execution-path slice on
top of `guest32_window`. `resolve_addr()` now treats every `HB_MODE_32BIT`
effective address as a wrapped 32-bit guest VA, including segment bases, instead
of requiring the x64-only `addr32` IR flag. Fixed `hb_lift_func_x86()` to set
`func->cfg->entry` like the x64 lifter; without that, x86 lifted functions could
not execute. Added regression
`interp_x86_guest32_memory_operands_wrap_to_direct_window`: `ebx=0xffffffe0`
plus disp32 wraps to guest addresses `0x4/0x8` and reads/writes through the
direct guest32 arena. Validation: `engine/hyperbridge make test` PASS; C runner
`197 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 21:32 VLAT: added the HyperBridge-side WOW64 CPU contract
spine. New `hb_wow64cpu` C ABI exposes versioned process/thread/context structs,
process init with guest32 arena reservation, thread init with x86 context, BOP
opcode retrieval, i386 context import/export, and memory notify wrappers wired to
guest32 VMA map/protect/free. This is not yet the PE arm64 `xtajit.dll`; it is
the stable internal ABI that `BTCpuProcessInit`/`BTCpuThreadInit`/`BTCpuSimulate`
can wrap next. Validation: `engine/hyperbridge make test` PASS; C runner
`196 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 21:25 VLAT: added the first x87 foundation slice for the
PE32 vertical MVP. `hb_regs_x86_t` now owns an `hb_x87_state_t` with default
control word/tag/top state. Added `hb_x87` helpers for reset, stack push/pop,
`FLDCW`/`FNSTCW`, and `FISTP i32` with x87 control-word rounding modes. This
targets the CRT-critical `_ftol`/integer-conversion path before full x87 decode.
Validation: `engine/hyperbridge make test` PASS; C runner
`193 passed, 0 failed`, Python suite `41 tests OK`.

Update 2026-05-24 21:17 VLAT: implemented the next PE32 memory-layer slice in
HyperBridge. Added a 4GB-aligned `guest32_window` direct mapping API:
`hb_memory_guest32_reserve/base/to_host/map/protect/unmap`, VMA generation
counters, and guest32 range split/protect/free tests. Validation:
`engine/hyperbridge make test` PASS; C runner `191 passed, 0 failed`, Python
suite `41 tests OK`.

Important boundary found during implementation: Apple Silicon host pages are
16KB while Win32 guest pages are 4KB. Therefore host `mprotect()` cannot be the
source of truth for exact Win32 4KB permissions. The direct-window path now uses
VMA metadata for 4KB guest permissions/invalidation generation and host backing
only at host-page granularity. This keeps the NO-SoftMMU model, but the future
x86-32 JIT must consult/encode VMA permission assumptions for faults/SMC instead
of relying on host page faults for every 4KB guest page.

Update 2026-05-24 18:31 VLAT: started the real HyperBridge memory-layer work for
the PE32 blocker. Added `hb_memory_map_private()` so a guest fixed VA such as
`0x400000` can be represented as guest address `0x400000` while backed by a host
mapping above macOS `__PAGEZERO`. Read/write/protect now use `host_base` for
allocated/private regions and preserve existing live-host mappings for Wine-owned
native ranges. Added regression `memory_private_guest_low_va_backing`; validation:
`engine/hyperbridge make test` PASS and C runner reports `189 passed, 0 failed`.
This is a foundation piece only; it does not yet make Wine WOW64 launch PE32.
The remaining boundary is still the Wine WOW64 process-parameter / image mapping
layer plus an arm64 `xtajit.dll`/HyperBridge x86 CPU module.

Update 2026-05-24 17:04 VLAT: retried forced pure-arm64 HyperBridge launch for
32-bit Notepad++ after clearing stale Wine processes. Result is unchanged at the
real blocker:
`reports/phase-h/npp-x86-forced-hyperbridge-20260524070209./`.
Failure key lines:
`map_fixed_area out of memory for 0x400000-0xb51000`,
`WINEPRELOADRESERVE ... overlaps preloader __PAGEZERO`, then
`Assertion failed: (!status), function build_wow64_parameters, file env.c,
line 1823`. This confirms again that the failure is low 32-bit VA/WOW64 CPU
integration, not missing DLL installation.
Also tested the low-pagezero arm64 probe with the valid local Apple Development
identity and Wine entitlements:
`artifacts/phase-h/lowpagezero-appledev-probe/`. It is still killed by macOS,
so ad-hoc signing was not the cause of the low-VA wall.
Added configurator fail-fast so PE32 cannot be forced onto `arm64-hyperbridge`
while the x86 WOW64 CPU/memory layer is absent. Implementation boundary is
captured in
`reports/engine-audit/X86-HYPERBRIDGE-WOW64-MEMORY-LAYER-SPEC.md`.
FEX's public Wine WOW64 backend confirms the needed `xtajit` contract but is not
a macOS drop-in: it also allocates bridge trampolines in the lower 2GB, which is
the address range blocked by macOS arm64 `__PAGEZERO`.

For immediate dev/manual testing only, a 32-bit Notepad++ window was launched
through the existing compatibility lane `x86-rosetta-wow64`:
`reports/phase-h/npp-x86-dev-rosetta-hold-20260524070345./`.
Window verified by CGWindow: `new 1 - Notepad++ [Administrator]`, owner
`notepad++.exe`. This is explicitly not pure HyperBridge. The fallback window was
closed before continuing pure-HyperBridge work to avoid confusing it with a real
x86 HyperBridge launch.

Update 2026-05-24 16:32 VLAT: 32-bit Notepad++ dev path is bounded. Dev launch
through `scripts/run-windows-app.sh` opens
`artifacts/phase-h/npp-x86/notepad++.exe`, but that is the `x86-rosetta-wow64`
lane. Forced pure-arm64 launch with absolute path and `WINEARCH=wow64` still
fails before UI:
`reports/phase-h/npp-x86-pure-arm64-abs-20260524-162402/`.
Root boundary: not prefix/DLL setup. The i386 PE wants low `0x400000`, and
`build_wow64_parameters()` needs low <2GB process parameters, while both
`dist-pure-arm64/bin/wine` and `wine-preloader` have 4GB `__PAGEZERO`. Minimal
low-pagezero arm64 probes are killed by macOS (`rc=137`), and deallocating/mapping
low `0x400000` from a normal arm64 process is also killed. Second blocker:
`dist-pure-arm64` has only `x86_64-windows/wow64cpu.dll`; there is no arm64
`wow64cpu` that routes Wine WOW64 to HyperBridge x86. Detailed report:
`reports/engine-audit/X86-PURE-HYPERBRIDGE-WOW64-BLOCKER.md`. Honest current
32-bit lane remains Rosetta until a HyperBridge-backed WOW64 CPU/memory layer is
built.

INSTALLER phase is active. PERF phase is complete enough for the requested task
order. Product Notepad++ Installed E2E now launches through `MacRunner.app` with
the live engine/dist and a freshly synced installer prefix. The Open dialog blank
list regression is closed by the shell32 drive-PIDL path fix and verified on the
real product window, but manual acceptance found a remaining shell-dialog visual
bug: the Open dialog file list is populated, while the left namespace pane is
blank and details-list item icons are missing.

Freeze-on-app-switch remains open, but current automation did not reproduce it;
do not spend cycles on focus-toggle or synthetic clicks. The next freeze step is
to use the newly installed lifecycle trace on the next real manual repro.

Performance work is evidence-driven on real Notepad++ UI-smoke scenarios. Current
artifacts:

- Baseline active sample: `reports/performance/npp-active-20260523-231858/`
- Trace-gate fix sample: `reports/performance/npp-post-tracegate-20260523-233115/`
- Live-region cache sample: `reports/performance/npp-post-live-region-cache-20260523-233849/`
- Block trace-gate sample: `reports/performance/npp-post-block-tracegate-20260523-234449/`
- IR-cache sample: `reports/performance/npp-post-ir-cache-20260523-235347/`
- Read-cache failure sample:
  `reports/performance/npp-post-live-read-cache-20260524-025717/`
- Control after read-cache revert:
  `reports/performance/npp-after-read-cache-revert-20260524-030411/`
- Boundary/env cleanup samples:
  `reports/performance/npp-post-direct-native-fastpath-20260524-031847/`,
  `reports/performance/npp-post-envgate-cache-20260524-032504/`,
  `reports/performance/npp-post-native-write-env-cache-20260524-033235/`,
    `reports/performance/npp-post-special-write-env-cache-20260524-033815/`,
    `reports/performance/npp-post-memory-region-lookup-cache-20260524-034541/`,
    `reports/performance/npp-post-import-target-map-20260524-040821/`,
    `reports/performance/npp-direct-live-mem-optin-20260524-041256/`,
    `reports/performance/npp-post-direct-live-mem-default-20260524-041702/`

Measured fixes applied:

- Cached interpreter trace env flags per `hb_interpreter_run`; clean run no longer
  calls SIMD/mem/branch tracing probes per instruction. `trace_simd_data_exec`
  samples dropped 1892 -> 0.
- Cached successful dynamic live VM regions from `macrunner_hb_special_read/write`
  into HyperBridge memory metadata. `macrunner_hb_special_read/write` samples
  dropped to 0 and `mach_vm_region` dropped from 2000 to near-zero in the next
  comparable sample.
- Cached block-level diagnostic gates in `macrunner_hb_run_x64`; clean run no
  longer calls calc/NPP-open trace probes per block. Both dropped to 0 samples.
- Added per-run IR block cache by guest PC. It reduced `macrunner_hb_lift_one_block`
  from 1011 -> 44 and `hb_lift_func_x64` from 914 -> 37 in the post-cache sample.
- Avoided duplicate guest-stack reads when a direct-native target is already a
  registered import thunk; the registered path now routes to `call_import_thunk`
  before the generic direct-native argument read.
- Cached clean-path diagnostic env gates in `macrunner_hb.c` and native-write
  trace env gates in `hb_memory.c` / `macrunner_hb_special_write`.
- Removed one duplicate `hb_memory_find_region` lookup from successful
  `hb_memory_read/write` by carrying the permission-check region into the
  read/write body.
- Added a target hash map for registered import thunks so direct-native target
  routing no longer linearly scans the import table on the hot path.
- Enabled direct same-process live memory read/write by default for nonallocated
  HyperBridge live regions, with `MACRUNNER_HB_DISABLE_DIRECT_LIVE_MEM=1` as an
  emergency rollback knob. This avoids the Mach VM syscall path for normal
  readable/writable in-process regions while keeping the old path available.

Functional verify after each perf patch: `scripts/verify-build-freshness.sh` PASS,
NPP UI-smoke `overall_effective=PASS`, `case=clean_exit status=PASS`. Visual/icon
paths were not changed.

Latest clean PERF sample is
`reports/performance/npp-post-direct-live-mem-default-20260524-041702/`.
It is functionally green (`overall_effective=PASS`, clean exit) and improved the
same full UI-smoke wall estimate from `169s` in
`npp-post-import-target-map-20260524-040821/` to `157s`. Sample deltas in the
same scenario: `hb_runtime_run -8.3%`, `hb_interpreter_run -8.2%`,
`exec_instr -7.2%`, `hb_memory_read -8.3%`, `mach_vm_read_overwrite -9.5%`,
`server_select -8.5%`. PERF phase is good enough to move back to the P0 freeze
track; remaining perf work is later import arity/prototype evidence and JIT
coverage, not a blocker for the requested task order.

## Current Bug

Open dialog shell visuals are active for installer acceptance.

Update 2026-05-24 12:37 VLAT: user screenshot shows the file list populated
(`autoCompletion`, `plugins`, `notepad++.exe`, etc.) but no left-side
`Favorites/Desktop/My Computer` tree and no file/folder glyphs in the details
list. Static boundary:

- left pane: `shell32/ebrowser.c` reserves nav-pane space and calls
  `CoCreateInstance(CLSID_NamespaceTreeControl)`, but no real
  `INameSpaceTreeControl2` implementation is present in this tree; only the
  caller/event sink exists. This explains a reserved but blank nav pane.
- list icons: `shell32/shlview.c` attaches `Shell_GetImageLists()` and inserts
  rows with `I_IMAGECALLBACK`; `LVN_GETDISPINFO` maps rows through
  `SHMapPIDLToSystemImageListIndex()`. Next boundary is whether the system image
  list/indices are valid or whether the listview draw/callback path drops them.

Do not mark installer E2E complete until the real Open dialog shows namespace
tree entries and file/folder icons onscreen.

Notepad++ x64 freeze remains active but deprioritized. User reproduced a real
click/event freeze during the live human-in-the-loop run.

Update 2026-05-24 12:25 VLAT: user later reported the freeze did not reproduce
while switching between windows, but prior Claude-Code switch freezes remain
unclosed. Keep the boundary below and only resume freeze work on a fresh real
repro.

Captured boundary from `reports/phase-h/npp-x64-20260523-212033/stderr.log`:
real clicks reached `NSApplication sendEvent`, `handleMouseButton`, and
`WineEventQueue` (`queue_post` + `queue_signal rc=1`). After line 97212,
Notepad++ pid `35490` stopped entering `macdrv_ProcessEvents`; the queue kept
growing to about 41 pending events. Therefore the break is between queue signal
and Wine-side wake/drain, not wndproc, not render, not syscall/native target.

DISPROVED fix attempt: `OnMainThread` re-signaling `WineEventQueue` after a
QUERY_EVENT-only wait is not sufficient. Rebuild/install/codesign of
`winemac.so` passed freshness, but run
`reports/phase-h/npp-x64-20260523-213910/` froze before manual acceptance.
Fresh sample paths:
`reports/phase-h/MILESTONE/real-event-freeze-after-resignal-20260523-213910/pid-37713-freeze-214656.sample.txt`
and `pid-37741-freeze-214702.sample.txt`.

New boundary from `npp-x64-20260523-213910/stderr.log`: NPP pid `37713` received
real AppKit events (`app_sendEvent_*`/`handleMouseButton_*` all returned) and
posted to NPP `WineEventQueue` `0x850f8c7c0` hundreds of times. Queue depth grew
to about 48 pending events. `queue_signal` fired repeatedly, including for the
NPP queue (`fd=13` write side), and `onmainthread_resignal_pending` fired 511
times. However NPP only entered `macdrv_ProcessEvents` 25 times and dequeued
only 2 events; after that, mostly `explorer.exe` pid `37741` entered
`ProcessEvents` with its own queue. Therefore the failure is not "pipe never
re-signaled"; the next boundary is `set_queue_fd`/`QS_DRIVER`/`process_driver_events`
ownership and wake/drain routing for the NPP queue.

App-switch freeze boundary from
`reports/phase-h/npp-x64-20260523-220445/stderr.log`: user switched to another
macOS app and Notepad++ froze. Samples:
`reports/phase-h/MILESTONE/event-routing-driver-trace-20260523-220445/pid-40586-switch-freeze-220952.sample.txt`
and `pid-40613-switch-freeze-220958.sample.txt`. NPP pid `40586` was parked in
AppKit `nextEvent` on the main thread and in x64 `NtWaitForMultipleObjects` /
`server_select` on another thread; CPU was 0.0%. NPP queue `0xbeaf90700`
(`read_fd=3`, `write_fd=13`) received `WINDOW_LOST_FOCUS` and `APP_DEACTIVATED`
events, and `queue_signal rc=1` succeeded. After line 11210, NPP thread `tid=24`
did not enter `process_driver_events`; only explorer pid `40613` continued
draining its own queues. Therefore the new boundary is not AppKit event receipt
or pipe write; it is Wine-side wait/message-pump wakeup for the NPP GUI thread
after app deactivation.

Second app-switch repro with wait tracing:
`reports/phase-h/npp-x64-20260523-222805/`. Samples:
`reports/phase-h/MILESTONE/event-routing-wait-trace-20260523-222805/pid-43252-switch-freeze-223254.sample.txt`,
`pid-43275-switch-freeze-223258.sample.txt`, and `pid-43254-switch-freeze-223301.sample.txt`.
NPP queue owner was native thread `6812327` / queue `0x774fa4740`. After line
11732, NPP logged zero `wait_message`, zero `NtUserMsgWait`, zero
`NtWaitForMultipleObjects`, and zero `process_driver_events` on Wine GUI thread
`tid=24`, while AppKit posted 216 events and signaled the queue 236 times. The
sample no longer shows native thread `6812327`; only the AppKit main thread and
unrelated wait threads remain. Next trace must prove whether the GUI thread
returns `WM_QUIT` / runs thread detach, or otherwise exits the message pump while
leaving the Cocoa app and stale event queue alive.

Lifecycle trace repro:
`reports/phase-h/npp-x64-20260523-224712/`. Freeze sample:
`reports/phase-h/MILESTONE/event-routing-lifecycle-trace-20260523-224712/pid-45490-freeze-225148.sample.txt`.
Queue owner was native thread `6825913`, queue `0xac8f9c740`. After line 25489
there are no more Wine-side `wait_message` / `NtUserMsgWait` / `process_driver_events`
entries for GUI thread `tid=24`, while AppKit continues posting/signaling the
queue. `WM_QUIT`, `NtUserPostQuitMessage`, `macdrv_ThreadDetach`, and
`queue_destroy` are absent. The owner thread still runs `OnMainThread`/QUERY_EVENT
until line 25624, then disappears from the macOS sample without normal Wine driver
detach. User cannot continue manual app-switch testing now; freeze is deprioritized
but remains active with this boundary. Next freeze work should trace
`macrunner_hb_BaseThreadInitThunk` / `macrunner_hb_x64_thread_entry` /
`RtlExitUserThread` to identify why the x64 GUI thread leaves the message pump
without unregistering the Cocoa event queue.

Update 2026-05-24 04:58 VLAT: lifecycle trace is now installed in both PE
`ntdll.dll` and Unix `ntdll.so`:
`hb_BaseThreadInitThunk_enter/unix_return/exit_call`,
`hb_x64_thread_entry_begin/return`, and `hb_run_guest_return`. Rebuilt and
installed x86_64/aarch64 PE `ntdll.dll` plus Unix `ntdll.so`; freshness PASS.
Automation attempts did not reproduce the manual freeze:
`reports/phase-h/npp-x64-20260524-044935/` switched Notepad++ away/back through
Finder/Terminal, then sampled pid `90872`; GUI thread was still alive in
`macrunner_hb_x64_thread_entry -> wait_message -> server_select`, and stderr
continued logging `wait_message`/`process_driver_events` through line 15505.
This is NOT closure. Keep freeze open as real-user-only until a fresh manual
freeze supplies the new lifecycle markers.

## Freeze Capture Protocol

Current question: when a real macOS click arrives while the window is frozen,
does it enter `NSApplication sendEvent`, get posted to `WineEventQueue`, wake the
queue/CFRunLoop source, reach `macdrv_ProcessEvents`/`macdrv_handle_event`, and
return from the Wine/server input calls? Instrument the event path and classify
the first missing or non-returning stage. Do not spend cycles on frontmost/focus.

Fresh trace run after killing the stale frozen instance:
`reports/phase-h/npp-x64-20260523-204929/`, live Notepad++ pid `30037`.
Automated 12-cycle sequence (`New -> File -> Edit -> Find`) did not reproduce
the parked freeze: every click reached `ProcessEvents`, `macdrv_handle_event`,
and returned from `NtUserSendHardwareInput`. Leave this trace-enabled fresh
window for manual reproduction; if it freezes, inspect this run's `stderr.log`
from the last click boundary instead of reusing stale-run evidence.

UPDATE3 from swarm: freeze reproduces only from real user clicks, not synthetic
events. Do not use synthetic click loops as repro evidence. Current live
human-in-the-loop run: `reports/phase-h/npp-x64-20260523-212033/`, launched via
detached screen `macrunner-freeze-trace-212033`; Notepad++ pid `35490`, window
`21500`. Trace env is enabled (`MACRUNNER_TRACE_UI_EVENT_PATH=1`,
`MACRUNNER_TRACE_UI_INPUT=1`). User should click/type manually until freeze,
then inspect the tail of this run's `stderr.log` to classify the first missing
stage: NSEvent handler, Wine queue post/signal, queue dequeue/ProcessEvents,
hardware message, or wndproc/message dispatch.

## Closed This Turn

- `wineboot.exe` helper EXE redirect is no longer incomplete: fresh host-exec
  traces show `image_base=0x140000000 machine=0xaa64 x64_guest=0`.
- The remaining bootstrap blocker was the DLL lane inside native ARM64 helpers:
  native `wineboot.exe` was still mapping x64 prefix `gdi32/shell32/user32/
  kernelbase/ntdll/comctl32/win32u`. Fixed in the loader by forcing native helper
  wrong-arch builtin candidates to current-machine `aarch64-windows`, and by
  allowing AMD64 machine mismatch only in the real AMD64 app lane.
- Verify after rebuild: `scripts/verify-build-freshness.sh` PASS; bounded
  `wineboot --init` exited `rc=0` after 2s with zero x64 maps/range registrations.
- Real Notepad++ window appeared. User on-screen screenshot at 2026-05-23 15:25
  shows colored toolbar icons. Window capture
  `reports/visual-regression/notepad-gate-manual-20260523-152238/notepad-window.bmp`
  reports `toolbar_band_colorful=10464`, `colorful=13304`.
- Icon bug is closed in `docs/ENGINE-CHANGE-JOURNAL.md` as ROOT-FIX /
  `verified-onscreen`.
- Secondary-window class-registration bug is closed: `Open` and `Find` dialogs
  create real onscreen windows after registering builtin classes on the
  `get_desktop_window()` early `top_window` return path.
- Open-dialog blank list in the installed/product prefix is closed. Root:
  shell32 filesystem folder init treated a drive PIDL display name as
  `L" (C:)"`, so `CreateFolderEnumList` enumerated a bogus path instead of
  `C:\Program Files\Notepad++\*`. Fix: derive drive paths with `_ILGetDrive` in
  `shfldr_fs.c` before falling back to `SHGetPathFromIDListW`. Verification:
  `scripts/verify-build-freshness.sh` PASS after rebuilding/syncing
  `shell32/comdlg32`; real `MacRunner.app` card `Notepad++ Installed E2E`
  launched `artifacts/installer-e2e/bottles/generic-x86-rosetta-wow64`, and
  on-screen capture
  `reports/installer-e2e/product-open-dialog-after-drive-fix.png` shows the
  Open dialog populated with `autoCompletion`, `plugins`, `langs.model.xml`,
  `notepad++.exe`, etc.

## DISPROVED — Do Not Recheck Without New Evidence

- `__wineboot_event` / service idle was not the root for the latest hang; the
  verified sample pointed at HB signal/range handling, and fresh traces found the
  native-helper wrong-arch DLL lane.
- `wineboot.exe` main image still x64 at `0x140000000` is false in current traces.
- Registered x64 guest range covers `0x140013f84` is false in current traces; old
  x64 ranges were high DLL views.
- `dyld dlopen_from` under `virtual_mutex` remains current is false after the host
  fix.
- `get_startup_info` `env_size` underflow is false for this path.
- Page-sized live-read cache in `hb_memory_read` is false for this path. It made
  `reports/performance/npp-post-live-read-cache-20260524-025717/` fail with
  `EXEC_FAULT pc=0x1400c6b69`; likely stale live memory because Wine/host writes
  bypass `hb_memory_write`. It was reverted and control
  `reports/performance/npp-after-read-cache-revert-20260524-030411/` returned to
  UI-smoke PASS.
- `rc=134` / invalid-free remains current is false after `get_pe_file_info`
  initialization.
- Wine-render/comctl32 toolbar patches are not the fix path; stock Wine render
  baseline is locked.
- The Notepad++ new-tab/menu issue is not a CPU spin or mutex deadlock per
  `sample-npp-hang-newtab-154227.txt`; all sampled threads are parked.
- The Notepad++ new-tab/menu issue is not render/icon: toolbar color is already
  verified on the real screen.
- The Notepad++ menu/Find issue is not general input/message dispatch: manual
  acceptance proved ordinary buttons react and text input works; prior trace
  showed a normal click reaches `WM_LBUTTONDOWN`/`NtUserMessageCall`.
- `winemac.drv` popup/dialog surface presentation is not the first boundary for
  Find: dialog creation failed earlier in user32/win32u class lookup with
  `ERROR_CLASS_DOES_NOT_EXIST`.
- The recurring Notepad++ freeze is not fixed or woken by
  `set frontmost to true`, app switching, or focus-toggle; user verified only
  force-quit recovers. Do not re-run focus-toggle without new counter-evidence.

## Artifacts

- `reports/phase-h/wineboot-x64-range-trace-20260523-150645/`
- `reports/phase-h/wineboot-long-sample-20260523-150819/`
- `reports/phase-h/MILESTONE/ntdll-so-native-helper-dll-lookup-build-20260523-152052.log`
- `reports/phase-h/wineboot-after-native-helper-dll-lookup-20260523-152205/`
- `reports/phase-h/MILESTONE/visual-gate-after-native-helper-dll-lookup-20260523-152237.log`
- `reports/phase-h/npp-x64-20260523-152238/`
- `reports/visual-regression/notepad-gate-manual-20260523-152238/`
- `reports/phase-h/npp-x64-20260523-153552/`
- `reports/phase-h/MILESTONE/sample-npp-hang-newtab-154227.txt`
- `reports/phase-h/npp-x64-20260523-163853/`
- `reports/phase-h/npp-x64-20260523-173253/`
- `reports/phase-h/npp-x64-20260523-174750/`
- `reports/visual-regression/notepad-secondary-verify-20260523-174750/`
- `reports/phase-h/MILESTONE/win32u-builtin-classes-clean-build-20260523-180952.log`
- `reports/phase-h/npp-x64-20260523-181225/`
- `reports/visual-regression/notepad-secondary-verify-20260523-181225/`
- `reports/phase-h/npp-x64-20260523-204929/`
- `reports/phase-h/MILESTONE/event-trace-20260523-204929/`
- `reports/phase-h/npp-x64-20260523-212033/`
- `reports/phase-h/MILESTONE/real-event-freeze-live-20260523-212033/`

## Update 2026-05-29 09:10 (Antigravity, Claude Opus 4.6 Thinking):

**What:** Dirty-tree triage + commit of all pending Codex/Cline/Kimi work (94 files).
No code changes made by this agent. This is a handoff-cleanup session.

**Commits made (4):**
1. `3172f52` — pe32: HyperBridge x86 decoder/lifter/interpreter + WOW64 signal (39 files, 15K+/3.7K-)
2. `aa37d0c` — docs/config: canonical workspace migration + PE32 investigation state
3. `4288c19` — app: control center UI (Cline/Kimi)
4. `51d450a` — docs/scripts/tools: status updates, validation, profiles

**Current blocker (unchanged from Codex 18:59 update):**
PE32 Notepad++ c000001d — process alive but no CG window. `_sigtramp` with
`x13=0xc000001d` (STATUS_ILLEGAL_INSTRUCTION), unknown native target. Codex
identified xtajit trace knobs but didn't get `publish-exception-context`
(guest EIP + instruction bytes) before running out of tokens.

**Next step:** Get `publish-exception-context` via scoped xtajit trace, classify
the instruction, apply family fix per AGENTS protocol.

**ARM64EC research brief:** NOT done. Focus is PE32 c000001d.

## Update 2026-05-29 12:xx (Antigravity Opus + Claude coordinator verify):

**Opus fix landed — commit `6b7886b`:** `win32u: Initialize GdiSharedHandleTable
in PEB32 on 64-bit Unix initialization`.
- Root cause: our single 64-bit `win32u.so` (`_WIN64` always defined) only set
  `peb64->GdiSharedHandleTable`; the `#ifndef _WIN64` upstream branch that would
  set the 32-bit PEB is dead in our unified-win32u design, so 32-bit `gdi32.dll`
  read `peb32->GdiSharedHandleTable == 0` → `c0000005` in `get_gdi_client_ptr`.
- Fix (gdiobj.c:585-595): added `#else` branch — when `NtCurrentTeb()->WowTebOffset`
  set, mirror `gdi_shared` into `peb32->GdiSharedHandleTable`. Verified vs vanilla
  WineHQ + MacRunner baseline: this is a correct symmetric mirror of the existing
  upstream pattern (idiomatic `WowTebOffset`), NOT a hack, NOT masking a HyperBridge
  bug. Bug is genuinely in our Wine OS-layer. CORRECT FIX.

**VERIFIED BY COORDINATOR (clean run, no trace flags):**
- `c0000005` is GONE — loader proceeds far past old crash. Opus fix CONFIRMED.
- BUT: **NO WINDOW.** Opus's "fully launch and execute" claim was overstated — he
  read a 2.2M-line *syscall-trace* log as "launched". It is not.

**NEW BLOCKER (this is the live one):** 100% CPU **hot-spin** (not a syscall block).
- Log freezes at `NtQuerySystemInformation enter class=102` (SystemModuleInformation
  family) with no `leave`. Process pegs 98-100% CPU indefinitely.
- GUI never reached: `0` `NtUser*` / `NtGdi*` / `CreateWindow` calls in log.
- Syscalls reached: NtMapViewOfSection ×128, NtAllocateVirtualMemory ×18,
  NtQuerySystemInformation ×2 (last = class 102, hung).
- Repro: `reports/phase-h/run_window_clean.sh` (clean, WINEDEBUG=-all, no auto-kill).

**Hypotheses for next agent (evidence-first, pick via scoped trace):**
1. WOW64 thunk for QSI class=102 returns malformed 32-bit-layout struct → guest
   loops retrying / iterating bad module list.
2. JIT/interp stuck in a tight loop on an opcode inside the module-enumeration path.
3. QSI class=102 not implemented for WOW64 → returns success with garbage.

**Next step:** scoped xtajit trace around QSI class=102 to capture guest EIP +
instruction bytes at the spin point; classify (WOW64 syscall thunk fix vs opcode
family fix per AGENTS); mirror x64-oracle (how class=102 returns on working x64
path — read-only). DO NOT stop until real Notepad++ x86 window is on screen
(CG-capture), not "process alive".

**Discipline note:** verdict = window pixels, not log length. Confirmed twice now.

## Update 2026-06-09 17:13 (Codex Lane A):

**HK ARM64X callback route fix landed locally:**
- File: `engine/wine/dlls/ntdll/unix/macrunner_hb.c`
- Change: post-normalize callback targets are classified against the metadata-bearing
  ARM64X module; executable ARM64X native targets are called directly instead of
  falling into x64 PE fallback.
- Build/deploy: `dlls/ntdll/ntdll.so` rebuilt from the absolute root and copied to
  `/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so`;
  deployed binary was ad-hoc codesigned.

**One HK validation run (absolute paths, no rerun):**
- Report: `reports/phase4-hollow-knight/laneA-postnormalize-arm64-direct-20260609-170857/`
- Counts: `wine-process-primary=1`, `wine-process-primary-installed=1`,
  `dispatch-fallback=0`, `normalize-trace=80`, `NtUserCreateWindowEx=0`.
- Evidence: normalize maps `ntdll.dll` `0x87fffa170c0` -> `0x87fff9f2a60`, and
  callback dispatch targets `0x87fff9f2a60` without PE fallback.

**Current blocker:** route fallback is fixed, but HK still does not reach
`NtUserCreateWindowEx`. The same run enters a post-direct signal loop:
first signal after normalized callback is `pc=0x0 fault=0x0 x26=0x87fffa170c0`,
then repeated bus signals continue with `x26=0x87fffa170c0`.

- 2026-06-09 23:45 Lane A HK: latest blocker after dispatch loop fix is native ARM64EC `NtGetTickCount` during `update_load_config`; patched PE `user_shared_data` high-address init, pending rebuild/run.

- 2026-06-09 23:56 Lane A HK: USD fault cleared; next visible blocker was `set_security_cookie` fault at `loader.c:5601` (`ldr w8,[x18,#0x40]`, fault `0x40`). Patched ARM64EC seed path; pending rebuild/run.
2026-06-10 00:09 · Lane A HK: latest blocker is ARM64 LdrInitializeThunk NtContinue into low x64 PE entry; patched low-PE HB detector to route strict AMD64 executable entries.
2026-06-10 00:18 · Lane A HK: low-PE route run proved macrunner_hb_amd64_main_on_arm64 stayed false; patched build_main_module to force HB for ARM64 process + AMD64 main.
2026-06-10 00:24 · Lane A HK: after AMD64-main gate run, Ldr X0 stayed in non-executable main image data; added main-AEP fallback in signal_arm64.c.
2026-06-10 00:30 · Lane A HK: primary still times out inside loader_init; added no-thread-entry probe to classify helper/primary Ldr handoff misses.
 · Lane A HK · current blocker: primary installs signal handler but does not emit LdrInitializeThunk after-loader; loader_init phase-probe added to locate stall · next run must check macrunner-hb-loader-phase
2026-06-10 00:52 · Lane A HK · current blocker: primary installs signal handler but does not emit LdrInitializeThunk after-loader; loader_init phase-probe added to locate stall · next run must check macrunner-hb-loader-phase
2026-06-10 01:06 · Lane A HK · run /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/phase4-hollow-knight/laneA-loader-phase-probe-20260610-005502: primary=1 fallback=0 pc0=0 NtUser=0; blocker shifted before LdrInitializeThunk/loader_init, last primary signal pc=0x87fff9cd86c fault=0x3004 · next map PC and patch root
2026-06-10 01:12 · Lane A HK · applying fix for primary pre-Ldr stall: signal-entry trace suppressed while x18=0; previous run last PC mapped to ntdll!__wine_dbg_output ldr [x18,#0x3004] · next run should reach loader/Ldr/NtUser
2026-06-10 01:24 · Lane A HK · run /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/phase4-hollow-knight/laneA-x18-signal-log-guard-20260610-011343: signal-entry recursion fixed (count=0), but primary stops after init_syscall_frame normalized=0x87fff9c7418 before any LdrInitializeThunk/loader phase; next target dispatcher return/frame setup
2026-06-10 01:31 · Lane A HK · next probe targets signal_arm64ec.c:LdrInitializeThunk because primary native target maps there, not signal_arm64.c · expect macrunner-hb-arm64ec-ldr markers
2026-06-10 01:43 · Lane A HK · applying init-frame fallback fix: previous run stopped after init frame and never entered signal_arm64ec Ldr; next run should show macrunner-hb-segv-earlyinit-fallback and then Ldr/loader/HB thread entry
2026-06-10 01:55 · Lane A HK · previous run: pending_x64 set but fallback skipped; x16 dispatcher route stole control. Broadened pending fallback to any depth=0 initial fault · next run should show macrunner-hb-segv-earlyinit-fallback/ldr-ctx-xlat
2026-06-10 02:07 · Lane A HK · previous run: fallback fired but ctx-xlat had Rip/Rsp/Rcx=0 and x16 dispatcher stole route. Fixed by saving original arm ctx and forcing pending route · next run should route x64 Ldr thunk correctly

## 2026-06-10 02:19 Lane A HK loader entry
- Previous pending-context run reached good AMD64 context synthesis but still normalized forced x64 thunk to ARM64 native. Current fix keeps raw x64 thunk PC for HB dispatch; build and one HK run pending.

## 2026-06-10 02:36 Lane A HK preserve raw dispatch
- Current blocker: lower callback dispatch normalized forced raw x64 Ldr thunk `0x87fff9f0500` to ARM64 native `0x87fff9c7418` after signal route. Applied one-shot preserve-raw dispatch flag; build and one HK run pending.

## 2026-06-10 03:01 Lane A HK ARM64EC entry context
- Current fix: raw x64 Ldr entry path now keeps the saved ARM64 init context instead of synthetic AMD64 context, and aligns ARM64EC tagged native targets before direct native calls. Build and one HK run pending.

## 2026-06-10 03:15 Lane A HK callback-run probe
- Current state: raw dispatch and ARM context are preserved, but HK still times out before `macrunner-hb-ldr-after-loader-init`. Added bounded callback-run probe; build and one HK run pending.

## 2026-06-10 03:30 Lane A HK native ARM64EC entry
- Current fix: first x64 entry block only tail-jumped to native Ldr; dispatch now calls the native entry directly with saved ARM64 context/stack. Build and one HK run pending.

## 2026-06-10 03:44 Lane A HK native entry stack
- Current fix: native ARM64EC entry now gets saved ARM context plus allocated signal stack pointer instead of raw top-of-stack. Build and one HK run pending.

## 2026-06-10 03:58 Lane A HK Ldr entry probe
- Current state: native entry helper is called, but no PE Ldr marker appears. Added entry/xlat markers in `signal_arm64ec.c`; build and one HK run pending.

## 2026-06-12 21:34 Lane D HK graphics reach check
- Lane D verified the RPC/services fix in real HK x64 DXMT runs:
  `RPC_S_SERVER_UNAVAILABLE=0`, no epmapper missing fault, services/rpcss start
  under `mr-run`.
- HK still does not reach graphics: `CreateDXGIFactory=0`,
  `D3D11CreateDevice=0`, `UnityWndClass=0`, `GfxDevice=0`.
- Evidence:
  `reports/phase4-hollow-knight/laneA-laneD-rpcss-services-try1-212030/`,
  `reports/phase4-hollow-knight/laneD-rpcss-sample2-212855/`,
  `reports/phase4-hollow-knight/laneD-wait-trace-213215/`.
- Current class: `WAIT_DEADLOCK` at rung `mono-init`; wine sample points through
  `macrunner_hb_try_kernel32_handle_semantic -> NtWaitForSingleObject/server_wait`.
  This is pre-DXGI/HyperBridge ownership, not a DXMT implementation gap.
## 2026-06-12 22:55 Lane D HK x64 DXGI pre-entry root evidence

Lane D verified the previous rpcss/epmapper fix: current HK x64 DXMT runs no longer show RPC_S_SERVER_UNAVAILABLE, but still do not reach CreateDXGIFactory/D3D11CreateDevice/Present.

New evidence points at HyperBridge executing DXGI builtin data as code before public DXGI markers:
- lift-probe hot loop in `DXGI.DLL` at runtime RVAs `0x15cc0`, `0x16040`, `0x16054`, `0x16066`, all disk `.rdata` ASCII strings (`%02x...`, `dxgi_device_GetGPUThreadPriority`, etc.).
- runtime guard probe showed synthetic builtin section metadata reports those RVAs as `.text sec_size=0xbaef6 sec_chars=0x60000020`; disk `dxgi.dll` `.text` is only `0x13bd6`, `.rdata` starts at `0x15000`.
- CFG fast path (`macrunner_hb_try_x64_cfg_dispatch_fast_path`) also trusts `RAX` targets with no executable-target validation, but existing predicates were poisoned by the synthetic `.text` size.

Implemented but pending HK validation due active `x18waittrace2` runner:
- `loader.c` now registers original AMD64 executable PE section ranges before Wine rewrites builtin module headers.
- `macrunner_hb.c` keeps a side-table of original exec ranges and makes `macrunner_hb_pc_in_executable_section()` use that table as authoritative for registered modules.
- Diagnostic probes remain gated by `MACRUNNER_HB_TRACE_EXEC_GUARD` / `MACRUNNER_HB_TRACE_LIFT_PROBE`.
