#!/usr/bin/env bash
# MacRunner lane auto-loop with BACKEND FAILOVER: codex -> kimi -> claude(opus).
#
# Why behavioural detection instead of matching an error string: codex/kimi/claude each word
# quota exhaustion differently, the wording changes between releases, and grepping past logs for
# `quota`/`429` here matched only SHA-256 hashes — i.e. a string matcher would both miss real
# exhaustion and fire on hex. What every dead backend DOES have in common is that the turn returns
# almost immediately and the lane journal gains nothing. So:
#
#   an iteration that finishes in < FASTFAIL seconds AND appends no line to the progress file
#   counts as a strike; STRIKES_MAX consecutive strikes retire that backend and promote the next.
#
# A backend that does real work resets its strike counter. When the last backend is retired the
# script writes LOOP-STATUS: BLOCKED into the progress file itself, so the operator sees a named
# reason rather than a silently dead loop.
#
# No pkill anywhere: several lanes run concurrently and one lane's cleanup has already been able to
# shoot another lane's live game run.
#
# Usage: scripts/lane-autoloop-failover.sh <lane> <progress-file> <max-iters> <prompt-file>
set -uo pipefail

LANE="${1:?lane name}"
PROGRESS="${2:?progress file}"
MAX="${3:-20}"
PROMPTFILE="${4:?prompt file}"

FASTFAIL="${FASTFAIL:-180}"     # seconds; a real iteration takes tens of minutes
STRIKES_MAX="${STRIKES_MAX:-2}" # consecutive no-work fast returns before retiring a backend

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
. config/env.sh 2>/dev/null
export PATH="$ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin:$PATH"
export PATH="$HOME/.kimi-code/bin:$PATH"

LOG="/tmp/mr-agents/${LANE}-autoloop.log"
mkdir -p /tmp/mr-agents
touch "$PROGRESS"

# failover chain, in order. Override per lane with e.g. CHAIN="kimi claude codex" — a lane whose
# work suits one backend should start there rather than always leading with codex.
read -r -a BACKENDS <<< "${CHAIN:-codex kimi claude}"
BE_IDX=0
STRIKES=0

run_backend() { # $1 = backend, $2 = prompt
  case "$1" in
    codex)  codex exec "$2" -m "${CODEX_MODEL:-gpt-5.6-terra}" -C "$ROOT" \
              --dangerously-bypass-approvals-and-sandbox >> "$LOG" 2>&1 ;;
    kimi)   kimi -p "$2" -m "${KIMI_MODEL:-kimi-code/k3}" >> "$LOG" 2>&1 ;;
    claude) claude -p "$2" --model "${CLAUDE_MODEL:-opus}" \
              --dangerously-skip-permissions >> "$LOG" 2>&1 ;;
  esac
}

echo "=== ${LANE} failover-autoloop start $(date +%H:%M:%S) max=$MAX chain=${BACKENDS[*]} ===" >> "$LOG"
for i in $(seq 1 "$MAX"); do
  BE="${BACKENDS[$BE_IDX]}"
  PROMPT="$(cat "$PROMPTFILE")"   # re-read each iteration: editing the prompt steers the live loop
  BEFORE="$(wc -l < "$PROGRESS" 2>/dev/null | tr -d ' ')"
  MARK="/tmp/mr-agents/.${LANE}-itermark"; : > "$MARK"
  T0="$(date +%s)"
  echo "=== ${LANE} iter $i / $MAX backend=$BE $(date +%H:%M:%S) ===" >> "$LOG"

  run_backend "$BE" "$PROMPT"

  ELAPSED=$(( $(date +%s) - T0 ))
  AFTER="$(wc -l < "$PROGRESS" 2>/dev/null | tr -d ' ')"
  # A backend that wrote a REPORT but no heartbeat has still worked. Counting only journal lines
  # retired a healthy Kimi on 2026-07-27 after it had already produced the root-cause verdict.
  TOUCHED="$(find reports -type f -newer "$MARK" 2>/dev/null | grep -vc jsonl)"
  sleep 3

  if [ "$ELAPSED" -lt "$FASTFAIL" ] && [ "$AFTER" -le "$BEFORE" ] && [ "$TOUCHED" -eq 0 ]; then
    STRIKES=$(( STRIKES + 1 ))
    echo "=== ${LANE} iter $i: backend=$BE returned in ${ELAPSED}s with no journal line (strike ${STRIKES}/${STRIKES_MAX}) ===" >> "$LOG"
    if [ "$STRIKES" -ge "$STRIKES_MAX" ]; then
      BE_IDX=$(( BE_IDX + 1 )); STRIKES=0
      if [ "$BE_IDX" -ge "${#BACKENDS[@]}" ]; then
        echo "=== ${LANE} ALL BACKENDS RETIRED at iter $i $(date +%H:%M:%S) ===" >> "$LOG"
        printf 'LOOP-STATUS: BLOCKED — every backend in the failover chain (%s) retired after %s consecutive no-work fast returns; quota or auth is exhausted and the operator must refresh it.\n' \
          "${BACKENDS[*]}" "$STRIKES_MAX" >> "$PROGRESS"
        exit 2
      fi
      echo "=== ${LANE} FAILOVER: $BE retired -> ${BACKENDS[$BE_IDX]} $(date +%H:%M:%S) ===" >> "$LOG"
      printf '%s · BACKEND FAILOVER · `%s` retired after %s consecutive returns under %ss with no journal line; continuing on `%s`.\n' \
        "$(date +%H:%M)" "$BE" "$STRIKES_MAX" "$FASTFAIL" "${BACKENDS[$BE_IDX]}" >> "$PROGRESS"
    fi
  else
    STRIKES=0
  fi

  # HARD: only a line that STARTS with "LOOP-STATUS:" counts — template text can never match.
  STATUS="$(grep -aoE '^LOOP-STATUS: (CONTINUE|GOAL|BLOCKED).*' "$PROGRESS" 2>/dev/null | tail -1)"
  echo "=== ${LANE} iter $i status: ${STATUS:-<none>} ===" >> "$LOG"
  case "$STATUS" in
    *GOAL*)    echo "=== ${LANE} GOAL REACHED at iter $i $(date +%H:%M:%S) ===" >> "$LOG"; exit 0;;
    *BLOCKED*) echo "=== ${LANE} HARD-BLOCKED at iter $i: $STATUS ===" >> "$LOG"; exit 2;;
  esac
done
echo "=== ${LANE} hit max iters $MAX without GOAL/BLOCKED $(date +%H:%M:%S) ===" >> "$LOG"
exit 3
