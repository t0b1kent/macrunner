#!/usr/bin/env bash
# Bisect the chaining wedge on QUANTITY of patches, not on kind of edge.
#
# Three axes have already been refuted, and all three cut on WHICH edges are eligible: narrowing
# the terminator set, direct-JMP-only, and forward-only. That last one is the clue this script
# acts on -- it removed 258,640 back edges and dropped avg_chain 5.19 -> 1.37, and the wedge was
# IDENTICAL. Chaining fully off (avg_chain 1.0) does not wedge. So between "wedges" and "does not"
# the difference is barely chain LENGTH at all; what changes is whether patching happens.
#
# Hence: hold the edge rule fixed and vary only how many patches are allowed to land.
#   N=1 wedging   => the culprit is a single patch, and CHAIN_EDGE tracing has already named it.
#   small N boots => the wedge is cumulative (resource/state), and the cap is itself a shippable
#                    subset of chaining.
#
# ── The control arm exists because the first version of this script was worthless ────────────
# It ran the whole ladder without MACRUNNER_HB_BLOCK_CHAIN=1, which is the master gate and
# defaults OFF. Five runs of a feature that was never enabled reported "no wedge at any N", and
# the swapchain times matched the no-chaining baseline exactly -- the result looked like an
# answer and was an artefact. So this version proves the premise BEFORE spending 20 minutes on
# the ladder: chaining must actually engage (avg_chain > 1) and the wedge must actually
# reproduce. If either fails, the ladder is not run at all and the script says why.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SECS="${SECS:-200}"
OUT="$ROOT/reports/research/BISECT-CHAINPATCH-$(date +%Y%m%d-%H%M).txt"
: > "$OUT"
say() { echo "[bisect $(date +%H:%M:%S)] $*" | tee -a "$OUT"; }

# Echoes: "<avg_chain> <wedge_count> <mono> <swapchain> <dir>"
probe() {
  local tag="$1" maxp="$2" d ac wedge mono swap
  while [ "$(ps -Ao comm | grep -cE 'Hollow Knight\.exe|AbzuGame')" -gt 0 ]; do sleep 10; done
  sleep 5
  env MACRUNNER_HB_BLOCK_CHAIN=1 MACRUNNER_HB_CHAIN_PATCH=1 \
      MACRUNNER_HB_CHAIN_MAX_PATCHES="$maxp" MACRUNNER_HB_TRACE_CHAIN_EDGE=1 \
      MACRUNNER_HB_CACHE_RELOC=1 MACRUNNER_HB_TRANSLATION_CACHE=1 \
      ./scripts/hk-run-try12-config.sh "$tag" "$SECS" 1 >/dev/null 2>&1
  d=$(ls -dt reports/phase4-hollow-knight/*"$tag"* 2>/dev/null | head -1)
  # Highest avg_chain any thread reported: a parked thread sitting at 1.0 must not mask the hot one.
  ac=$(grep -o 'avg_chain=[0-9.]*' "$d/run.log" 2>/dev/null | cut -d= -f2 | sort -g | tail -1)
  wedge=$(grep -c 'c000007b' "$d/run.log" 2>/dev/null | tr -d ' \n')
  mono=$(grep -c 'Begin MonoManager' "$d/run.log" 2>/dev/null | tr -d ' \n')
  swap=$(grep -o 'time_to_swapchain=[0-9]*' "$d/triage-summary.txt" 2>/dev/null | head -1 | cut -d= -f2)
  echo "${ac:-0} ${wedge:-0} ${mono:-0} ${swap:-NONE} $(basename "${d:-none}")"
}

say "binary=$(shasum -a256 engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/xtajit64.so | cut -c1-16) secs=$SECS"
say "control: chaining ON, no cap -- does it engage, and does the wedge reproduce?"
read -r AC WEDGE MONO SWAP DIR <<< "$(probe ctl 0)"
say "control avg_chain=$AC wedge=$WEDGE mono=$MONO swapchain=$SWAP dir=$DIR"

if [ "$(echo "$AC > 1.0" | bc -l 2>/dev/null || echo 0)" != "1" ]; then
  say "STOP: avg_chain=$AC — сцепление НЕ включилось. Лестницу не гоняю, она была бы бессмысленной."
  exit 2
fi
if [ "$WEDGE" -eq 0 ]; then
  say "STOP: клин НЕ воспроизвёлся при работающем сцеплении (avg_chain=$AC)."
  say "Это само по себе результат: предпосылка изменилась, бисектить нечего. Нужен новый замер, а не лестница."
  exit 3
fi
say "предпосылка подтверждена: сцепление работает И клин воспроизводится. Гоню лестницу."

for N in 1 8 64 512; do
  read -r AC WEDGE MONO SWAP DIR <<< "$(probe "mp$N" "$N")"
  say "N=$N avg_chain=$AC wedge=$WEDGE mono=$MONO swapchain=$SWAP dir=$DIR"
done
say "---- wedge=0 при avg_chain>1 = этот N выживает; первый N с wedge>0 = порог ----"
