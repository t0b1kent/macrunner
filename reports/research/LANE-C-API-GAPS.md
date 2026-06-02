# Lane C API Gaps

Running ledger for Win32/NT loader, virtual memory, and DLL API gaps found while bringing up real applications.

Lane C owns fixes in `engine/wine/dlls/ntdll/unix/loader.c`, `engine/wine/dlls/ntdll/unix/virtual.c`, and the real DLL implementations under `engine/wine/dlls/**`.

Do not edit `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/wine/dlls/ntdll/unix/signal_arm64.c`, `engine/hyperbridge/**`, or `engine/dxmt/**`. If a required API belongs in the HyperBridge thunk dispatcher, record it here for Lane A/coordinator.

## Open Gaps

- 2026-06-03: WOW64/x86 app probes (`lane-c-npp-x86-20260603-054654.log`, `lane-c-keepass-setup-20260603-054833.log`) both exit `123` immediately after `Wow64LdrpInitialize before cpu_simulate`; `NtMapViewOfSection` returns success-class `STATUS_IMAGE_NOT_AT_BASE (0x40000003)` and there are no `err:`, missing import, unsupported, or crash signatures. This does not point to a Lane C DLL/loader/VM API gap; needs Lane A/coordinator investigation of the x86 CPU/BOP continuation path if x86 bring-up is prioritized.
- 2026-06-02: `engine/wine/include/winnt.h` defines `MEM_RESET_UNDO` as `0x10000000`, but real Win32/llvm-mingw guests use `0x01000000`. Lane C runtime accepts both in `ntdll/unix/virtual.c`; header correction is outside Lane C's owned DLL/loader/virtual file set.

## Closed In Checkpoints

- 2026-06-03: HIGHADJ relocation support in `ntdll/loader.c` and `ntdll/unix/virtual.c`. Both PE relocation paths now handle standard two-slot `IMAGE_REL_BASED_HIGHADJ` fixups and reject truncated HIGHADJ blocks instead of treating the relocation type as unsupported.
- 2026-06-03: `NtQueryVirtualMemory` buffer validation in `ntdll/unix/virtual.c`. Supported query classes now reject null result buffers with `STATUS_ACCESS_VIOLATION` before filling basic, region, working-set, image, or Wine Unix function outputs.
- 2026-06-03: Core VM in/out pointer validation in `ntdll/unix/virtual.c`. `NtAllocateVirtualMemory`, `NtAllocateVirtualMemoryEx`, `NtFreeVirtualMemory`, `NtProtectVirtualMemory`, `NtMapViewOfSection`, `NtMapViewOfSectionEx`, and `NtWow64AllocateVirtualMemory64` now reject null mandatory address/size/protection pointers before trace/APC/local dereferences.
- 2026-06-03: Lock/unlock/flush VM pointer validation in `ntdll/unix/virtual.c`. `NtLockVirtualMemory`, `NtUnlockVirtualMemory`, and `NtFlushVirtualMemory` now reject null address/size pointer arguments with `STATUS_ACCESS_VIOLATION` before dereferencing them or queuing remote APCs.
- 2026-06-03: Forwarded-export string hardening in `ntdll/loader.c`. Forwarder strings are now required to be NUL-terminated inside the export directory before parsing `DLL.Name` / `DLL.#ordinal`, preventing malformed export data from driving unbounded loader string scans.
- 2026-06-03: Unix loader path-format allocation hardening in `ntdll/unix/loader.c`. Startup ntdll path construction now fails explicitly on `asprintf()` failure, and reexec probing skips paths that could not be formatted instead of calling `access()` on null.
- 2026-06-03: Loader environment-buffer allocation hardening in `ntdll/loader.c`. `get_env_var()` now returns `STATUS_NO_MEMORY` if the query buffer allocation fails instead of passing a null buffer to `RtlQueryEnvironmentVariable()`.
- 2026-06-03: System-directory NT name allocation hardening in `ntdll/loader.c`. KnownDll and bootstrap builtin search now return `STATUS_NO_MEMORY` and close opened section mappings if the synthesized system32 NT path cannot be allocated.
- 2026-06-03: `NtSetInformationVirtualMemory` range-array validation in `ntdll/unix/virtual.c`. Supported classes now return `STATUS_ACCESS_VIOLATION` for null `MEMORY_RANGE_ENTRY` arrays when `count > 0`, avoiding Unix-side null dereferences while preserving existing parameter-order status results.
- 2026-06-03: Delay-load failure-hook descriptor fix in `ntdll/loader.c`. `LdrResolveDelayLoadedAPI()` now fills the `DELAYLOAD_INFO` name union with the actual import name for name-described failures instead of storing the low word of the import-name RVA as an ordinal.
- 2026-06-03: Placeholder allocation validation in `ntdll/unix/virtual.c`. `MEM_RESERVE_PLACEHOLDER` creation now rejects contradictory commit/reset/replace combinations before creating a committed or reset placeholder view.
- 2026-06-03: Runtime delay-load no-INT fallback in `ntdll/loader.c`. `LdrResolveDelayLoadedAPI()` now mirrors normal import binding and uses the delay IAT as the import descriptor source when `ImportNameTableRVA` is absent.
- 2026-06-03: `NtAllocateVirtualMemoryEx` address-requirements validation in `ntdll/unix/virtual.c`. Null address-requirement pointers are rejected before dereference, and `HighestEndingAddress + 1` now uses the full page mask alignment check.
- 2026-06-02: Large-page allocation semantics in `ntdll/unix/virtual.c`. `MEM_LARGE_PAGES` is now accepted by the public type masks and returns explicit invalid-parameter vs privilege-not-held results instead of falling through the unknown-bit path.
- 2026-06-02: Write-watch allocation validation in `ntdll/unix/virtual.c`. `MEM_WRITE_WATCH` now requires `MEM_RESERVE` so commit-only calls cannot implicitly create write-watch regions.
- 2026-06-02: Reserved-area split hardening in `ntdll/unix/virtual.c`. Reserved-area removal now reports split bookkeeping allocation failure atomically, with virtual-heap callers restoring the reserved mapping before falling back.
- 2026-06-02: Loader startup/path allocation hardening in `ntdll/unix/loader.c`. DLL path setup, prefix/path helpers, re-exec argv, WOW64 ntdll path, macOS temp-link creation, preloader exec formatting, wineserver PATH probing, and Android JNI argv construction now handle allocation failure instead of dereferencing null buffers.
- 2026-06-02: `MEM_RESET_UNDO` support in `ntdll/unix/virtual.c`. Both the real Win32 bit (`0x01000000`) and the current Wine header bit are accepted, with Darwin `MADV_FREE_REUSE` handling when available.
- 2026-06-02: Working-set query allocation hardening in `ntdll/unix/virtual.c`. `MemoryWorkingSetExInformation` now returns `STATUS_NO_MEMORY` if the temporary reference array allocation fails.
- 2026-06-02: LargeAddressAware registry lookup hardening in `ntdll/unix/virtual.c`. The AppDefaults key lookup now handles failed app-name allocation and closes the root key on all paths.
- 2026-06-02: Relocation protection hardening in `ntdll/loader.c`. PE relocation now checks section `NtProtectVirtualMemory()` failures and restores successfully changed section protections through a common cleanup path.
- 2026-06-02: TLS allocation failure hardening in `ntdll/loader.c`. `alloc_tls_slot()` no longer reports success when per-thread TLS data allocation fails, and it closes the enumerated thread handle on early allocation failures.
- 2026-06-02: IAT protection hardening in `ntdll/loader.c`. Static import binding now fails explicitly if the loader cannot make a read-only import address table writable or restore its original protection.
- 2026-06-02: `MEM_RESET` error propagation in `ntdll/unix/virtual.c`. `NtAllocateVirtualMemory` now returns an NTSTATUS for failed `madvise(MADV_DONTNEED)` instead of silently reporting success.
- 2026-06-02: Delay-import RVA fixup hardening in `ntdll/unix/loader.c`. The builtin PE mapper now mirrors normal import handling and skips the import-name table walk when a delay descriptor has no INT.
- 2026-06-02: Loader hook protection hardening in `ntdll/unix/loader.c`. The Apple x86_64 hook helper now fails loudly if `mprotect()` cannot make the target page writable before patching bytes.
