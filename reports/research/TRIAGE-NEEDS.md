# TRIAGE-NEEDS.md — Harness gaps for Lane A (mr-run.sh)

> ✅ RESOLVED 2026-06-07 (coordinator): `mr-run.sh` now routes `flight.jsonl` into the run dir when
> the caller exports `MACRUNNER_RUN_DIR`, AND auto-runs `tools/triage/classify_run.py` on that dir at
> the end of every run (opt-out `MACRUNNER_NO_AUTOTRIAGE=1`, never affects the run's exit code).
> Use: `MACRUNNER_RUN_DIR="$RUNDIR" scripts/mr-run.sh <dist> <exe> <tmo> > "$RUNDIR/run.log" 2>&1`.
> No further Lane A change needed for items below.

## 1. flight.jsonl path mismatch (HIGH — causes every analyzer to run at degraded confidence)

### Finding (2026-06-07, Lane X investigation)

`scripts/mr-run.sh` writes the flight recorder to a **separate, timestamped directory**:

```sh
# mr-run.sh lines 43-52 (abridged):
if [ "${MACRUNNER_FLIGHT_RECORDER:-0}" != "0" ]; then
  FLIGHT_DIR="$ROOT/artifacts/flight/$(date +%Y%m%d-%H%M%S)-$$"
  FLIGHT_PATH="$FLIGHT_DIR/flight.jsonl"
  mkdir -p "$(dirname "$FLIGHT_PATH")"
  export MACRUNNER_FLIGHT_RECORDER_PATH="$FLIGHT_PATH"
  ...
fi
```

The triage analyzers call `find_logs(run_dir)` which recursively walks the **run directory** (e.g.
`run-20260607-HHMMSS-<tag>/`) looking for `flight.jsonl`. Since mr-run.sh writes to
`artifacts/flight/<timestamp>-<pid>/flight.jsonl`, the file is never found.

Consequence: every `stream_events()` call falls back to raw run.log line parsing instead of
structured JSONL events → analyzers output `*_TRACE_INSUFFICIENT` at 0.30 confidence instead of
conclusive BLOCKED/PASS verdicts.

### Required change in mr-run.sh (Lane A owns this file)

When a run directory is created (i.e., `MR_RUN_DIR` / `RUN_DIR` is set), set `FLIGHT_PATH`
**inside** the run directory:

```sh
# Proposed replacement for lines 44-52 of mr-run.sh:
if [ "${MACRUNNER_FLIGHT_RECORDER:-0}" != "0" ]; then
  if [ -n "${MR_RUN_DIR:-}" ]; then
    # Co-locate flight.jsonl with the other run artifacts so triage analyzers find it
    FLIGHT_PATH="${MR_RUN_DIR}/flight.jsonl"
  else
    FLIGHT_DIR="$ROOT/artifacts/flight/$(date +%Y%m%d-%H%M%S)-$$"
    FLIGHT_PATH="$FLIGHT_DIR/flight.jsonl"
  fi
  mkdir -p "$(dirname "$FLIGHT_PATH")"
  export MACRUNNER_FLIGHT_RECORDER_PATH="$FLIGHT_PATH"
  export MACRUNNER_FLIGHT_RECORDER_FILE="$FLIGHT_PATH"
  export MACRUNNER_FLIGHT_PATH="$FLIGHT_PATH"
  echo "[mr-run] flight=$FLIGHT_PATH" >&2
fi
```

Alternative (zero-risk): after the run finishes and before prefix cleanup, copy the flight file:
```sh
if [ -n "${FLIGHT_PATH:-}" ] && [ -f "$FLIGHT_PATH" ] && [ -n "${MR_RUN_DIR:-}" ]; then
  cp "$FLIGHT_PATH" "${MR_RUN_DIR}/flight.jsonl"
fi
```

### What Lane X already fixed (no mr-run.sh edit needed)

- `classify_run.py` now prints a clear WARNING when no flight.jsonl is found, instead of silently
  degrading. Message includes the env var to enable and a pointer to this file.
- All analyzers already degrade gracefully via `find_logs()` + `stream_events()` fallback.

## 2. Flight recorder implementation status

`MACRUNNER_FLIGHT_RECORDER=1` is supported in mr-run.sh (env var gate exists). The flight-recorder
implementation itself is in the engine (HyperBridge / ntdll tracing). Lane X does not own that
source. Status of the underlying recorder is Lane A's purview — once the path is fixed (item 1),
triage will automatically benefit when the recorder is active.
