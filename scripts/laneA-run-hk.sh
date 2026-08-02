#!/usr/bin/env bash
# Lane A HK run wrapper: mr-run + auto-retry on the xtajit64-c0000135 boot flake
# (exit=53, ~150-line log, no forward markers — load_dll race in fresh prefix).
# Usage: laneA-run-hk.sh <tag> <timeout> [max_tries]   (extra env via environment)
#
# Translation cache policy:
# - Warm by default: MACRUNNER_HB_TRANSLATION_CACHE defaults to 1.
# - Set MACRUNNER_HK_COLD_RUN=1 only when cold translation timing is the experiment.
# - If MACRUNNER_HB_TRANSLATION_CACHE_ROOT is not supplied, this wrapper uses a
#   per-ntdll.so root under artifacts/hb-translation-cache/ntdll-<sha16>.  The
#   persistent block key does not include the engine binary hash, so the root is
#   deliberately build-scoped to avoid stale wrong-code after ntdll/codegen rebuilds.
# Evidence policy:
# - Real D3D boundary and DXGI swapchain markers are enabled by default so
#   filtered triage can distinguish real DXGI/D3D calls from IAT binding noise.
# Memory access policy:
# - MACRUNNER_HB_DIRECT_MEM defaults to 1. This is the safe special_read/write
#   direct-copy path, not the gated JIT native-memory lowering flags.
# - Set MACRUNNER_HB_DIRECT_MEM=0 to re-measure the old mach-per-access path.
# Dispatch policy:
# - MACRUNNER_HB_SINGLE_LOOKUP defaults to 1 after the dispatch-rate A/B showed
#   rung-11-preserving speedup to the real swapchain frontier.
# - Set MACRUNNER_HB_SINGLE_LOOKUP=0 to re-measure the old double-lookup path.
set -u
TAG="${1:?need tag}"; TMO="${2:-420}"; MAX="${3:-3}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HK="$ROOT/../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe"
WINE_DIST="${MACRUNNER_LANEA_WINE_DIST:-$ROOT/engine/wine/dist-arm64ec-spike}"
DXMT_DIST="$ROOT/engine/graphics/dist/dxmt"
DXVK_DIST="$ROOT/engine/graphics/dist/dxvk"
MACHINE_DIR="x86_64-windows"
UNIX_DIR="x86_64-unix"

if [ ! -d "$WINE_DIST" ]; then
  echo "[laneA] missing dist: $WINE_DIST" >&2
  exit 2
fi
if [ ! -d "$DXMT_DIST/$MACHINE_DIR" ]; then
  echo "[laneA] missing DXMT machine dir: $DXMT_DIST/$MACHINE_DIR" >&2
  exit 2
fi
if [ ! -f "$DXMT_DIST/$MACHINE_DIR/dxgi.dll" ] || [ ! -f "$DXMT_DIST/$MACHINE_DIR/d3d11.dll" ] || [ ! -f "$DXVK_DIST/$MACHINE_DIR/d3d9.dll" ]; then
  echo "[laneA] missing DXMT runtime DLLs in $DXMT_DIST/$MACHINE_DIR" >&2
  exit 2
fi

timestamp_stream() {
  python3 -c 'import os, sys, time
start = float(os.environ.get("LANEA_RUN_START_EPOCH") or time.time())
for line in sys.stdin:
    now = time.time()
    sys.stdout.write("[laneA-ts epoch=%.3f +%.3fs] %s" % (now, now - start, line))
    sys.stdout.flush()
'
}

# ── PIPE WATCHDOG (MacRunner 2026-07-29, HK master lane iter 7) ────────────────────────────
#
# THE BUG THIS FIXES, measured twice in one afternoon and characterised end to end.
# Line ~218 runs `mr-run.sh ... 2>&1 | timestamp_stream >> run.log`.  mr-run starts wine's
# service processes (MACRUNNER_MR_RUN_START_SERVICES=1); those inherit the pipeline's stderr
# and are reparented to init when the guest dies.  The guest then exits, mr-run.sh exits —
# and `services.exe` is STILL holding the pipe's write end, so the python filter never sees
# EOF, `rc=${PIPESTATUS[0]}` is never reached, and the whole driver chain hangs forever with
# its run long dead.
#
# Measured 2026-07-29 18:5x on laneA-BLACKFRAME-DRAWTRACE-a1: guest 10408 dead, chain
# 97576→56973→57004→93248→93250 wedged 24 min.  `lsof` on the filter's fd 0 gave pipe device
# 0xdd6a4a8f3dd15998, and exactly one process held the matching write end:
#     pid=13371  cmd=services.exe  fd=2   (ppid 1, from this run's winetemp dir)
# `kill -TERM 13371` unwedged the entire chain within 10 s.  The identical shape wedged
# iter 6's HDRPROBE-AB2 arm for 37 min and cost the blackframe run 40 min of the title slot.
#
# WHY THE SCOPE IS SAFE.  It does not guess by name, dist path or winetemp directory — any of
# which could match a sibling lane's live run.  It kills *only* processes that hold the write
# end of THIS pipeline's stderr, which is by construction the set of processes preventing this
# run's teardown, and it does so only after the guest recorded in $RUNDIR/wine-child.pid is
# already dead plus a grace period.  A live sibling cannot be in that set.
# Kill switch: MACRUNNER_LANEA_NO_PIPE_WATCHDOG=1.
_is_descendant_of() {   # $1 = pid, $2 = ancestor pid
  local p="$1" n=0
  while [ -n "$p" ] && [ "$p" != "1" ] && [ "$p" != "0" ] && [ "$n" -lt 12 ]; do
    [ "$p" = "$2" ] && return 0
    p="$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' ')"
    n=$((n+1))
  done
  return 1
}

pipe_watchdog() {   # $1 = rundir
  local rundir="$1" grace="${MACRUNNER_LANEA_PIPE_GRACE:-25}" gp filt dev writers n=0
  [ "${MACRUNNER_LANEA_NO_PIPE_WATCHDOG:-0}" = "1" ] && return 0

  # Wait for the guest to be recorded and then to die.  Bounded so the watchdog can never
  # outlive a run that simply takes a long time.
  while [ "$n" -lt 720 ]; do
    sleep 10; n=$((n+1))
    gp="$(cat "$rundir/wine-child.pid" 2>/dev/null || true)"
    [ -n "$gp" ] || continue
    kill -0 "$gp" 2>/dev/null || break
  done
  [ -n "${gp:-}" ] || return 0
  kill -0 "$gp" 2>/dev/null && return 0    # still alive at the cap: do nothing

  sleep "$grace"                            # let a healthy teardown happen on its own

  # Locate the filter.  It is NOT a direct child of this script: `timestamp_stream` is a shell
  # FUNCTION, so bash forks a subshell for it and python is that subshell's child — measured
  # 57004(script) → 93248(subshell) → 93250(python).  Matching on ppid==$$ finds nothing and
  # would make this watchdog a no-op that merely looks like a fix, so walk the ancestry instead.
  filt=""
  for cand in $(ps -Ao pid,comm | awk '$2 ~ /[Pp]ython/ {print $1}'); do
    if _is_descendant_of "$cand" "$$"; then filt="$cand"; break; fi
  done
  [ -n "$filt" ] || return 0
  dev="$(lsof -p "$filt" -a -d 0 2>/dev/null | awk '$5=="PIPE"{print $6; exit}')"
  [ -n "$dev" ] || return 0

  writers="$(lsof 2>/dev/null | awk -v d="->$dev" '$NF==d {print $2}' | sort -u)"
  [ -n "$writers" ] || return 0
  for w in $writers; do
    [ "$w" = "$filt" ] && continue
    [ "$w" = "$$" ] && continue
    echo "[laneA] pipe-watchdog: guest $gp is dead but pid $w still holds the run pipe" \
         "($(ps -o comm= -p "$w" 2>/dev/null)) — TERM, else this driver hangs forever" \
         >> "$rundir/run.log"
    kill -TERM "$w" 2>/dev/null || true
  done
}

append_time_to_swapchain() {
  python3 - "$1" <<'PY' >> "$1" 2>/dev/null || true
import re
import sys

path = sys.argv[1]
marker = "macrunner-hb-dxgi-swapchain: create method=CreateSwapChainForHwnd"
try:
    text = open(path, "r", encoding="utf-8", errors="ignore").read()
except OSError:
    sys.exit(0)

if re.search(r"\btime_to_swapchain=[0-9]+(?:\.[0-9]+)?s\b", text):
    sys.exit(0)

start = None
m = re.search(r"\[laneA\] run_start_epoch=([0-9]+(?:\.[0-9]+)?)\b", text)
if m:
    start = float(m.group(1))

for line in text.splitlines():
    if marker not in line:
        continue
    if not ("rc=0x0" in line or "rc=(nil)" in line or "rc=0x00000000" in line):
        continue
    rel = re.search(r"\+([0-9]+(?:\.[0-9]+)?)s\]", line)
    if rel:
        print("[laneA] time_to_swapchain=%ds source=laneA-ts marker=CreateSwapChainForHwnd" % round(float(rel.group(1))))
        sys.exit(0)
    epoch = re.search(r"\[laneA-ts epoch=([0-9]+(?:\.[0-9]+)?)\b", line)
    if epoch and start is not None:
        print("[laneA] time_to_swapchain=%ds source=epoch marker=CreateSwapChainForHwnd" % round(float(epoch.group(1)) - start))
        sys.exit(0)

print("[laneA] time_to_swapchain=UNKNOWN source=missing-timestamp marker=CreateSwapChainForHwnd")
PY
}

# Heartbeat tracing produces 100MB+ logs that choke the auto-triage
# (classify_run/analyze_generic_fallback: 94% CPU, 70+ min, killed). Default
# auto-triage OFF when heartbeat is on, unless the caller set it explicitly.
if [ -n "${MACRUNNER_HB_TRACE_HEARTBEAT:-}" ] && [ "${MACRUNNER_HB_TRACE_HEARTBEAT}" != "0" ]; then
  export MACRUNNER_NO_AUTOTRIAGE="${MACRUNNER_NO_AUTOTRIAGE:-1}"
fi

for try in $(seq 1 "$MAX"); do
  # Wait out any other live wine run (parallel lane) to avoid bootstrap races,
  # BUT do not hang on ORPHANED/stale spike wine — a crashed or hard-killed prior
  # run leaves stale .exe / services.exe / wineserver that this loop would
  # otherwise wait the full cap on (~600s false stall — bit us twice 2026-06-18).
  # After ~60s, assume orphan: scoped force-clean (mr-clean.sh = wineserver -k of
  # the spike dist + winetemp + stale services/.exe) + drop throwaway prefixes,
  # then proceed. mr-clean is scoped to dist-arm64ec-spike, so a separate worktree
  # / non-spike wine is untouched.
  # MacRunner 2026-07-29 (HK master lane, iter 3) — DO NOT force-clean a LIVE run.
  #
  # The force-clean below exists for orphans and that purpose is unchanged.  What it could not
  # previously tell apart is an orphan from somebody's legitimately running title.  It fires
  # UPSTREAM of mr-run.sh's atomic slot lock, so that lock cannot prevent it, and two concurrent
  # invocations therefore destroy each other's runs.  Measured today: a pre-registered A/B died
  # at +64.5 s with exit=143 (SIGTERM) while another run was live on the machine.
  #
  # mr-run.sh already records the slot owner's pid in the lock and already defines staleness by
  # PID LIVENESS rather than by age (a real run legitimately lasts 45+ min).  Reuse exactly that
  # definition instead of inventing a second one: a LIVE owner means the wine we can see is a
  # real run -> keep waiting; no owner, or a dead one, means this is the orphan the branch was
  # written for -> clean it exactly as before.
  n=0
  while pgrep -f "$WINE_DIST" >/dev/null 2>&1; do
    sleep 5; n=$((n+1))
    if [ "$n" -ge 12 ]; then
      _slot_owner="$(cat "${TMPDIR:-/tmp}/macrunner-title-slot.lock/pid" 2>/dev/null)"
      if [ -n "$_slot_owner" ] && kill -0 "$_slot_owner" 2>/dev/null; then
        [ $((n % 60)) -eq 0 ] && \
          echo "[laneA] title slot held by LIVE pid $_slot_owner — waiting $((n*5))s, NOT force-cleaning" >&2
        if [ "$n" -ge "${LANEA_SLOT_WAIT_MAX_ITERS:-720}" ]; then
          echo "[laneA] slot held by live pid $_slot_owner for $((n*5))s — giving up rather than killing someone's run" >&2
          exit 75
        fi
        continue
      fi
      echo "[laneA] live spike wine persisted ~60s and NO live slot owner — force-cleaning orphaned spike wine (scoped) and proceeding" >&2
      "$ROOT/scripts/mr-clean.sh" >/dev/null 2>&1 || true
      rm -rf "$ROOT"/artifacts/_mr-run.* 2>/dev/null || true
      break
    fi
  done
  RUNDIR="$ROOT/reports/phase4-hollow-knight/laneA-$TAG-try$try-$(date +%H%M%S)"
  mkdir -p "$RUNDIR"; echo "$RUNDIR" > /tmp/laneA-current-rundir.txt
  OVERLAY_DIR="$RUNDIR/dxmt-builtin-overlay"
  OVERLAY_MACHINE_DIR="$OVERLAY_DIR/$MACHINE_DIR"
  OVERLAY_UNIX_DIR="$OVERLAY_DIR/$UNIX_DIR"
  mkdir -p "$OVERLAY_MACHINE_DIR" "$OVERLAY_UNIX_DIR"
  cp -f "$DXMT_DIST/$MACHINE_DIR/d3d11.dll" "$OVERLAY_MACHINE_DIR/d3d11.dll"
  cp -f "$DXMT_DIST/$MACHINE_DIR/dxgi.dll" "$OVERLAY_MACHINE_DIR/dxgi.dll"
  cp -f "$DXMT_DIST/$MACHINE_DIR/d3d10core.dll" "$OVERLAY_MACHINE_DIR/d3d10core.dll"
  cp -f "$DXMT_DIST/$MACHINE_DIR/winemetal.dll" "$OVERLAY_MACHINE_DIR/winemetal.dll"
  cp -f "$DXVK_DIST/$MACHINE_DIR/d3d9.dll" "$OVERLAY_MACHINE_DIR/d3d9.dll"
  # ARM64EC twins (KEYSTONE): DXMT's d3d11/dxgi/d3d10core/winemetal PEs carry the
  # "Wine builtin DLL" marker, so the loader (load_builtin -> find_builtin_dll,
  # loader.c:2110/2122) resolves them as builtins via the *current-machine*
  # (aarch64-windows) twin. MACRUNNER_DXMT_ROOT=$OVERLAY_DIR is prepended to
  # WINEDLLPATH (mr-run.sh), and find_builtin_dll looks for
  # "$OVERLAY_DIR/aarch64-windows/<name>.dll". Without DXMT's aarch64 twins here it
  # falls back to wine's OWN d3d11/dxgi -> wined3d -> wined3d_create()==NULL on
  # macOS -> DXGI_ERROR_UNSUPPORTED (887a0004). So deploy the aarch64 side too.
  ARM_MACHINE_DIR="aarch64-windows"
  ARM_UNIX_DIR="aarch64-unix"
  OVERLAY_ARM_MACHINE_DIR="$OVERLAY_DIR/$ARM_MACHINE_DIR"
  OVERLAY_ARM_UNIX_DIR="$OVERLAY_DIR/$ARM_UNIX_DIR"
  mkdir -p "$OVERLAY_ARM_MACHINE_DIR" "$OVERLAY_ARM_UNIX_DIR"
  for d in d3d11 dxgi d3d10core winemetal; do
    cp -f "$DXMT_DIST/$ARM_MACHINE_DIR/$d.dll" "$OVERLAY_ARM_MACHINE_DIR/$d.dll"
  done
  cp -f "$DXMT_DIST/$ARM_UNIX_DIR/winemetal.so" "$OVERLAY_ARM_UNIX_DIR/winemetal.so"
  [ -f "$DXVK_DIST/$ARM_MACHINE_DIR/d3d9.dll" ] && \
    cp -f "$DXVK_DIST/$ARM_MACHINE_DIR/d3d9.dll" "$OVERLAY_ARM_MACHINE_DIR/d3d9.dll"

  CACHE_ENABLED="${MACRUNNER_HB_TRANSLATION_CACHE:-1}"
  if [ "${MACRUNNER_HK_COLD_RUN:-0}" = "1" ]; then
    CACHE_ENABLED=0
  fi
  CACHE_ROOT="${MACRUNNER_HB_TRANSLATION_CACHE_ROOT:-}"
  if [ "$CACHE_ENABLED" != "0" ] && [ -z "$CACHE_ROOT" ]; then
    NTDLL_SO="$WINE_DIST/lib/wine/aarch64-unix/ntdll.so"
    if [ -f "$NTDLL_SO" ]; then
      NTDLL_HASH="$(shasum -a 256 "$NTDLL_SO" | awk '{print substr($1,1,16)}')"
      CACHE_ROOT="$ROOT/artifacts/hb-translation-cache/ntdll-$NTDLL_HASH"
      mkdir -p "$CACHE_ROOT"
    else
      echo "[laneA] warning: missing ntdll.so for cache-root hash: $NTDLL_SO" >&2
    fi
  fi
  {
    echo "[laneA] translation_cache=$CACHE_ENABLED"
    [ -n "$CACHE_ROOT" ] && echo "[laneA] translation_cache_root=$CACHE_ROOT"
    [ "${MACRUNNER_HK_COLD_RUN:-0}" = "1" ] && echo "[laneA] cold_run=1"
    echo "[laneA] direct_mem=${MACRUNNER_HB_DIRECT_MEM:-1}"
    echo "[laneA] single_lookup=${MACRUNNER_HB_SINGLE_LOOKUP:-1}"
  } >> "$RUNDIR/run.log"

  RUN_START_EPOCH="$(python3 -c 'import time; print("%.3f" % time.time())')"
  export LANEA_RUN_START_EPOCH="$RUN_START_EPOCH"
  echo "[laneA] run_start_epoch=$RUN_START_EPOCH" >> "$RUNDIR/run.log"

  # Armed BEFORE the pipeline, because once the pipeline blocks there is no later point at
  # which this shell regains control — that is the whole failure mode (see pipe_watchdog).
  pipe_watchdog "$RUNDIR" &
  PIPE_WD_PID=$!

  set +e
  set -o pipefail
    MACRUNNER_RUN_DIR="$RUNDIR" MACRUNNER_HB_TRANSLATION_CACHE="$CACHE_ENABLED" \
    MACRUNNER_HB_TRANSLATION_CACHE_ROOT="$CACHE_ROOT" \
    # ВНИМАНИЕ: список переменных ниже — не документация, а ФИЛЬТР. Всё, чего в нём нет,
    # до процесса игры не доходит вовсе. Прогон с MACRUNNER_DXMT_SWAPCHAIN_TRACE=1,
    # выставленным снаружи, дал ноль строк Present — и это едва не было прочитано как
    # "игра не выводит кадры", хотя в архивном прогоне с проверенно включённым гейтом
    # их 4090, все успешные. Доставку проверять по final-child.json: там фактическое
    # окружение процесса. Умолчание 0, а не пусто: для getenv() пустая строка — это
    # "переменная задана". Комментарии внутри продолжения строки ставить НЕЛЬЗЯ:
    # обратная косая склеивает строки до обработки #, и остаток команды съедается.
    MACRUNNER_HB_TRACE_D3D_BOUNDARY="${MACRUNNER_HB_TRACE_D3D_BOUNDARY:-1}" \
    MACRUNNER_HB_TRACE_DXGI_SWAPCHAIN="${MACRUNNER_HB_TRACE_DXGI_SWAPCHAIN:-1}" \
    MACRUNNER_HB_WP_ADDR="${MACRUNNER_HB_WP_ADDR:-}" \
    MACRUNNER_HB_DIRECT_MEM="${MACRUNNER_HB_DIRECT_MEM:-1}" \
    MACRUNNER_HB_SINGLE_LOOKUP="${MACRUNNER_HB_SINGLE_LOOKUP:-1}" \
    MACRUNNER_GRAPHICS_BACKEND="${MACRUNNER_GRAPHICS_BACKEND:-dxmt}" \
    MACRUNNER_DXMT_SWAPCHAIN_TRACE="${MACRUNNER_DXMT_SWAPCHAIN_TRACE:-0}" \
    MACRUNNER_DXMT_FRAME_DUMP="${MACRUNNER_DXMT_FRAME_DUMP:-0}" \
    MACRUNNER_DXMT_ROOT="$OVERLAY_DIR" \
    MACRUNNER_PREFIX_SYSTEM32_ARCH="$MACHINE_DIR" \
    MACRUNNER_MR_RUN_START_SERVICES="${MACRUNNER_MR_RUN_START_SERVICES:-1}" \
    WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d9=n,b;d3d11,dxgi,d3d10core,winemetal=n,b}" \
    WINEDLLPATH="$OVERLAY_MACHINE_DIR:$OVERLAY_UNIX_DIR:$WINE_DIST/lib/wine/$MACHINE_DIR:$WINE_DIST/lib/wine/$UNIX_DIR" \
    WINESYSTEMDLLPATH="$OVERLAY_MACHINE_DIR" \
    WINEDEBUG="${WINEDEBUG:--all}" "$ROOT/scripts/mr-run.sh" \
    "$WINE_DIST" "$HK" "$TMO" ${HK_EXTRA_ARGS:-} 2>&1 | timestamp_stream >> "$RUNDIR/run.log"
  rc=${PIPESTATUS[0]}
  set +o pipefail
  # The pipeline returned, so the watchdog has nothing left to guard — retire it rather than
  # leaving one background sleeper per attempt.
  kill -TERM "$PIPE_WD_PID" 2>/dev/null || true
  append_time_to_swapchain "$RUNDIR/run.log"
  lines=$(wc -l < "$RUNDIR/run.log")
  echo "try$try rc=$rc lines=$lines $RUNDIR"
  # Оверлей DXMT нужен ТОЛЬКО во время прогона: после него это 68 МБ мёртвого веса, побайтово
  # одинаковых во всех прогонах. За 02.08 накопилось 342 копии = 22 ГБ, съевших почти весь
  # свободный диск. Логи (0.7 ГБ на все прогоны) сохраняются целиком — ценность в них, а сборка
  # DXMT и так лежит в engine/graphics/dist.
  rm -rf "$RUNDIR/dxmt-builtin-overlay" 2>/dev/null || true

  if grep -q 'Mono path' "$RUNDIR/run.log" 2>/dev/null; then
    echo "VALID_RUN=$RUNDIR"; exit 0
  fi
  sleep 5
done
echo "ALL_TRIES_FLAKED"; exit 1
