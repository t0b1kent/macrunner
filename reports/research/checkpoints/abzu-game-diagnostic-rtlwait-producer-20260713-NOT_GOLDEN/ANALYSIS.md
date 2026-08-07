# ABZU producer-wait result

## Verdict

The stall is a real two-thread critical-section deadlock, not a D3D completion wait and not a phantom worker:

`0x00a4 owns main-process-heap section -> waits loader_section owned by 0x00a8`

`0x00a8 owns loader_section -> waits main-process-heap section owned by 0x00a4`

## Byte evidence

- `run.log:1731`: real game main entry, PID 160, guest TID 164 (`0x00a4`).
- `run.log:1826-1831`: `0x00a4` waits on `0x000007FFD08EB288`, exactly `loader_section + 0x18`; the first 5-second wait times out, Wine identifies owner `0x00a8`, then retries for 60 seconds.
- `run.log:1781-1786,1803-1806`: new thread loader-init (`index=1`, then `ThreadInit`) becomes guest TID `0x00a8`; it waits on `0x000007FFD02400E8`, exactly `main process heap section + 0x18`; owner is `0x00a4`, then it retries for 60 seconds.
- The sealed EXE identifies this new thread's requested guest entry as `0x14059D330`, a jump thunk to `0x1405B0950`. That target is a normal event-loop thread procedure (500 ms handle wait, callback/reset path), but `0x00a8` never reaches it: it is blocked inside loader `ThreadInit` first. Therefore its parked state is not the benign event wait of a started worker.
- Neither final wait address has any `RtlWakeAddress*` record. A wake can only occur after the corresponding critical section is released; neither owner can reach release.
- `HOST-SAMPLE.txt`, captured at exact age 58s: exactly two threads remain in `NtWaitForAlertByThreadId -> futex_wait -> __ulock_wait2` for all 3234 samples each. `Thread_32829038` carries the full HB main stack; `Thread_32829670` is the second syscall-waiting thread, correlated with `0x00a8`.
- `run.log:1502`: the only D3D11 record is loader-time `macrunner-hb-iatentry` import resolution. It precedes game main entry by 229 lines. There is no D3D11 probe entry, return, or device-created record.

## Answers

1. `macrunner_hb_run_x64+12104` (the instrumented-build counterpart of sealed `+12028`) is a suspended nested import call-site, not a spinning guest block. The guest operation is `RtlpWaitForCriticalSection(loader_section)`: a 32-bit wait on `loader_section.LockSemaphore` at `+0x18`, first 5s then 60s. It is not an event handle or CreateDevice completion.
2. Obligated producer is present guest TID `0x00a8`. It must release `loader_section`, which would wake `0x00a4`; it cannot because it is parked waiting for the heap critical section owned by `0x00a4`. This is a circular-wait producer failure. Current `0x0068` is a different bootstrap-process main entry and `0x00a0` has no trace records; neither is the obligated producer. IDs are run-local.
3. `D3D11CreateDevice` was not observed called. Execution deadlocks during loader/thread initialization before API entry; therefore zero DXMT/Metal downstream is expected. HB producer classification for D3D11 is never reached.

The remaining source question is which `0x00a4` path retains the process-heap critical section while triggering creation/loader initialization of the `0x14059D330` worker; the runtime causal edge itself is now proven.

## Contract note

One 90-second attempt was consumed; no retry, build, source edit, global kill, or 900-second run occurred. The wrapper's macOS `awk` serialization regex failed syntactically and did not fail closed because the wrapper used `set -u` without `-e`. An independent process check immediately before spawn and the post-run check both found zero Wine/ABZU/build blockers. Evidence remains `NOT_GOLDEN`; this deviation does not affect the in-run lock-owner diagnostics.
