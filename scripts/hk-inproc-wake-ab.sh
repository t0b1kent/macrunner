#!/bin/bash
# MacRunner 2026-07-29 — A/B for the in-process msync wake.
#
# What is under test: MACRUNNER_MSYNC_INPROC_WAKE. Off, an alertable wait is woken only by the
# wineserver, which costs two trips through a single-threaded process per wait. On, a signaler in
# the same process performs the 1->0 CAS and the ulock_wake itself. Measured baseline that motivated
# it: 36526 waits, avg 11449 us, none timing out — about 416 s of a 476 s cold start spent blocked.
#
# Both arms are identical except that one flag, and both wait for the title slot themselves: this
# machine normally has other lanes launching runs, and an arm that silently does not run leaves a
# number that still looks like a result.
#
# 2026-07-29 (ITER-13) — LAUNCHER FIXED, the first run of this script was VOID.
# It called laneA-run-hk.sh DIRECTLY, which leaves the run contract BLOCKED before wine ever
# starts: reports/phase4-hollow-knight/laneA-inprocwake-off-try1-232647/run-contract.json is
# status=BLOCKED with blockers runner.branch_map.{actxprxy,crt_case_fusion,wwise_observer}
# = "branch_input_absent", plus application.save_snapshot_manifest_sha256 = "path_absent".
# hk-run-try12-config.sh is what exports those branch inputs (its lines 115-116 etc), so it is
# the required launcher — it then delegates to laneA-run-hk.sh itself.
# The failure is silent in the worst way: the arm still writes a run dir, a 226 KB run.log and a
# flight.jsonl, and analyze() below would have reported NOT_REACHED/empty metrics as if measured.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="${1:-inprocwake}"
TMO="${2:-600}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/reports/inproc-wake-ab-$STAMP"
mkdir -p "$OUT"

WINE_SRC="$ROOT/engine/wine"
BUILD="$WINE_SRC/build-arm64ec-spike"
DIST="$WINE_SRC/dist-arm64ec-spike"
NTDLL="$DIST/lib/wine/aarch64-unix/ntdll.so"

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/ab.log"; }

wait_for_slot() {
  local who="$1" w=0
  while [ "$(ps -Ao comm 2>/dev/null | grep -cE 'Hollow Knight\.exe|AbzuGame')" -gt 0 ]; do
    sleep 30; w=$((w+1))
    [ $((w % 10)) -eq 0 ] && log "  [$who] жду слот ($((w/2)) мин)"
    if [ "$w" -ge 240 ]; then log "  [$who] ОТМЕНА: слот занят 2 ч"; return 1; fi
  done
  [ "$w" -gt 0 ] && log "  [$who] слот освободился через $((w/2)) мин"
  sleep 5
  return 0
}

# ---------------------------------------------------------------- build + verify the ARTIFACT
wait_for_slot build || exit 1
log "собираю ntdll"
( cd "$BUILD" && make -j8 >"$OUT/build.log" 2>&1 && make install >>"$OUT/build.log" 2>&1 )
rc=$?
if [ $rc -ne 0 ]; then log "СБОРКА УПАЛА (см. build.log)"; tail -20 "$OUT/build.log" | tee -a "$OUT/ab.log"; exit 1; fi

# Exit code 0 is not evidence that the change shipped: an incremental make whose .o is newer than
# its .c skips the compile, and a skipped install leaves the old .so in place. The format string
# below exists only in the new code, so its presence in the SHIPPED file is the actual proof.
if strings "$NTDLL" 2>/dev/null | grep -q 'ipw_reg='; then
  log "артефакт подтверждён: ipw_reg= найдено в $(basename "$NTDLL") ($(stat -f '%Sm' -t '%H:%M:%S' "$NTDLL"))"
else
  log "СТОП: в поставленном ntdll.so НЕТ нового кода — замерять нечего"
  exit 1
fi

# ---------------------------------------------------------------- arms
analyze() {
  local name="$1" logf="$2" dest="$OUT/$name-metrics.txt"
  : > "$dest"
  if [ ! -f "$logf" ]; then echo "NOT_REACHED=1 (нет $logf)" >> "$dest"; return; fi
  grep -o 'macrunner-hb-syncmeter:.*'      "$logf" | tail -1 >> "$dest"
  grep -o 'macrunner-msync-diag:.*'        "$logf" | tail -1 >> "$dest"
  grep -oE 'ops_total=[0-9]+'              "$logf" | tail -1 >> "$dest"
  grep -oE 'avg_latency_us=[0-9]+'         "$logf" | tail -1 >> "$dest"
  grep -oE 'ipw_reg=[0-9]+ ipw_woke=[0-9]+ ipw_full=[0-9]+' "$logf" | tail -1 >> "$dest"
  grep -oE 'MENU_SECONDS=[0-9.]+'          "$logf" | tail -1 >> "$dest"
  cat "$dest"
}

run_arm() {
  local arm="$1" gate="$2"
  wait_for_slot "$arm" || return 1
  log "=== плечо $arm (MACRUNNER_MSYNC_INPROC_WAKE=$gate) ==="
  env \
    MACRUNNER_MSYNC_INPROC_WAKE="$gate" \
    MACRUNNER_HB_TRANSLATION_CACHE=1 \
    MACRUNNER_HB_TRACE_SYNCMETER=1 \
    "$ROOT/scripts/hk-run-try12-config.sh" "$TAG-$arm" "$TMO" 2 \
    > "$OUT/$arm.stdout" 2>&1
  local rundir
  rundir="$(grep -o 'VALID_RUN=.*' "$OUT/$arm.stdout" | head -1 | cut -d= -f2-)"
  [ -z "$rundir" ] && rundir="$(grep -oE '/[^ ]*laneA-'"$TAG-$arm"'[^ ]*' "$OUT/$arm.stdout" | head -1)"
  log "rundir=$rundir"
  analyze "$arm" "$rundir/run.log" | tee -a "$OUT/ab.log"
  "$ROOT/scripts/mr-clean.sh" >/dev/null 2>&1 || true
}

run_arm off 0
run_arm on  1

log ""
log "================ ИТОГ ================"
for a in off on; do
  log "--- $a ---"
  sed 's/^/    /' "$OUT/$a-metrics.txt" 2>/dev/null | tee -a "$OUT/ab.log"
done
log "полный вывод: $OUT"
