#!/usr/bin/env bash
# Lane A HK run wrapper: mr-run + auto-retry on the xtajit64-c0000135 boot flake
# (exit=53, ~150-line log, no forward markers — load_dll race in fresh prefix).
# Usage: laneA-run-hk.sh <tag> <timeout> [max_tries]   (extra env via environment)
set -u
TAG="${1:?need tag}"; TMO="${2:-420}"; MAX="${3:-3}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HK="$ROOT/../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe"
WINE_DIST="$ROOT/engine/wine/dist-arm64ec-spike"
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
  n=0
  while ps ax -o command | grep -E '\.exe|mr-run\.sh' | grep -vE 'grep|winedevice|explorer.exe /desktop|svchost|plugplay' | grep -q .; do
    sleep 5; n=$((n+1))
    if [ "$n" -ge 12 ]; then
      echo "[laneA] live .exe/mr-run persisted ~60s — force-cleaning orphaned spike wine (scoped) and proceeding" >&2
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
  MACRUNNER_RUN_DIR="$RUNDIR" MACRUNNER_HB_TRANSLATION_CACHE="${MACRUNNER_HB_TRANSLATION_CACHE:-0}" \
    MACRUNNER_GRAPHICS_BACKEND="${MACRUNNER_GRAPHICS_BACKEND:-dxmt}" \
    MACRUNNER_DXMT_ROOT="$OVERLAY_DIR" \
    MACRUNNER_PREFIX_SYSTEM32_ARCH="$MACHINE_DIR" \
    MACRUNNER_MR_RUN_START_SERVICES="${MACRUNNER_MR_RUN_START_SERVICES:-1}" \
    WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d9=n,b;d3d11,dxgi,d3d10core,winemetal=n,b}" \
    WINEDLLPATH="$OVERLAY_MACHINE_DIR:$OVERLAY_UNIX_DIR:$WINE_DIST/lib/wine/$MACHINE_DIR:$WINE_DIST/lib/wine/$UNIX_DIR" \
    WINESYSTEMDLLPATH="$OVERLAY_MACHINE_DIR" \
    WINEDEBUG="${WINEDEBUG:--all}" "$ROOT/scripts/mr-run.sh" \
    "$WINE_DIST" "$HK" "$TMO" ${HK_EXTRA_ARGS:-} > "$RUNDIR/run.log" 2>&1
  rc=$?
  lines=$(wc -l < "$RUNDIR/run.log")
  echo "try$try rc=$rc lines=$lines $RUNDIR"
  if grep -q 'Mono path' "$RUNDIR/run.log" 2>/dev/null; then
    echo "VALID_RUN=$RUNDIR"; exit 0
  fi
  sleep 5
done
echo "ALL_TRIES_FLAKED"; exit 1
