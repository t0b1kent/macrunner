# Lane C API Gaps

Running ledger for Win32/NT loader, virtual memory, and DLL API gaps found while bringing up real applications.

Lane C owns fixes in `engine/wine/dlls/ntdll/unix/loader.c`, `engine/wine/dlls/ntdll/unix/virtual.c`, and the real DLL implementations under `engine/wine/dlls/**`.

Do not edit `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/wine/dlls/ntdll/unix/signal_arm64.c`, `engine/hyperbridge/**`, or `engine/dxmt/**`. If a required API belongs in the HyperBridge thunk dispatcher, record it here for Lane A/coordinator.

## Open Gaps

- None recorded in this Lane C pass yet.

## Closed In Checkpoints

- 2026-06-02: Delay-import RVA fixup hardening in `ntdll/unix/loader.c`. The builtin PE mapper now mirrors normal import handling and skips the import-name table walk when a delay descriptor has no INT.
- 2026-06-02: Loader hook protection hardening in `ntdll/unix/loader.c`. The Apple x86_64 hook helper now fails loudly if `mprotect()` cannot make the target page writable before patching bytes.
