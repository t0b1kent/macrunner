#!/usr/bin/env bash
set -euo pipefail
ROOT="${MACRUNNER_COMPAT_HOME:-$HOME/.macrunner-compat}"
REPO="$ROOT/winget-pkgs"
URL="https://github.com/microsoft/winget-pkgs.git"
mkdir -p "$ROOT"
if [[ -d "$REPO/.git" ]]; then
  if git -C "$REPO" pull --ff-only >/tmp/macr-winget-pull.log 2>/tmp/macr-winget-pull.err; then
    state="updated"
  else
    state="existing, update unavailable: $(tr '\n' ' ' </tmp/macr-winget-pull.err | sed 's/[[:space:]]\+/ /g' | cut -c1-160)"
  fi
else
  git clone --depth=1 "$URL" "$REPO" >/tmp/macr-winget-clone.log
  state="fresh"
fi
read -r last_date count < <(python3 - "$REPO" <<'PY'
import datetime, pathlib, sys
root = pathlib.Path(sys.argv[1]) / 'manifests'
latest = 0.0
count = 0
for path in root.rglob('*.installer.yaml'):
    count += 1
    try:
        latest = max(latest, path.stat().st_mtime)
    except OSError:
        pass
if latest:
    print(datetime.datetime.fromtimestamp(latest, datetime.UTC).strftime('%Y-%m-%dT%H:%M:%SZ'), count)
else:
    print('unknown', count)
PY
)
echo "$state, last manifest $last_date, installer manifests $count"
