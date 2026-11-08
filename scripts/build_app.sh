#!/bin/bash
# Builds De-TuneX.app (release) into ./build using SwiftPM — no Xcode project needed.
#
# Usage: bash scripts/build_app.sh [--both | --x86_64 | --arm64 | --universal]
#   (no option)   native architecture of this Mac        → build/DeTuneX.app
#   --both        two separate apps, labelled            → build/DeTuneX-arm64.app  ("De-TuneX (Apple Silicon)")
#                                                           build/DeTuneX-x86_64.app ("De-TuneX (Intel)")
#                 plus a zip of each for sharing
#   --arm64       Apple Silicon only                     → build/DeTuneX-arm64.app
#   --x86_64      Intel only                             → build/DeTuneX-x86_64.app
#   --universal   one app containing both                → build/DeTuneX-universal.app
set -eo pipefail
cd "$(dirname "$0")/.."

MIN_MACOS="13.0"
PLIST_BUDDY=/usr/libexec/PlistBuddy

# build_one <swift-build-args> <app-file-name> <display-label or empty>
build_one() {
    local args="$1" name="$2" label="$3"
    echo ""
    echo "▸ Building $name (release)…"
    # shellcheck disable=SC2086
    swift build -c release $args
    # shellcheck disable=SC2086
    local bin_dir; bin_dir="$(swift build -c release $args --show-bin-path)"

    local app="build/$name.app"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
    cp "$bin_dir/DeTuneX" "$app/Contents/MacOS/DeTuneX"
    cp Support/Info.plist "$app/Contents/Info.plist"
    cp Support/AppIcon.icns "$app/Contents/Resources/AppIcon.icns"

    if [ -n "$label" ]; then
        # Label what Finder, the Dock and the menu bar show.
        "$PLIST_BUDDY" -c "Set :CFBundleDisplayName De-TuneX ($label)" "$app/Contents/Info.plist"
        "$PLIST_BUDDY" -c "Set :CFBundleName De-TuneX ($label)" "$app/Contents/Info.plist"
    fi

    # Ad-hoc sign with the hardened runtime + audio-input entitlement so macOS
    # shows the microphone permission prompt for De-TuneX itself.
    codesign --force --options runtime \
        --entitlements Support/DeTuneX.entitlements \
        --sign - "$app"
    touch "$app"   # refresh Finder/Dock icon caches

    echo "✓ $app  —  architecture(s): $(lipo -archs "$app/Contents/MacOS/DeTuneX")"
}

zip_app() {
    local name="$1"
    rm -f "build/$name.zip"
    ditto -c -k --keepParent "build/$name.app" "build/$name.zip"
    echo "  zipped → build/$name.zip"
}

ARM="--triple arm64-apple-macosx${MIN_MACOS}"
X86="--triple x86_64-apple-macosx${MIN_MACOS}"

case "${1:-}" in
    --both|--all)
        build_one "$ARM" "DeTuneX-arm64"  "Apple Silicon"; zip_app "DeTuneX-arm64"
        build_one "$X86" "DeTuneX-x86_64" "Intel";         zip_app "DeTuneX-x86_64"
        ;;
    --arm64)          build_one "$ARM" "DeTuneX-arm64"  "Apple Silicon" ;;
    --x86_64|--intel) build_one "$X86" "DeTuneX-x86_64" "Intel" ;;
    --universal)      build_one "--arch arm64 --arch x86_64" "DeTuneX-universal" "" ;;
    "")               build_one "" "DeTuneX" "" ;;
    *) echo "Unknown option: $1"
       echo "Usage: bash scripts/build_app.sh [--both | --x86_64 | --arm64 | --universal]"; exit 1 ;;
esac

echo ""
echo "Done. Apps are in ./build"
