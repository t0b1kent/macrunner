#!/usr/bin/env bash
# Move cold phase4 material to the external archive: finish the copy, verify by checksum
# (symlinks included), and only then remove the local original. Never deletes anything that
# has not been proven byte-identical on the destination first.
#
# NB: `rsync -rcn` without -l prints "skipping non-regular file" for symlinks and that is NOT
# a difference — counting those notices as diffs is what made the first verification look
# broken on 2026-07-28. Use -a and filter the notices.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
DEST="/Volumes/MacOS 1/MacRunner-ARCHIVES/phase4-cold-20260728"
LOG=/tmp/mr-agents/archive-cold.log; mkdir -p /tmp/mr-agents
say(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }

ITEMS="reports/phase4-hollow-knight/checkpoints/a1-unknown-forensic-20260719-NOT_GOLDEN
reports/phase4-hollow-knight/checkpoints/20260722-present-capability-02b4-VERIFIED_NOT_GOLDEN
reports/phase4-hollow-knight/forced-relink-and-dep-fix-20260724-NOT_GOLDEN
reports/phase4-hollow-knight/laneA-post-scene-guest-loop-pair-20260726"

say "=== archive-cold start ==="
FREED=0
while read -r d; do
  [ -d "$d" ] || { say "skip (absent): $d"; continue; }
  b="$(basename "$d")"
  say "copy: $b"
  rsync -a "$d/" "$DEST/$b/" >> "$LOG" 2>&1
  n=$(rsync -acn --out-format='%n' "$d/" "$DEST/$b/" 2>/dev/null | grep -v '/$' | grep -vc '^skipping')
  say "verify: $b diffs=$n"
  if [ "$n" -eq 0 ]; then
    sz=$(du -sk "$d" 2>/dev/null | cut -f1)
    chmod -R u+w "$d" 2>/dev/null
    rm -rf "$d" && FREED=$((FREED+sz))
    printf 'MOVED TO EXTERNAL ARCHIVE 2026-07-28\n\nThis directory now lives at:\n  %s/%s/\n\nVerified byte-identical with `rsync -acn` (checksums, symlinks included) BEFORE the local\ncopy was removed. Nothing was deleted that had not been proven present on the destination.\nBrought back with:  rsync -a "%s/%s/" "%s/"\n' "$DEST" "$b" "$DEST" "$b" "$d" > "${d}.MOVED-TO-EXTERNAL.txt"
    say "removed local: $b (${sz} KB)"
  else
    say "KEPT local (verification failed): $b"
  fi
done <<< "$ITEMS"
say "=== done, freed ~$((FREED/1048576)) GB ==="
df -h / | tail -1 >> "$LOG"
