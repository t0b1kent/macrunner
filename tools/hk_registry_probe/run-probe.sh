#!/usr/bin/env bash
# run-probe.sh — run the HK registry probe under the pure-arm64 Wine stack
# against a DISPOSABLE prefix seeded with the Team Cherry section.
#
# Tests:
#   A) seed user.reg BEFORE wineserver starts, then run probe  (normal path)
#   B) boot wineserver with default user.reg, THEN overwrite user.reg on disk
#      while the server stays up, then run probe             (timing hypothesis)
#
# Never touches artifacts/hk-windows-oracle-prefix-template* for writing —
# the template is only READ to extract the seed section.
set -euo pipefail

ROOT="${MACRUNNER_ROOT:-/Users/timurtoby/Documents/MacRunner/Main/MacRunner}"
# The spike dist is the one the HK lane (and thus the game) actually uses and
# the only dist carrying x86_64-windows DLLs. We only RUN it, never modify it.
WINE_DIST="${MACRUNNER_PROBE_WINE_DIST:-$ROOT/engine/wine/dist-arm64ec-spike}"
WINE="$WINE_DIST/bin/wine"
WINESERVER="$WINE_DIST/bin/wineserver"
TEMPLATE_USERREG="$ROOT/artifacts/hk-windows-oracle-prefix-template-20260721-225722-v3/user.reg"
WORK="$ROOT/artifacts/_hk-reg-probe"
PROBE_EXE="$ROOT/tools/hk_registry_probe/hk_registry_probe.exe"
RUN_ID="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/reports/phase4-hollow-knight/registry-probe-$RUN_ID"
MODE="${1:-A}"   # A or B

mkdir -p "$WORK" "$OUT"

# --- safety: refuse to run while an HK title run is live ---
if ps -axo command= | grep -E 'mr-run\.sh|extracted-hollow-knight' | grep -v grep >/dev/null; then
    echo "REFUSE: HK title run is live" >&2
    exit 4
fi

[[ -x "$PROBE_EXE" ]] || { echo "ERROR: probe exe missing: $PROBE_EXE" >&2; exit 2; }
[[ -f "$TEMPLATE_USERREG" ]] || { echo "ERROR: template user.reg missing" >&2; exit 2; }
[[ -x "$WINE" ]] || { echo "ERROR: wine missing: $WINE" >&2; exit 2; }

# Record exact artifact identity of the dist used (verify-what-ran gate).
{
  for f in "$WINE" "$WINESERVER" \
           "$WINE_DIST/lib/wine/x86_64-windows/advapi32.dll" \
           "$WINE_DIST/lib/wine/x86_64-windows/kernelbase.dll" \
           "$WINE_DIST/lib/wine/x86_64-windows/ntdll.dll" \
           "$WINE_DIST/lib/wine/aarch64-unix/ntdll.so"; do
    [ -f "$f" ] && shasum -a 256 "$f"
  done
} > "$OUT/dist-artifacts.sha256" 2>/dev/null
cat "$OUT/dist-artifacts.sha256"

export TMPDIR="${MACRUNNER_WINE_TMPDIR:-/private/tmp/macr-wine-$(id -u)}"
mkdir -p "$TMPDIR"
export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/lib:/usr/local/lib:/usr/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
export WINEDEBUG=-all
export MACRUNNER_HB_X64_LOADER=1

PREFIX="$WORK/prefix-$MODE"
rm -rf "$PREFIX"
mkdir -p "$PREFIX"

seed_user_reg() {
    # Minimal seed: template user.reg already carries the Team Cherry section.
    cp "$TEMPLATE_USERREG" "$PREFIX/user.reg"
    shasum -a 256 "$PREFIX/user.reg" | tee "$OUT/seed-userreg.sha256"
}

stop_server() {
    WINEPREFIX="$PREFIX" "$WINESERVER" -k >/dev/null 2>&1 || true
    sleep 0.5
}

run_probe() {
    local tag="$1"
    ( cd "$WORK" && WINEPREFIX="$PREFIX" "$WINE" "$PROBE_EXE" ) \
        >"$OUT/probe-$tag.stdout" 2>"$OUT/probe-$tag.stderr" || echo "probe-$tag rc=$?" | tee -a "$OUT/rc.log"
    echo "--- probe-$tag stdout ---"
    cat "$OUT/probe-$tag.stdout"
}

if [[ "$MODE" == "A" ]]; then
    # Seed BEFORE any wineserver for this prefix exists.
    seed_user_reg
    run_probe "A-seed-before-server"
elif [[ "$MODE" == "B" ]]; then
    # 1) boot fresh prefix (wineserver loads default/empty user.reg and stays up)
    ( cd "$WORK" && WINEPREFIX="$PREFIX" "$WINE" wineboot --init ) >"$OUT/wineboot.stdout" 2>"$OUT/wineboot.stderr" || true
    WINEPREFIX="$PREFIX" "$WINESERVER" -w >/dev/null 2>&1 || true
    cp "$PREFIX/user.reg" "$OUT/userreg-after-wineboot.reg" 2>/dev/null || true
    # 2) overwrite user.reg on disk WHILE wineserver keeps running
    seed_user_reg
    # 3) run probe against the still-running server
    run_probe "B-seed-after-server"
    stop_server
else
    echo "usage: $0 [A|B]" >&2
    exit 2
fi

stop_server
echo "OUT=$OUT"
