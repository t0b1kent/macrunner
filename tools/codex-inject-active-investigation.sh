#!/bin/sh
# Surfaces the live investigation state into Codex context on SessionStart/PreCompact.
# Paired with docs/ACTIVE-INVESTIGATION.md + AGENTS.md memory-rule #3.
# Output goes to stdout so the Codex hook pipeline can inject it as context.
F=/Volumes/MacOS/MacRunner/docs/ACTIVE-INVESTIGATION.md
if [ -f "$F" ]; then
  echo "=== ACTIVE-INVESTIGATION (read FIRST; section DISPROVED = do NOT re-test) ==="
  cat "$F"
  echo "=== end ACTIVE-INVESTIGATION ==="
fi
exit 0
