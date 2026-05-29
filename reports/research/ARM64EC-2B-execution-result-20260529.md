# ARM64EC 2B execution result - 2026-05-29

## Build

`scripts/build-wine-arm64ec-spike.sh` now configures the spike with `x86_64`:

```text
47:        --enable-archs=arm64ec,aarch64,i386,x86_64 \
```

`x86_64-windows/ntdll.dll` and `xtajit64` are present in the installed spike:

```text
-rwxr-xr-x  1 timurtoby  staff   214744 May 29 23:32 engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/xtajit64.so
-rw-r--r--  1 timurtoby  staff   983040 May 29 23:32 engine/wine/dist-arm64ec-spike/lib/wine/aarch64-windows/xtajit64.dll
-rw-r--r--  1 timurtoby  staff  3350528 May 29 23:32 engine/wine/dist-arm64ec-spike/lib/wine/x86_64-windows/ntdll.dll
```

Rebuild/install result:

```text
ntdll_rebuild_rc=0
log=/tmp/arm64ec-2b-ntdll-rebuild-5.log
```

## Run

`/tmp/wineprefix-arm64ec-2b` was not usable on this host because Wine refuses to create a prefix under `/tmp` when `/tmp` is not owned by the user. I used a fresh scoped prefix under `artifacts/` and killed only that prefix's `wineserver`.

Final run:

```text
boot_exit=0
run_exit=124
boot_log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/arm64ec-2b-final-boot-20260529-233229.log
run_log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/arm64ec-2b-final-run-20260529-233229.log
prefix=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/wineprefix-arm64ec-2b-final-20260529-233229
```

Furthest final trace point:

```text
macrunner-hb-signal-init: pid=30682 stage=x64-image-map-start trace=0
macrunner-hb-signal-init: pid=30682 stage=x64-image-map-primary-installed rc=0/0/0
macrunner-hb-signal-init: pid=30682 stage=wine-process-start trace=0
macrunner-hb-signal-init: pid=30682 stage=wine-process-primary-installed rc=0/0/0
macrunner-hb-bootstrap-gate: current=aa64 has_env=1 value=L"1" enabled=1 main=8664 image=0000000140000000
macrunner-hb-bootstrap-main-entry-override: old=00000001400013D0 new=00000001400013D0 image=0000000140000000
macrunner-hb-bootstrap-base-thread-thunk-install: old=0000087FFFD90110 bridge=000007FFD07C7BC8
```

## Evidence gathered during the run

Before the loader routing fix, the x64 runner reached `steps=7` but decoded ARM64 bytes from ARM64X native entrypoints:

```text
macrunner-hb-runtime-fail: label=dll block_pc=0x87eff5cb810 ... out=UNSUPPORTED_OPCODE ... blocks=2 steps=7
```

`0x87eff5cb810` is `kernelbase.dll` RVA `0xeb810`, disassembled as ARM64:

```text
00000001800eb810 <__arm64x_native_entrypoint>:
1800eb810: 17ff2243 b 0x1800b411c <DllMain>
```

After adding `kernelbase.dll`, the same class appeared for `kernel32.dll`:

```text
macrunner-hb-runtime-fail: label=dll block_pc=0x87fffd77db2 ... out=MEMORY_FAULT ... blocks=3 steps=7
```

`0x87fffd77db2` is also ARM64X native entry code:

```text
0000000180057db0 <__arm64x_native_entrypoint>:
180057db0: 17ffac72 b 0x180042f78 <DllMain>
```

I therefore routed `kernel32.dll` and `kernelbase.dll` through the existing native-entry allowlist instead of treating those bytes as x64 opcode gaps.

## Current state

The `c0000135` x86_64-ntdll blocker is fixed, and the run now reaches the x64 loader bootstrap with the base-thread bridge installed. `xtajit64` is built and installed, but the final run does not yet emit `ProcessInit`, `ThreadInit`, or `BeginSimulation`; the next root fix is to enter `xtajit64` through the correct ARM64EC/export boundary for the aa64 x64-loader path, not by directly linking ARM64EC-only init symbols from native aa64 ntdll.
