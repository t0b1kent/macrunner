#!/usr/bin/env bash
# lane-autoloop-claude.sh — headless autoloop for a CLAUDE-CLI lane (Fable 5 etc.).
# Why: an interactive GUI Claude.app session ENDS its turn and waits for user input — so an
# autonomous lane stalls every few minutes ("продолжай"). This driver re-invokes the lane
# itself in the SAME terminal, CONTINUING the same conversation each cycle, until the lane
# writes a real "^LOOP-STATUS: GOAL|BLOCKED" line into its progress file (or max iters / tokens).
#
# iter 1   : claude -p "<full resume prompt>"            (fresh thread, reads brief+PROGRESS)
# iter 2..N: claude -p --continue "<short nudge>"        (SAME thread → keeps in-turn context)
#
# Output streams to this terminal (you watch it like any terminal lane). Ctrl-C stops the loop;
# the in-flight claude turn finishes its current tool then the loop exits.
#
# Usage: scripts/lane-autoloop-claude.sh <lane-name> <progress-file> <max-iters> <prompt-file> <model>
#   e.g. scripts/lane-autoloop-claude.sh laneA \
#          reports/research/LANE-A-PROGRESS.md 200 \
#          reports/research/LANE-A-AUTOLOOP-PROMPT.txt claude-fable-5
set -uo pipefail

LANE="${1:?lane name}"
PROGRESS="${2:?progress file}"
MAX="${3:-200}"
PROMPTFILE="${4:?prompt file}"
# Оператор запретил Fable 5 (2026-08-03). Умолчание было claude-fable-5, и лайны молча уезжали на
# него, потому что вызывающий не передавал 5-й аргумент. Умолчание изменено, чтобы запрет нельзя
# было нарушить забывчивостью.
MODEL="${5:-claude-opus-5}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
. config/env.sh 2>/dev/null
export PATH="$ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin:$PATH"

LOG="/tmp/mr-agents/${LANE}-claude-autoloop.log"
mkdir -p /tmp/mr-agents
PROMPT="$(cat "$PROMPTFILE")"
NUDGE="продолжай по плану БЕЗ остановки: проверь статус последнего прогона (ctx + classify_run), сделай следующий forward-шаг по MEGA-BRIEF, в конце захода ОДНОЙ строкой с колонки 0 в LANE-A-PROGRESS.md напиши LOOP-STATUS: CONTINUE|GOAL|BLOCKED <причина>. Прогоны запускай БЛОКИРУЮЩЕ с timeout (ты под драйвером — он перезапустит тебя сам; НЕ оставляй фоновых незавершённых ранов, НЕ жди нотификаций)."

echo "=== ${LANE} CLAUDE-autoloop start $(date '+%F %H:%M:%S') max=$MAX model=$MODEL ===" | tee -a "$LOG"

trap 'echo "=== ${LANE} autoloop interrupted $(date +%H:%M:%S) ===" | tee -a "$LOG"; exit 130' INT

for i in $(seq 1 "$MAX"); do
  echo "" | tee -a "$LOG"
  echo "============ ${LANE} iter $i / $MAX  $(date '+%H:%M:%S') ============" | tee -a "$LOG"

  if [ "$i" -eq 1 ]; then
    # iter 1: CONTINUE the existing Lane A conversation (keeps all its EC-unwind/context) and
    # re-anchor it on the brief. If there is no prior conversation in this cwd, --continue fails →
    # fall back to a fresh thread with the same full prompt.
    claude -p --continue "$PROMPT" --model "$MODEL" --dangerously-skip-permissions 2>&1 | tee -a "$LOG"
    rc=${PIPESTATUS[0]}
    if [ "$rc" -ne 0 ]; then
      echo "=== ${LANE} iter 1: --continue rc=$rc (no prior convo?) → fresh thread ===" | tee -a "$LOG"
      claude -p "$PROMPT" --model "$MODEL" --dangerously-skip-permissions 2>&1 | tee -a "$LOG"
      rc=${PIPESTATUS[0]}
    fi
  else
    # --continue keeps the SAME conversation thread → Lane A retains its context across cycles.
    claude -p --continue "$NUDGE" --model "$MODEL" --dangerously-skip-permissions 2>&1 | tee -a "$LOG"
    rc=${PIPESTATUS[0]}
  fi

  # scoped cleanup only (orphan explorer/winetemp). NEVER global wine pkill (security constraint).
  pkill -9 -f 'explorer.exe' 2>/dev/null
  pkill -9 -f 'winetemp-' 2>/dev/null
  sleep 2

  if [ "$rc" -ne 0 ]; then
    # claude exits non-zero on token/usage limit or hard error → likely "tokens кончились".
    echo "=== ${LANE} iter $i: claude rc=$rc (token/usage limit or error?) — pausing 60s then retry ===" | tee -a "$LOG"
    sleep 60
  fi

  # HARD status gate: only a line STARTING at column 0 with LOOP-STATUS counts (template text can't match).
  # PHILOSOPHY: only GOAL (HK playable) stops the loop. A blocker is NOT a stop — Lane A must SOLVE it with
  # a root fix (max nativeness, no stubs). BLOCKED is logged for visibility but the loop KEEPS GOING so the
  # lane attacks the blocker / another angle next turn. Operator Ctrl-C, token exhaustion or max-iters end it.
  STATUS="$(grep -aoE '^LOOP-STATUS: (CONTINUE|GOAL|BLOCKED).*' "$PROGRESS" 2>/dev/null | tail -1)"
  echo "=== ${LANE} iter $i status: ${STATUS:-<none>} ===" | tee -a "$LOG"
  case "$STATUS" in
    *GOAL*)    echo "=== ${LANE} ✅ GOAL REACHED at iter $i $(date +%H:%M:%S) ===" | tee -a "$LOG"; exit 0;;
    *BLOCKED*) echo "=== ${LANE} ⚠ blocker reported (iter $i) — NOT stopping; lane must root-fix it next turn: $STATUS ===" | tee -a "$LOG";;
  esac
done
echo "=== ${LANE} hit max iters $MAX without GOAL/BLOCKED $(date +%H:%M:%S) ===" | tee -a "$LOG"
exit 3
