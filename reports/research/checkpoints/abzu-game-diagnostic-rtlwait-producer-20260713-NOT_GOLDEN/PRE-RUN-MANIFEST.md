# ABZU RtlWaitOnAddress producer observer — immutable pre-run manifest

- Classification: `NOT_GOLDEN`, diagnostic only; no A/B or causal progress claim.
- Attempt budget: exactly `1`; retry prohibited.
- Duration: `90s` (explicitly not 900s).
- Source edits/builds: none. KEEP `e7cca2e3`; no reset/revert/stash/commit.
- Execution root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu`.
- Evidence root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/checkpoints/abzu-game-diagnostic-rtlwait-producer-20260713-NOT_GOLDEN`.
- Frozen runtime: `artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist`.
- Runtime inventory authority: sealed `abzu-game-diagnostic-guest-pc-movement-20260713-NOT_GOLDEN/ARTIFACT-CHECKSUMS.sha256`, SHA-256 `deb4e1fda76632073d4cf32d26c5827e3f982dff3fb3078cc87efee8401adf93`.
- Selected Unix ntdll: `14d3563d105defde6efae10563e58b3ac7e88278b2533b96e882c8d6efacf2b5`.
- Selected Windows ntdll: `8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc`.
- Selected x64 d3d11: `28c0ecc91784212cb76a114e40de09bc0ffe2e95b8771b72f760217ad4d19fc7`.
- Runner: `scripts/mr-run.sh`, SHA-256 `d2d23492abd564693cab90cb0c49204b6f357f1af3f4244bd6cda6200eec8ee2`.
- Game: `AbzuGame-Win64-Shipping.exe`, SHA-256 `b9bdbc743742f45845b152df7663b042e2b5da5d318894584d70f6d63d6839e7`.
- ABZU worktree HEAD: `9f60f074eaf26eb91e33ff06cccf5658338099ea`; dirty tree is preserved byte-for-byte and is not built.
- Host: Apple Silicon macOS; timezone `Asia/Vladivostok`; inherited locale is recorded byte-exact in `CHILD-ENV.bin` before spawn.
- Prefix policy: fresh (`MACRUNNER_MR_RUN_USE_WARM_PREFIX=0`), wineboot skipped, services enabled as in sealed run.
- Graphics/dist/overrides: DXMT; `WINEDLLOVERRIDES=d3d11,dxgi,d3d10core,winemetal=n,b`; `WINEMSYNC=1`; JIT backend.
- Independent diagnostic variable: replace `WINEDEBUG=-all` with `WINEDEBUG=-all,+sync`, enabling the already-built `ntdll` `RtlWaitOnAddress`/`RtlWakeAddress*` TRACE channel. Guest-PC movement and prior wait observers remain disabled.
- Passive host observation: one 4-second macOS sample at game age 58s; no attach mutation.
- Command: `scripts/mr-run.sh "$DIST" "$GAME" 90`, exactly once via `run-once.sh`.
- Pre-run serialization must fail closed if any Wine, wineserver, ABZU, mr-run, or active build process exists.
- Cleanup: runner-owned/scoped only; global `pkill` is forbidden.

Question under test: which user address main waits on, which thread calls the matching wake, and whether the wait precedes an observed D3D11CreateDevice entry.
