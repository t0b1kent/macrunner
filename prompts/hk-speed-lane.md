# LANE: make the cold start fast, and do not stop at the first fix

Auto-loop. Relaunched in fresh threads until `LOOP-STATUS: GOAL` or `LOOP-STATUS: BLOCKED` at
column 0 of `reports/research/LANE-HK-SPEED-PROGRESS.md`. Repo:
`/Users/timurtoby/Documents/MacRunner/Main/MacRunner`.

## Standing instruction — this is the whole point of this lane

**Keep going until you hit a real dead end.** Measure, fix, measure, fix. Do not stop to ask
permission for a step the measurement already implies. There are exactly three real dead ends:
a choice only the operator can make (destroying another lane's work, an irreversible action), a
measurement saying there is nothing left to win, or a step needing knowledge you cannot obtain.
Everything else is work you should already be doing.

Report as you go, in the journal, not instead of working. A negative result is a result: record
it and continue.

## Goal

Cold start to the Hollow Knight menu under 120 s (today: 476 s), and warm start under 60 s.
Rosetta does it in under 45 s but has a hardware advantage we cannot take — see below.

## Measured, do not re-derive

Cold boot budget: prefix sync 17 s · wine start 18 s · to Unity init 21 s · **Mono load phase
232 s** · language 124 s · menu scene 61 s.

**Lever 1 — persistent translation cache.** Blocks are re-translated because they cannot be
stored. Rejection attribution (counters already in the tree, printed in
`macrunner-hb-translation-cache-progress` every 5000 compiles):

| | before helper registration | after |
|---|---|---|
| retention | 64 % | 62 % |
| `mh_unkhelper` | 16970 | **0** |
| `mh_widearg` | 7922 | 11759 |
| `mh_toomany` | 2222 | 3202 |
| `mh_argshape` | **0** | **24585** |

Registering the 24 missing helpers (05f3f3f9) eliminated `unkhelper` completely — and retention
did not move, because those blocks now reach the NEXT filter and fail there. `argshape` is now
46 % of all rejections.

**The relocation table is the answer to `argshape` and `toomany` together.** `hb_codegen_buffer_t`
already records every x1/x23 immediate at emission time (d36cf063): 4.9 sites per block measured,
cap raised to 256 after 127 overflows at 64. The store/load paths still SCAN for those sites
instead of reading the table. Switching them removes the need to recognise anything. That is the
next step and it is not optional.

`widearg` (a pointer moved into x2/x3/x4) is the only rejection that describes a real property of
the code rather than a weakness of the matcher. Leave it for last.

**Lever 2 — synchronisation.** A live `sample` of a stalled run: two threads at 538 samples each
in `macrunner_hb_try_kernel32_handle_semantic -> NtWaitForSingleObject -> inproc_wait ->
msync_wait_objs -> __ulock_wait2`, process at 28 % CPU. Not translation, not fixable by any cache.
Untouched so far.

**Lever 3 — memory barriers, and its ceiling.** Codegen emits `dmb ish/ishld/ishst` to emulate
x86 TSO (24 sites in `hb_arm64_codegen.c`). Apple Silicon has a hardware total-store-ordering
mode that removes the need entirely and Rosetta enables it — a privilege of the system
translator that a third-party process cannot take. So barrier COST can be reduced by emitting
only where ordering is observable; it cannot be eliminated. Do not chase parity on a cold start.

## Traps, each paid for

- **Build:** `scripts/build-wine-arm64ec-spike.sh` does NOT build hyperbridge. Run
  `make -C engine/hyperbridge` with `SDKROOT="$(xcrun --show-sdk-path)"` FIRST, then relink.
  Verify by SHA **and** `strings` of what lands in `dist-arm64ec-spike`, never by exit code.
- **Never build while a run is live** — replacing the binary under a running game invalidates its
  numbers. Clear the slot first; check on `comm`, never `ps -Awwo args | grep` (that matches your
  own command line).
- **Telemetry only prints from atexit unless you use the progress line.** A run killed by its
  timeout emits no summary at all — two A/Bs were lost to this before the progress line existed.
  Buffers are 1024 because the overflow branch returns SILENTLY.
- **Register the prediction before the run**, including what would refute you. Today's best
  results came from that; the worst hours came from reading numbers after the fact.
- **Patching emitted machine code fails silently.** The store path self-checks by round-tripping
  and comparing bytes; keep that property in anything you change there.
- `exit=53` is the known xtajit64 boot flake — retry, never call it a regression.

## Territory

Yours: `engine/hyperbridge/**`, `tools/**`, `reports/**`, `scripts/**` for measurement harnesses.
Forbidden: `engine/wine/dlls/ntdll/unix/macrunner_hb.c`, `engine/dxmt/**`. Never `pkill`; scope by
verified PID. Never `git add -A`. No commits — the coordinator commits.

## Termination

- `GOAL` — cold start under 120 s and warm under 60 s, measured twice each.
- `BLOCKED` — one sentence, only for a real dead end as defined above.
- Otherwise keep going.
