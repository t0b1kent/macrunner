#!/usr/bin/env bash
# A/B for MACRUNNER_HB_NO_SNAPSHOT, one binary, arms interleaved.
#
# Why interleaved and not "all of A then all of B": machine load is a first-order confound here.
# The same configuration has been measured at 87 s and at 121 s from load alone, which is larger
# than any effect this gate can plausibly have. Running A A A B B B would hand whichever arm ran
# during the quiet stretch a win it did not earn. Alternating spreads any load drift across both.
#
# Metric is time_to_swapchain, which the triage already extracts, because it is a real milestone
# rather than a counter: every dispatch counter in the engine is __thread, so no single number
# describes whole-process progress, and adding an atomic one would tax the very path being measured.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
REPS="${1:-3}"
SECS="${2:-200}"
OUT="$ROOT/reports/research/AB-NOSNAPSHOT-$(date +%Y%m%d-%H%M).txt"
: > "$OUT"

say() { echo "[ab $(date +%H:%M:%S)] $*" | tee -a "$OUT"; }
say "reps=$REPS secs=$SECS binary=$(shasum -a256 engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/xtajit64.so | cut -c1-16)"

run_arm() {
  local arm="$1" tag="$2" gate="$3"
  # Never start on top of a live game: two runs sharing the machine measure each other's load.
  while [ "$(ps -Ao comm | grep -cE 'Hollow Knight\.exe|AbzuGame')" -gt 0 ]; do sleep 10; done
  sleep 5
  env MACRUNNER_HB_NO_SNAPSHOT="$gate" MACRUNNER_HB_CACHE_RELOC=1 \
      MACRUNNER_HB_TRANSLATION_CACHE=1 \
      ./scripts/hk-run-try12-config.sh "$tag" "$SECS" 1 >/dev/null 2>&1
  local d t
  d=$(ls -dt reports/phase4-hollow-knight/*"$tag"* 2>/dev/null | head -1)
  t=$(grep -o 'time_to_swapchain=[0-9]*' "$d/triage-summary.txt" 2>/dev/null | head -1 | cut -d= -f2)
  local cls
  cls=$(grep -o '^CLASS: .*' "$d/triage-summary.txt" 2>/dev/null | head -1)
  say "$arm swapchain=${t:-NONE}s  ${cls:-CLASS: none}  dir=$(basename "${d:-none}")"
  echo "$arm ${t:-NONE}" >> "$OUT.raw"
}

for i in $(seq 1 "$REPS"); do
  run_arm "ON " "abon$i"  1
  run_arm "OFF" "aboff$i" 0
done

say "---- сводка ----"
for arm in "ON" "OFF"; do
  vals=$(grep "^$arm " "$OUT.raw" 2>/dev/null | awk '{print $2}' | grep -E '^[0-9]+$' | tr '\n' ' ')
  n=$(echo $vals | wc -w | tr -d ' ')
  if [ "$n" -gt 0 ]; then
    avg=$(echo $vals | tr ' ' '\n' | awk '{s+=$1; n++} END {printf "%.1f", s/n}')
    say "$arm n=$n значения: $vals среднее: ${avg}s"
  else
    say "$arm n=0 — ни одного успешного замера"
  fi
done
say "ВАЖНО: разброс от нагрузки ранее измерен 87-121 s. Разница меньше этого — не результат."
