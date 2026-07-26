#!/usr/bin/env bash
# MacRunner lane auto-loop — Kimi Code variant. Clone of lane-autoloop2.sh with three deliberate
# differences, each of which matters when this runs CONCURRENTLY with the codex run-lane:
#
#   1. CLI: `kimi -p <prompt> -m <model>` (non-interactive one-shot). NB: `-p` REFUSES to combine with
#      `-y/--yolo` or `--auto` ("Cannot combine --prompt with --yolo"), and it does not need them —
#      smoke-tested 2026-07-26: prompt mode executes shell tools and writes files unprompted. The
#      model must be the config.toml KEY (`kimi-code/k3`), not the display name shown in the banner
#      ("K3"), which errors with `config.invalid: Model "K3" is not configured`. Kimi has no `-C`,
#      so the script cd's to $ROOT itself.
#   2. NO pkill BETWEEN ITERATIONS. lane-autoloop2.sh kills `explorer.exe` / `winetemp-` orphans,
#      which is correct for the loop that OWNS the game-run slot — but fatal here: this loop is an
#      OFFLINE lane running alongside a live game run, and those pkills would shoot the other lane's
#      processes. An offline lane must never touch run processes at all.
#   3. The prompt file is re-read EVERY iteration (lane-autoloop2.sh caches it once before the loop),
#      so a live loop can be steered by editing its prompt file without a restart.
#
# Usage: scripts/lane-autoloop-kimi.sh <lane-name> <progress-file> <max-iters> <prompt-file> [model]
set -uo pipefail

LANE="${1:?lane name}"
PROGRESS="${2:?progress file}"
MAX="${3:-20}"
PROMPTFILE="${4:?prompt file}"
MODEL="${5:-kimi-code/k3}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
. config/env.sh 2>/dev/null
export PATH="$ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin:$PATH"
export PATH="$HOME/.kimi-code/bin:$PATH"

LOG="/tmp/mr-agents/${LANE}-autoloop.log"
mkdir -p /tmp/mr-agents

echo "=== ${LANE} kimi-autoloop start $(date +%H:%M:%S) max=$MAX model=$MODEL ===" >> "$LOG"
for i in $(seq 1 "$MAX"); do
  echo "=== ${LANE} iter $i / $MAX $(date +%H:%M:%S) ===" >> "$LOG"
  # re-read each iteration: editing $PROMPTFILE steers the next turn without restarting the loop
  PROMPT="$(cat "$PROMPTFILE")"
  kimi -p "$PROMPT" -m "$MODEL" >> "$LOG" 2>&1
  sleep 3
  # HARD: only a line that STARTS with "LOOP-STATUS:" counts — instruction/template text can't match.
  STATUS="$(grep -aoE '^LOOP-STATUS: (CONTINUE|GOAL|BLOCKED).*' "$PROGRESS" 2>/dev/null | tail -1)"
  echo "=== ${LANE} iter $i status: ${STATUS:-<none>} ===" >> "$LOG"
  case "$STATUS" in
    *GOAL*)    echo "=== ${LANE} GOAL REACHED at iter $i $(date +%H:%M:%S) ===" >> "$LOG"; exit 0;;
    *BLOCKED*) echo "=== ${LANE} HARD-BLOCKED at iter $i: $STATUS ===" >> "$LOG"; exit 2;;
  esac
done
echo "=== ${LANE} hit max iters $MAX without GOAL/BLOCKED $(date +%H:%M:%S) ===" >> "$LOG"
exit 3
