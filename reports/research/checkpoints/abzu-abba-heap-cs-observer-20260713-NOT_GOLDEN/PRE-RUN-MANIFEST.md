# ABZU heap-CS observer — immutable pre-run manifest

- Classification: `NOT_GOLDEN`, diagnostic only; no progress/regression claim.
- Attempt budget: exactly `1`; retry prohibited. Duration: `90s`.
- Question: identify the last unmatched process-heap `ENTER_OK` on TID `0x00a4` and order it against loader-section acquisition/wait on `0x00a8`.
- Independent diagnostic variable: env-gated logging in `ntdll/sync.c`; `RtlEnterCriticalSection` and `RtlLeaveCriticalSection` emit `ENTER_REQ/ENTER_OK/LEAVE_REQ/LEAVE_OK` only for `main process heap section` or `loader_section`, only on TIDs `0x00a4/0x00a8`, with a shared 5000-event budget.
- Canonical source HEAD at build: `0b09f80511f51e0eb8f5d58da642ae0e6a866745`; KEEP reference `e7cca2e3`, no reset/revert/stash/commit.
- Observer source: `engine/wine/dlls/ntdll/sync.c`, SHA-256 `1d3e450f7cf665a3bef58115d1a38d58338e640438d31a7d83e2596a8faeeb5f`.
- User-requested Unix target command completed rc=0 but was a no-op because PE `sync.c` is not linked into `ntdll.so`. Correct ARM64X target then built: `dlls/ntdll/aarch64-windows/ntdll.dll`; build log proves exactly two source compilations (`aarch64-windows/sync.o`, `arm64ec-windows/sync.o`) plus relink, with zero diagnostics.
- Observer ARM64X ntdll SHA-256: `39227983425d1b9d740cf141bdbe1d21bf68ef02c3fcac9d2d769fff7713ca0b`.
- Build log: `reports/build/abzu-heap-cs-observer-ntdll-pe-20260713.log`, SHA-256 `18851e2b95a8e5d2631ac7cdb995a45622df04a2fe2191597f6957357f30bd3a`.
- Execution root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu`; HEAD `f58219bb31dd808c4da6784f8cb4cd1775a88edf`; its dirty source is preserved and not built.
- Frozen runtime: `artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist`.
- Pre-observer Windows ntdll SHA-256: `8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc`; backed up byte-exact and restored on script exit.
- Unix ntdll SHA-256: `14d3563d105defde6efae10563e58b3ac7e88278b2533b96e882c8d6efacf2b5`; unchanged.
- Runner SHA-256: `d2d23492abd564693cab90cb0c49204b6f357f1af3f4244bd6cda6200eec8ee2`.
- Game SHA-256: `b9bdbc743742f45845b152df7663b042e2b5da5d318894584d70f6d63d6839e7`.
- Host: macOS `26.5.2 (25F84)`, arm64, timezone `Asia/Vladivostok (+1000)`, locale `C.UTF-8`; exact child environment captured in `CHILD-ENV.bin`.
- Prefix/runtime: fresh prefix, wineboot skipped, services + actxprxy enabled, DXMT, JIT, WINEMSYNC, requested syscall/callback/memory/cache flags. `MACRUNNER_TRACE_UI_INPUT` is explicitly unset.
- Translation cache is preserved. No `make clean`, global kill, or second title run.
- Serialization fails closed before deployment/attempt consumption if Wine, wineserver, ABZU, mr-run, make, ninja, or cmake build is active.
- Passive host sample: one four-second sample at game age 58s.

