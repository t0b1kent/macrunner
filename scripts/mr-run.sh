#!/usr/bin/env bash
# MacRunner run wrapper — runs an exe under the spike Wine in a throwaway prefix and
# ALWAYS cleans up (scoped wineserver -k + remove the prefix) on exit, timeout, or Ctrl-C.
# This structurally prevents orphan hot-spinning Wine and leftover 1.5G prefixes.
#
# Usage:
#   scripts/mr-run.sh <dist-dir> <exe-path> [timeout_sec] [-- extra wine args...]
# Output (stdout/stderr of the run) goes to the terminal; redirect yourself if you want a log.
# Keeps NO prefix afterwards: copy any evidence you need out during the run.
set -u
DIST="${1:?need dist dir}"; EXE="${2:?need exe path}"; TMO="${3:-120}"
shift 3 2>/dev/null || shift $#
[ "${1:-}" = "--" ] && shift

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# /tmp rejects prefixes on this host (ownership) — use artifacts scratch.
PREFIX="$(mktemp -d "$ROOT/artifacts/_mr-run.XXXXXX")"
WINE="$DIST/bin/wine"
WSRV="$DIST/bin/wineserver"

cleanup() {
  WINEPREFIX="$PREFIX" "$WSRV" -k >/dev/null 2>&1 || true
  pkill -f "$PREFIX" 2>/dev/null || true
  sleep 1
  rm -rf "$PREFIX" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "[mr-run] prefix=$PREFIX timeout=${TMO}s exe=$EXE" >&2
WINEPREFIX="$PREFIX" WINEDEBUG="${WINEDEBUG:--all}" timeout "$TMO" "$WINE" "$EXE" "$@"
rc=$?
echo "[mr-run] exit=$rc (prefix auto-removed on exit)" >&2
exit $rc
