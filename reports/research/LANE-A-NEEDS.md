# Lane A Needs

## 2026-06-04 - Lane C ntdll rebuild regression blocks Lane A litmus launch

Lane A TSO litmus `mp` passed before the Lane C ntdll changes when running from the
Lane A checkpoint baseline:

- PASS: `reports/phase4-hollow-knight/run-20260604-092108-tso-litmus-mp-envmode`
- Baseline commit: `49fd1fc checkpoint(Lane A): enforce JIT TSO memory ordering`
- Reached `macrunner-hb-ldr-init: phase=after-loader`, then heartbeat and `PASS mp`

After `HEAD=5038f2d`, rebuilding/reinstalling `ntdll.so` from current sources causes
the same clean Lane A baseline to park before `phase=after-loader`:

- FAIL/timeout: `reports/phase4-hollow-knight/run-20260604-101820-tso-litmus-mp-clean-baseline-120`
- Last lines: `phase=before`, `xtajit64 ProcessInit/ThreadInit completed`, then timeout
- Heartbeats: `0`

Current diff since `49fd1fc` in Lane C-owned files is limited to:

- `engine/wine/dlls/ntdll/unix/system.c`
- `engine/wine/dlls/ntdll/unix/virtual.c`

Lane A request for Lane C: validate the `virtual.c` 4GB-boundary ENOMEM-as-occupied
change against the x64 HyperBridge loader path. It currently prevents the litmus exe
from reaching `after-loader` after a current-source ntdll rebuild.
