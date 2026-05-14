#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

APP_NAME="MacRunner Control Center"
BUNDLE_ID="com.macrunner.controlcenter"
BUNDLE_VERSION="0.2.0"
BUILD_DIR=".build/release"
APP_BUNDLE="dist/${APP_NAME}.app"

echo "Building ${APP_NAME}..."
swift build -c release

echo "Assembling .app bundle..."
rm -rf "${APP_BUNDLE}"
mkdir -p "${APP_BUNDLE}/Contents/MacOS"
mkdir -p "${APP_BUNDLE}/Contents/Resources"

# Copy executable
cp "${BUILD_DIR}/MacRunnerControlCenter" "${APP_BUNDLE}/Contents/MacOS/${APP_NAME}"

# Write Info.plist
cat > "${APP_BUNDLE}/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>MacRunner Control Center</string>
    <key>CFBundleIdentifier</key>
    <string>com.macrunner.controlcenter</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>MacRunner Control Center</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>0.2.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.utilities</string>
    <key>LSBackgroundOnly</key>
    <false/>
    <key>NSHumanReadableCopyright</key>
    <string>Copyright 2026 MacRunner Project</string>
</dict>
</plist>
EOF

# Placeholder icon (empty icns file so Finder shows default app icon)
touch "${APP_BUNDLE}/Contents/Resources/AppIcon.icns"

# Copy docs into Resources
cp -R docs "${APP_BUNDLE}/Contents/Resources/" 2>/dev/null || true
cp README.md "${APP_BUNDLE}/Contents/Resources/" 2>/dev/null || true

# Ad-hoc sign the bundle
codesign --force --deep --sign - "${APP_BUNDLE}" 2>/dev/null || true

echo "App bundle created at: $(pwd)/${APP_BUNDLE}"
