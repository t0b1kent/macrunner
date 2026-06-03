# Lane C Phase 3 Probe Evidence - 2026-06-03

Scope: x64 Win32 DLL/API breadth probes via `scripts/mr-run.sh` against `engine/wine/dist-arm64ec-spike`, with fresh throwaway prefixes and scoped runner cleanup.

## Results

| Probe | Result | Evidence |
| --- | --- | --- |
| `common_controls_x64` | PASS | `reports/lane-c-phase3-probes2-20260603-101631/summary.tsv` |
| `audio_device_enum_x64` | PASS | `reports/lane-c-phase3-probes2-20260603-101631/summary.tsv` |
| `xinput_probe_x64` | PASS | `reports/lane-c-phase3-probes2-20260603-101631/summary.tsv` |
| `controller_stub_x64` | PASS | `reports/lane-c-phase3-probes2-20260603-101631/summary.tsv` |
| `com_apartment_x64` | PASS | `reports/lane-c-phase3-probes-20260603-100928/summary.tsv` |
| `winsock_init_x64` | PASS | `reports/lane-c-phase3-probes-20260603-100928/summary.tsv` |
| `shell_known_folder_x64` | FAIL, not Lane C-owned | `reports/lane-c-phase3-shell-known-20260603-101505/` |
| `dll_loadlibrary_x64` | TIMEOUT | `reports/lane-c-phase3-probes-20260603-100928/summary.tsv` |
| `ole_initialize_x64` | TIMEOUT | `reports/lane-c-phase3-probes-20260603-100928/summary.tsv` |

## Classification

`shell_known_folder_x64` calls `SHGetFolderPathA(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, path)` and exits `2` after printing `SHGetFolderPath failed`. The focused rerun enters the native `SHELL32.dll!SHGetFolderPathA` import thunk, then rejects cross-arch callback targets mapped to ARM64 PE `kernel32.dll`, `advapi32.dll`, `shlwapi.dll`, and `ucrtbase.dll`. There is no missing DLL, unsupported opcode, or shell32-local return trace proving a shell32 function-body semantic bug.

Lane C action: record as a Lane A/coordinator callback-routing gap in `LANE-C-API-GAPS.md`; do not patch shell32 speculatively.
