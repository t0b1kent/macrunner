#!/usr/bin/env bash
# MacRunner housekeeping — kill orphaned spike Wine trees + clear scratch temp.
# Run after EVERY run/phase so nothing keeps hot-spinning CPU or leaking disk.
# SAFE: scoped to this worktree's arm64ec spike dist. NEVER a global `pkill wine`
# or common TMPDIR `winetemp-*` kill (would kill other Wine work). Idempotent.
#
# Usage:
#   scripts/mr-clean.sh            # kill orphan spike wine + winetemp temp
#   scripts/mr-clean.sh --prune    # also delete throwaway run prefixes + giant logs
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SPIKE_TAG="$ROOT/engine/wine/dist-arm64ec-spike"

echo "[mr-clean] killing orphaned spike Wine (scoped to $SPIKE_TAG)…"
# scoped: only processes whose command references this worktree's spike dist.
pkill -f "$SPIKE_TAG" 2>/dev/null && echo "  killed spike-dist procs" || echo "  no spike-dist procs"
echo "  skipped winetemp process kill: winetemp lives in common TMPDIR and is not worktree-scoped"
sleep 1
# Do not remove common TMPDIR winetemp-* here; other worktrees may have live Wine helpers there.
left="$(pgrep -fc "$SPIKE_TAG" 2>/dev/null || echo 0)"
echo "[mr-clean] remaining spike procs: $left"

if [ "${1:-}" = "--prune" ]; then
  echo "[mr-clean] --prune: deleting throwaway run prefixes (drive_c trees) + giant logs…"
  # delete the bulk (drive_c) of per-run prefixes under artifacts, KEEP small evidence (json/ppm/logs)
  find "$ROOT/artifacts" \( -type d -name 'hk-windows-oracle-prefix-template*' -prune \) -o \
    \( -type d -name 'drive_c' -exec rm -rf {} + \) 2>/dev/null
  # delete giant raw trace logs (>50M); keep small summaries
  find "$ROOT/reports" -type f -name '*.log' -size +50M -delete 2>/dev/null
  echo "[mr-clean] prune done. Disk:"; df -h /System/Volumes/Data 2>/dev/null | tail -1 | awk '{print "  avail="$4" used="$3" ("$5")"}'
fi
echo "[mr-clean] done."
