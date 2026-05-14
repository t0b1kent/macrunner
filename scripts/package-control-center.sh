#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_SRC="$ROOT/app/macr-control-center"
VERSION="${MACRUNNER_VERSION:-0.3.0}"
DIST="$ROOT/dist"
STAGE="$DIST/stage"
APP="$STAGE/MacRunner.app"
RES="$APP/Contents/Resources"
FRAMEWORKS="$APP/Contents/Frameworks"
IDENTITY="${MACRUNNER_CODESIGN_IDENTITY:-Apple Development: 27GN9XE9CP}"

mkdir -p "$DIST"
rm -rf "$STAGE"
mkdir -p "$APP/Contents/MacOS" "$RES" "$FRAMEWORKS"

swift build -c release --package-path "$APP_SRC"
cp "$APP_SRC/.build/release/MacRunnerControlCenter" "$APP/Contents/MacOS/MacRunner"
if [[ -x "$APP_SRC/.build/release/macr-hud" ]]; then
  cp "$APP_SRC/.build/release/macr-hud" "$RES/macr-hud"
fi
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>MacRunner</string>
<key>CFBundleIdentifier</key><string>app.macrunner.control-center</string>
<key>CFBundleName</key><string>MacRunner</string>
<key>CFBundleShortVersionString</key><string>$VERSION</string>
<key>CFBundleVersion</key><string>$VERSION</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
</dict></plist>
PLIST

mkdir -p "$RES/engine" "$RES/hyperbridge" "$RES/bridge"
[[ -d "$ROOT/engine/wine/dist" ]] && ditto "$ROOT/engine/wine/dist" "$RES/engine/wine-dist" || true
[[ -d "$ROOT/engine/hyperbridge/dist" ]] && ditto "$ROOT/engine/hyperbridge/dist" "$RES/hyperbridge" || true
[[ -f "$ROOT/engine/bridge/arm64ec-x64-bridge" ]] && cp "$ROOT/engine/bridge/arm64ec-x64-bridge" "$RES/bridge/" || true

ditto "$APP_SRC/Frameworks/Sparkle.framework" "$FRAMEWORKS/Sparkle.framework"
ditto "$APP_SRC/Frameworks/CrashReporter.framework" "$FRAMEWORKS/CrashReporter.framework"
mkdir -p "$RES/tools"
ditto "$APP_SRC/Sources/MacRunnerControlCenter/Resources/tools" "$RES/tools"
chmod +x "$RES/tools/legendary" "$RES/tools/gogdl"
install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP/Contents/MacOS/MacRunner" 2>/dev/null || true

if security find-identity -v -p codesigning | grep -q "$IDENTITY"; then
  codesign --force --deep --options runtime --sign "$IDENTITY" "$APP"
elif command -v codesign >/dev/null 2>&1; then
  codesign --force --deep --sign - "$APP"
fi

DMG="$DIST/MacRunner-$VERSION.dmg"
rm -f "$DMG"
hdiutil create -volname "MacRunner $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
shasum -a 256 "$DMG" > "$DMG.sha256"
cat > "$DIST/appcast.xml" <<XML
<rss version="2.0"><channel><title>MacRunner Updates</title><item><title>MacRunner $VERSION</title><enclosure url="file://$DMG" sparkle:version="$VERSION" sparkle:shortVersionString="$VERSION" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" /></item></channel></rss>
XML
printf 'DMG: %s\n' "$DMG"
printf 'SHA256: %s\n' "$(cut -d ' ' -f1 "$DMG.sha256")"
