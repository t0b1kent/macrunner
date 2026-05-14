#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

APP_NAME="MacRunner Control Center"
APP_BUNDLE="dist/${APP_NAME}.app"
PKG_DIR="dist/MacRunner_Control_Center_v0.2"

echo "Packaging DMG-like folder..."
rm -rf "${PKG_DIR}"
mkdir -p "${PKG_DIR}"

# Copy app bundle
cp -R "${APP_BUNDLE}" "${PKG_DIR}/"

# Copy top-level docs
cp README.md "${PKG_DIR}/README.txt" 2>/dev/null || true
cp -R docs "${PKG_DIR}/Documentation" 2>/dev/null || true

# Create Applications symlink
ln -s /Applications "${PKG_DIR}/Applications"

echo "DMG-like folder ready at: $(pwd)/${PKG_DIR}"
echo "Next step: create a DMG or zip from this folder for distribution."
