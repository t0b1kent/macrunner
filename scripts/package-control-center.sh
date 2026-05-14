#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_SRC="$ROOT/app/macr-control-center"
VERSION="${MACRUNNER_VERSION:-1.0}"
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
cat > "$RES/engine/README.txt" <<'ENGINE'
MacRunner Control Center package is built without reading or bundling engine/ or wine-fork/.
Runtime engines are discovered at launch through configured local paths and invoked via Process().
ENGINE

ditto "$APP_SRC/Frameworks/Sparkle.framework" "$FRAMEWORKS/Sparkle.framework"
ditto "$APP_SRC/Frameworks/CrashReporter.framework" "$FRAMEWORKS/CrashReporter.framework"
mkdir -p "$RES/tools"
ditto "$APP_SRC/Sources/MacRunnerControlCenter/Resources/tools" "$RES/tools"
chmod +x "$RES/tools/legendary" "$RES/tools/gogdl"
cat > "$RES/LICENSES.txt" <<'LICENSES'
MacRunner bundles third-party components including Wine-derived runtime pieces, Sparkle, PLCrashReporter, legendary, and gogdl.

Wine modifications are handled under LGPL 2.1 minimal-compliance terms. Source for LGPL-covered Wine modifications is available to recipients on request: source@macrunner.app.

MacRunner Control Center, product logic, profiles, UI, compatibility reports, and HyperBridge integration are proprietary closed-source components.
LICENSES
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
<rss version="2.0"><channel><title>MacRunner Updates</title><item><title>MacRunner $VERSION</title><enclosure url="file://$DMG" sparkle:version="$VERSION" sparkle:shortVersionString="$VERSION" xmlns:sparkle="urn:macrunner:sparkle" /></item></channel></rss>
XML
printf 'DMG: %s\n' "$DMG"
printf 'SHA256: %s\n' "$(cut -d ' ' -f1 "$DMG.sha256")"
