#!/bin/bash
# Clean window-verification run (NO heavy trace flags) — after PEB32 GdiSharedHandleTable fix
ROOT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner"
export WINEPREFIX="$ROOT/bottles/generic-x86"
export MACRUNNER_HB_X64_LOADER=1
export MACRUNNER_WINE_DIST="$ROOT/engine/wine/dist-pure-arm64"
export WINELOADER="$ROOT/engine/wine/dist-pure-arm64/bin/wine"
export WINESERVER="$ROOT/engine/wine/dist-pure-arm64/bin/wineserver"
export WINEDLLPATH="$ROOT/engine/wine/dist-pure-arm64/lib/wine"
export MACRUNNER_HB_SKIP_WINEBOOT=1
export WINE_MONO_NO_INSTALL=1
export WINEDEBUG=-all

cd "$ROOT/artifacts/phase-h/npp-x86"
exec "$ROOT/engine/wine/dist-pure-arm64/bin/wine" ./notepad++.exe -noPlugin -nosession
