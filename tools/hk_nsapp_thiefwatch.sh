#!/bin/bash
# LANE-HK-INPUT thief-catcher: attach lldb to the HK game process the instant
# it appears, breakpoint +[NSApplication sharedApplication], log backtrace of
# the FIRST caller (= whoever creates the stock NSApplication before winemac's
# run_cocoa_app can create a WineApplication), then keep running.
# Usage: hk_nsapp_thiefwatch.sh <rundir>
set -u
RUNDIR="${1:?rundir required}"
LOG="$RUNDIR/lldb-nsapp.log"

PID=""
for i in $(seq 1 1500); do
  PID=$(pgrep -f 'Hollow Knight.exe WINEDEBUG' | head -1)
  [ -n "$PID" ] && break
  sleep 0.2
done
[ -n "$PID" ] || { echo "no game pid found" > "$LOG"; exit 1; }

CMD=$(mktemp /tmp/hk_thiefwatch.XXXXXX.lldb)
cat > "$CMD" <<'EOF'
settings set auto-confirm true
breakpoint set --fullname "+[NSApplication sharedApplication]"
breakpoint command add -o "bt 25" -o "process continue" 1
breakpoint set --fullname "+[WineApplication sharedApplication]"
breakpoint command add -o "bt 25" -o "process continue" 2
process continue
EOF

echo "$(date '+%H:%M:%S') attaching to pid $PID" > "$LOG"
/usr/bin/lldb -b -p "$PID" -s "$CMD" >> "$LOG" 2>&1
rm -f "$CMD"
