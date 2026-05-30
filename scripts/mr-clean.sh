#!/usr/bin/env bash
# MacRunner housekeeping — kill orphaned spike Wine trees + clear scratch temp.
# Run after EVERY run/phase so nothing keeps hot-spinning CPU or leaking disk.
# SAFE: scoped to the arm64ec spike dist + winetemp only. NEVER a global `pkill wine`
# (would kill other Wine work). Idempotent.
#
# Usage:
#   scripts/mr-clean.sh            # kill orphan spike wine + winetemp temp
#   scripts/mr-clean.sh --prune    # also delete throwaway run prefixes + giant logs
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SPIKE_TAG="dist-arm64ec-spike"

echo "[mr-clean] killing orphaned spike Wine (scoped to $SPIKE_TAG + winetemp)…"
# scoped: only processes whose command references the spike dist or a wine temp dir
pkill -f "$SPIKE_TAG" 2>/dev/null && echo "  killed spike-dist procs" || echo "  no spike-dist procs"
pkill -f 'winetemp-' 2>/dev/null && echo "  killed winetemp procs" || echo "  no winetemp procs"
sleep 1
# remove winetemp scratch dirs the killed trees left behind
find "${TMPDIR:-/tmp}" -maxdepth 1 -type d -name 'winetemp-*' -prune -exec rm -rf {} + 2>/dev/null
left="$(pgrep -fc "$SPIKE_TAG" 2>/dev/null || echo 0)"
echo "[mr-clean] remaining spike procs: $left"

if [ "${1:-}" = "--prune" ]; then
  echo "[mr-clean] --prune: deleting throwaway run prefixes (drive_c trees) + giant logs…"
  # delete the bulk (drive_c) of per-run prefixes under artifacts, KEEP small evidence (json/ppm/logs)
  find "$ROOT/artifacts" -type d -name 'drive_c' -prune -exec rm -rf {} + 2>/dev/null
  # delete giant raw trace logs (>50M); keep small summaries
  find "$ROOT/reports" -type f -name '*.log' -size +50M -delete 2>/dev/null
  echo "[mr-clean] prune done. Disk:"; df -h /System/Volumes/Data 2>/dev/null | tail -1 | awk '{print "  avail="$4" used="$3" ("$5")"}'
fi
echo "[mr-clean] done."
