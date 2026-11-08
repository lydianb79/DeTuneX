#!/bin/bash
# Builds De-TuneX.app (release) into ./build using SwiftPM — no Xcode project needed.
#
# Usage: bash scripts/build_app.sh [--x86_64 | --arm64 | --universal]
#   (no option)   native architecture of this Mac
#   --x86_64      Intel build (runs on Intel Macs, or on Apple Silicon via Rosetta 2)
#   --arm64       Apple Silicon build
#   --universal   one binary containing both
set -eo pipefail
cd "$(dirname "$0")/.."

MIN_MACOS="13.0"
case "${1:-}" in
    --x86_64|--intel) ARGS="--triple x86_64-apple-macosx${MIN_MACOS}"; LABEL="x86_64" ;;
    --arm64)          ARGS="--triple arm64-apple-macosx${MIN_MACOS}";  LABEL="arm64" ;;
    --universal)      ARGS="--arch arm64 --arch x86_64";               LABEL="universal" ;;
    "")               ARGS="";                                          LABEL="native ($(uname -m))" ;;
    *) echo "Unknown option: $1"; echo "Usage: bash scripts/build_app.sh [--x86_64 | --arm64 | --universal]"; exit 1 ;;
esac

echo "▸ Building release for $LABEL…"
# shellcheck disable=SC2086
swift build -c release $ARGS
# shellcheck disable=SC2086
BIN_DIR="$(swift build -c release $ARGS --show-bin-path)"

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
echo "  Architecture(s): $(lipo -archs "$APP/Contents/MacOS/DeTuneX")"
echo "  Run with: open $APP"
