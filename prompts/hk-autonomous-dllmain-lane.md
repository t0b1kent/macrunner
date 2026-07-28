# AUTONOMOUS LANE: winemac.drv loads in Hollow Knight's process and its DllMain never runs

You are running in an auto-loop and will be relaunched in fresh threads until you write
`LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at column 0 of
`reports/research/LANE-HK-DLLMAIN-PROGRESS.md`. Work continuously; do not wait for a human.

Repo root: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Objective

`stage=dllmain_attach` and `stage=macdrv_init_entry` appear for **Hollow Knight's own wine pid**,
and `load_display_driver` reports the real driver instead of `PLACEHOLDER KEPT`. That is the goal.
Not "the module loads", not "the fix looks right" — those have both already been true while input
stayed dead.

## What is measured, and what is only inferred

**Measured, do not re-derive:**

- In HK runs, `stage=dllmain_attach` for `winemac.drv` occurs **exactly once**, as
  `arch=aarch64 pid=005c` — the explorer-class process — where `dllmain_unixcall_init` returns
  `status=00000000`. There is **no** `dllmain_attach` for wine pid **0020**, which is HK. Both
  pids appear in the same log.
- So in HK's process winemac.drv's DllMain **never executes**. `__wine_init_unix_call()` lives
  inside DllMain, so it is not failing — it is never reached, and no unixlib bridge can help.
- The loader nonetheless reports the module LOADED in HK: with `-all,+loaddll`,
  `build_module Loaded L"c:\windows\system32\winemac.drv" at …: builtin`.
- `load_display_driver` then keeps its re-entrancy placeholder for life (`PLACEHOLDER KEPT`),
  so there is no `win_data`, no `WineWindow`, no Cocoa queue, and injected keys measure 0.
- Of 84 module loads on HK's thread, **37 wine builtins load twice** — the x86_64 guest copy plus
  a native aarch64 twin (win32u, user32, gdi32, dxgi, d3d11 …). `winemac.drv` is the **only** wine
  builtin with no twin. The other single-view modules are legitimately single: the game,
  UnityPlayer, Mono, xinput1_3, ntdll, xtajit64.
- The dist **does** ship `aarch64-windows/winemac.drv` (159744 B), `x86_64-windows/winemac.drv`
  (126976 B) and `aarch64-unix/winemac.so`. So the twin exists on disk; it is not being used.

**Refuted at runtime, each behind its own flag — do not retry these:**

- `MACRUNNER_PREFIX_WINEMAC_NATIVE` (force the aarch64 build into system32). The swap demonstrably
  took effect — HK's load address moved — and **nothing else changed**: no `dllmain_attach` for
  0020, still `PLACEHOLDER KEPT`. So it is not about which file sits at the pinned path.
- `MACRUNNER_HB_BUILTIN_UNIXLIB_RESOLVE` — never fires; gated for a different call site.
- Adding `winemac.drv` to the `MemoryWineUnixFuncs` bridge next to `winemetal.dll`
  (`macrunner_hb.c`, coordinator's edit, default-ON, `MACRUNNER_HB_WINEMAC_UNIXLIB_BRIDGE=0` to
  disable). Two UNGATED probes in the shipped ntdll — `macrunner-hb-ntdll-memory-semantic` (once
  per process) and `macrunner-hb-unixfuncs-query` (every query, outside the failure branch) — both
  read **0** in a run on the artifact containing them. The interception is never reached, and the
  bridge is aimed a link too late anyway. **Note the structural reason it was never the right
  shape:** `winemetal.dll` is not in the dist at all — it comes from DXMT's per-run overlay, which
  is why it needs a bridge. `winemac.drv` is an ordinary wine builtin with its own unixlib.

## Where to look

`User32LoadDriver` (`user32/user_main.c:130`) is `LdrLoadDll( L"c:\\windows\\system32", 0, &str,
&module )` — it **pins the search path**, so the display driver never goes through the
builtin-twin resolution every other module reaches. That is a fact about the code, not yet a
proven cause. The question to answer with a measurement is narrow: **in HK's x86_64 guest process,
what does the loader do between "builtin mapped" and "process attach", and which step drops
winemac.drv?** Compare it against a builtin that DOES get its twin and its DllMain in the same
process — win32u and user32 are right there in the same log.

`-all,+loaddll` is the cheap channel and is already wired: `hk-run-try12-config.sh` honours
`MACRUNNER_HK_WINEDEBUG`. Widen deliberately and narrowly — a broad channel both slows an already
throughput-starved boot and buries the log.

## Traps, each of which has cost this project real time

- **A probe inside a failure branch cannot prove absence.** The first `unixfuncs` probe sat inside
  `if (status && …)`, so its zero meant "never ran OR ran and succeeded" — opposite fixes.
- **Verify artifacts by CONTENT as well as SHA.** An "artifact unchanged" guard gave a false abort
  because a sister lane had already built the identical sources; `strings -a <artifact> | grep
  <probe>` is the check that settles it. And `scripts/build-wine-arm64ec-spike.sh` does **not**
  build hyperbridge.
- **The shipping build tree is `engine/wine/build-arm64ec-spike/`.** The in-tree
  `engine/wine/dlls/ntdll/ntdll.so` is a stale decoy that ships nothing.
- **Match process names on `comm`**, never `ps -Awwo args | grep` — the latter matches your own
  command line.
- **`exit=53`** is the known xtajit64 boot flake; retry it.
- **An offline proof that a mechanism is wrong is not a proof that removing it is safe** — the
  kernelbase EC-mirror disable was correct analysis and still killed the guest twice.

## Territory

- **YOURS:** `engine/wine/dlls/user32/**`, `engine/wine/dlls/winemac.drv/**`,
  `engine/wine/dlls/ntdll/loader.c`, `scripts/sync-prefix-from-dist.sh`, `tools/**`, `reports/**`.
- **FORBIDDEN:** `engine/wine/dlls/ntdll/unix/macrunner_hb.c` and `engine/hyperbridge/**` — a
  sister lane is bisecting an ntdll regression there with ~1100 uncommitted lines in flight;
  `engine/dxmt/**`; `engine/wine/dlls/win32u/driver.c` (the e2e lane owns it).
- **Never `pkill`/`killall`** — scope by verified PID. **Never `git add -A`. No commits** — the
  coordinator commits. The e2e lane shares the single JIT title slot: check it on `comm` and wait
  rather than trampling a live run.

## Reporting

One report per iteration under `reports/phase4-hollow-knight/`. Heartbeat line per step to
`reports/research/LANE-HK-DLLMAIN-PROGRESS.md`. State the counts you measured, not the ones you
expected. Mark every unproven statement `[HYPOTHESIS]`.

## Termination

- `LOOP-STATUS: GOAL` — `dllmain_attach` + `macdrv_init_entry` for HK's own wine pid, placeholder
  replaced by the real driver, evidence shown.
- `LOOP-STATUS: BLOCKED` — something only the operator can decide. One sentence.
- Otherwise keep going.
