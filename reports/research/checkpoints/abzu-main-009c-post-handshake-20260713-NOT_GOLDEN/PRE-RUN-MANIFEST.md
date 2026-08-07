# ABZU main 0x009c post-handshake probe — immutable pre-run manifest

- Classification: `NOT_GOLDEN`, one diagnostic attempt only; title timeout `120s`.
- Question: after the third successful `FRunnableThreadWin` handshake, which guest block/import/wait/run-exit owns creator/main TID `0x009c`, and why is no real CDF1/D3D11 call reached?
- Independent diagnostic variable: `MACRUNNER_HB_MAIN_009C_PROBE=1`. The preceding event lifecycle observer and all proven Fix A/dynamic-IAT runtime controls remain enabled; disproved idle-event theories are not retested.
- Main HEAD: `4be5ec135492d622b13acc7a22a53738a0776024`; ABZU HEAD: `2b62f6b7ac71f1c64c00ce39e0ff6fb998dc01c8`.
- Main source: `macrunner_hb.c=583ec00f…`, `sync.c=2d17e9a6…`, `thread.c=d9a32873…`; source guards `event=7672cdd8…`, `main=61a10c58…`.
- Coherent deploy pair: Unix ntdll `c27cf0576259dc77c88625ac57cd41c35fb8fcb46f0ea81314e18aa62e2a6254`; ARM64X PE ntdll `a62ea85ae98dd53182476de0677a7a21b94968a931a9dd96737dc4834148ac5f`.
- Runtime originals to restore: Unix `14d3563d…`; PE `8e2a5691…`. Graphics sources, DXMT manifest, runner, game, overlay winemetal, host/locale/timezone, complete dirty branch status, child environment, process serialization snapshot, exact pre/post cache inventories and disk state are captured by `run-once.sh`.
- Observer contract: arm only after main TID `0x009c` returns success from its third wait on handle `0x98`; record its first 1024 blocks, then every 4096th block or consecutive self-loop, capped at 5000 records; imports capped at 1024, waits at 512, and run exit recorded. No state mutation or synthetic signal.
- Fail closed: any retry marker, active Wine/title/build, HEAD/hash drift, incoherent deploy, or restore mismatch invalidates the attempt. Translation cache is inventory-only and must not be deleted.
- Exact launch environment is the baseline from the event-lifecycle observer plus `MACRUNNER_HB_MAIN_009C_PROBE=1`; unsafe wait-stack and UI-input traces remain unset. Full byte-exact environment is written to `CHILD-ENV.bin` before launch.
