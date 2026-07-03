#!/usr/bin/env bash
# Anti-drift gate: run BEFORE flooring any deploy as a reference binary.
# Catches the fc1a6924 / 89361993 class = a deploy built while an uncommitted WIP
# was applied, then stashed (git-clean tree, stale object, contaminated binary).
# Usage: hkboost-driftcheck.sh [<deployed-ntdll.so>] [wip-token ...]
#   e.g. hkboost-driftcheck.sh engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so region_fusion
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NTDLL="${1:-$ROOT/engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so}"
shift 2>/dev/null || true
TOKENS=("$@"); [ ${#TOKENS[@]} -eq 0 ] && TOKENS=(region_fusion)
fail=0

echo "== 1. git stash list (must be empty or all verified-not-applied):"
if git -C "$ROOT" stash list | grep -q .; then
  git -C "$ROOT" stash list | sed 's/^/   /'
  echo "   WARN: stashes present — verify none is applied to the working tree."; fail=1
else echo "   clean (no stashes)"; fi

echo "== 2. engine/** source git-clean:"
dirty=$(git -C "$ROOT" status --porcelain -- 'engine/**/*.c' 'engine/**/*.h' 2>/dev/null)
if [ -n "$dirty" ]; then echo "$dirty" | sed 's/^/   /'; echo "   FAIL: dirty engine source"; fail=1
else echo "   clean"; fi

echo "== 3. stale objects (source newer than .o → incremental would recompile):"
python3 - "$ROOT" <<'PY'
import os,glob,sys,time
root=sys.argv[1]
stale=0
for bd in glob.glob(root+'/engine/wine/build-arm64ec-spike/dlls/*/*') + [root+'/engine/hyperbridge/src']:
    for o in glob.glob(bd+'/*.o') if os.path.isdir(bd) else []:
        c=o[:-2]+'.c'
        if os.path.exists(c) and os.path.getmtime(c) > os.path.getmtime(o)+1:
            print('   STALE %-40s src +%.0f min' % (os.path.basename(o), (os.path.getmtime(c)-os.path.getmtime(o))/60)); stale+=1
print('   FAIL: %d stale objects → force-clean before deploy' % stale if stale else '   none stale')
PY

echo "== 4. deployed-binary symbol audit for WIP tokens: ${TOKENS[*]}"
if [ -f "$NTDLL" ]; then
  for t in "${TOKENS[@]}"; do
    n=$(nm "$NTDLL" 2>/dev/null | grep -ic "$t" || true)
    if [ "$n" -gt 0 ]; then echo "   FAIL: '$t' x$n present in $(basename "$NTDLL") — WIP contamination"; fail=1
    else echo "   ok: '$t' absent"; fi
  done
else echo "   (no binary at $NTDLL — skip)"; fi

echo "== VERDICT: $([ $fail -eq 0 ] && echo 'CLEAN — safe to floor' || echo 'DRIFT RISK — do NOT floor until resolved')"
exit $fail
