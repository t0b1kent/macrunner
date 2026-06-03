#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"

EXE="${DXMT_X64_BINDING_EXE:-$PROJECT_ROOT/artifacts/phase-h/dxmt-x64-tests/dx11_clear_present_gui_x64.exe}"
TIMEOUT_SECONDS="${DXMT_X64_BINDING_TIMEOUT_SECONDS:-45}"
LOG_DIR="$PROJECT_ROOT/artifacts/dxmt-x64-binding"
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$LOG_DIR/run-$STAMP"
LOG="$RUN_DIR/dxmt-x64-binding.log"

mkdir -p "$RUN_DIR"
"$PROJECT_ROOT/scripts/disk-guard.sh" --check-only >/dev/null

if [[ ! -r "$EXE" ]]; then
  echo "binding_result=FAIL reason=missing_exe exe=$EXE"
  exit 2
fi

set +e
(
  cd "$(dirname "$EXE")" || exit 2
  MACRUNNER_GRAPHICS_BACKEND=dxmt \
  MACRUNNER_PREFIX_SYSTEM32_ARCH=x86_64-windows \
  MACRUNNER_MR_RUN_USE_WARM_PREFIX=1 \
  MACRUNNER_MR_RUN_SKIP_WINEBOOT=1 \
  MACRUNNER_HB_SKIP_WINEBOOT="${MACRUNNER_HB_SKIP_WINEBOOT:-1}" \
  WINEDEBUG="${WINEDEBUG_SMOKE:--all,+loaddll}" \
  WINEDLLOVERRIDES="${DXMT_X64_BINDING_OVERRIDES:-d3d11,dxgi,d3d10core,winemetal=n}" \
  "$PROJECT_ROOT/scripts/mr-run.sh" "$PROJECT_ROOT/engine/wine/dist-arm64ec-spike" "$EXE" "$TIMEOUT_SECONDS"
) >"$LOG" 2>&1
run_rc=$?
set -e

python3 - "$LOG" "$run_rc" <<'PY'
from pathlib import Path
import re
import sys

log = Path(sys.argv[1])
run_rc = int(sys.argv[2])
text = log.read_text(encoding="utf-8", errors="replace")

checks = {
    "winemetal": re.search(r'Loaded L"C:\\\\windows\\\\system32\\\\winemetal\.dll".*: native', text, re.I),
    "dxgi": re.search(r'Loaded L"C:\\\\windows\\\\system32\\\\DXGI\.DLL".*: native', text, re.I),
    "d3d11": re.search(r'Loaded L"C:\\\\windows\\\\system32\\\\d3d11\.dll".*: native', text, re.I),
    "dxmt_sync": "engine/graphics/dist/dxmt/x86_64-windows/d3d11.dll" in text
                 and "engine/graphics/dist/dxmt/x86_64-windows/dxgi.dll" in text
                 and "engine/graphics/dist/dxmt/x86_64-windows/winemetal.dll" in text,
    "unixlib": not re.search(
        r'winemetal_init_unix_call|__wine_init_unix_call|c0000135|LoadLibraryExW\(winemetal\).*gle=(?:126|1114)',
        text,
        re.I,
    ),
}
present_reached = bool(re.search(r'D3D11CreateDevice|CreateDXGIFactory|CreateSwapChain|Present\(', text, re.I))
missing = [name for name, ok in checks.items() if not ok]

print(f"log={log}")
print(f"run_rc={run_rc}")
print(f"dxmt_sync={'PASS' if checks['dxmt_sync'] else 'FAIL'}")
print(f"load_winemetal={'PASS' if checks['winemetal'] else 'FAIL'}")
print(f"load_dxgi={'PASS' if checks['dxgi'] else 'FAIL'}")
print(f"load_d3d11={'PASS' if checks['d3d11'] else 'FAIL'}")
print(f"unixlib_binding={'PASS' if checks['unixlib'] else 'FAIL'}")
print(f"present_reached={'YES' if present_reached else 'NO'}")
if missing:
    print(f"binding_result=FAIL missing={','.join(missing)}")
    raise SystemExit(1)
print("binding_result=PASS")
PY
