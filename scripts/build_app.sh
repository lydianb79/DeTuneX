#!/bin/bash
# Builds De-TuneX.app (release) into ./build using SwiftPM — no Xcode project needed.
# Usage: bash scripts/build_app.sh [--universal]
set -eo pipefail
cd "$(dirname "$0")/.."

ARCH=""
if [ "${1:-}" = "--universal" ]; then
    ARCH="--arch arm64 --arch x86_64"
fi

echo "▸ Building (release)…"
# shellcheck disable=SC2086
swift build -c release $ARCH
# shellcheck disable=SC2086
BIN_DIR="$(swift build -c release $ARCH --show-bin-path)"

APP="build/DeTuneX.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN_DIR/DeTuneX" "$APP/Contents/MacOS/DeTuneX"
cp Support/Info.plist "$APP/Contents/Info.plist"
cp Support/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"

# Ad-hoc sign with the hardened runtime + audio-input entitlement so macOS
# shows the microphone permission prompt for De-TuneX itself.
echo "▸ Signing (ad-hoc)…"
codesign --force --options runtime \
    --entitlements Support/DeTuneX.entitlements \
    --sign - "$APP"

# Refresh Finder/Dock icon caches for this bundle.
touch "$APP"

echo "✓ Built $APP"
echo "  Run with: open $APP"
