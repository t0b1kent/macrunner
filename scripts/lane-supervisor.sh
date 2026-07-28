#!/usr/bin/env bash
# Keep a one-line-per-lane liveness snapshot on disk, so a dead lane is visible at a glance
# instead of being discovered an hour later.
#
# Why this exists: on 2026-07-28 two lanes reached GOAL and exited, and the coordinator kept
# working the problem by hand in conversation for an hour without noticing the loops were gone.
# The relaunch guard only fires on iteration-cap exhaustion — GOAL and BLOCKED are terminal by
# design — so nothing reported the transition.
#
# Writes reports/research/LANE-STATUS.md every INTERVAL seconds. Read that file first, before
# claiming anything about what the lanes are doing.
#
# Never `ps -Awwo args | grep` for liveness of anything whose name might appear in this script's
# own command line — that self-match has already reported a busy slot on an empty machine. Here
# we grep for the autoloop script name, which this file does not exec directly.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
OUT="reports/research/LANE-STATUS.md"
INTERVAL="${INTERVAL:-600}"

while :; do
  {
    printf '# Lane status — %s\n\n' "$(date '+%Y-%m-%d %H:%M:%S')"
    printf '| lane | loop | guard | journal lines | journal age | last LOOP-STATUS |\n'
    printf '|---|---|---|---|---|---|\n'
    for P in reports/research/LANE-*-PROGRESS.md; do
      [ -f "$P" ] || continue
      LANE="$(basename "$P" | sed 's/^LANE-//; s/-PROGRESS\.md$//')"
      # pgrep, not `ps | grep`: the substituted pattern lands in the subshell's own command
      # line and `ps -Awwo args | grep -c` then counts ITSELF, reporting every lane alive —
      # including ones dead for a day. pgrep excludes its own process. First version of this
      # file made exactly that mistake, in the function whose comment warns about it.
      LOOP=$(pgrep -f -- "lane-autoloop-failover\.sh .*$(basename "$P")" 2>/dev/null | wc -l | tr -d ' ')
      GUARD=$(pgrep -f -- "lane-relaunch-on-exit\.sh .*$(basename "$P")" 2>/dev/null | wc -l | tr -d ' ')
      LINES=$(wc -l < "$P" 2>/dev/null | tr -d ' ')
      AGE=$(( ($(date +%s) - $(stat -f %m "$P" 2>/dev/null || echo 0)) / 60 ))
      ST=$(grep -aoE '^LOOP-STATUS: [A-Z]+' "$P" 2>/dev/null | tail -1)
      printf '| %s | %s | %s | %s | %s min | %s |\n' \
        "$LANE" "$([ "$LOOP" -gt 0 ] && echo alive || echo DEAD)" \
        "$([ "$GUARD" -gt 0 ] && echo armed || echo none)" \
        "$LINES" "$AGE" "${ST:-—}"
    done
    printf '\nA lane showing DEAD with a terminal LOOP-STATUS finished its objective and needs a\n'
    printf 'new one. A lane showing DEAD with no terminal status died unexpectedly — read\n'
    printf '/tmp/mr-agents/<lane>-autoloop.log.\n'
  } > "$OUT.tmp" && mv "$OUT.tmp" "$OUT"
  sleep "$INTERVAL"
done
