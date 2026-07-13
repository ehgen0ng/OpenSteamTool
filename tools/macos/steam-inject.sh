#!/bin/bash
# Ad-hoc re-sign steam_osx (drops hardened runtime => library validation off,
# DYLD_* honored) and inject OpenSteamTool, or restore the original binary.
#
#   steam-inject.sh [--config Debug|Release]  re-sign + inject + run
#   steam-inject.sh --restore                 restore Valve-signed steam_osx
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
STEAM_BIN="/Applications/Steam.app/Contents/MacOS/steam_osx"
CONFIG="${OST_CONFIG:-Debug}"
RESTORE=false

usage() {
    echo "Usage: $0 [--config Debug|Release] [--restore]" >&2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --config)
            [ "$#" -ge 2 ] || { usage; exit 2; }
            CONFIG="$2"
            shift 2
            ;;
        --restore)
            RESTORE=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

case "$CONFIG" in
    Debug|Release) ;;
    *)
        echo "[ERROR] Unsupported configuration: $CONFIG" >&2
        exit 2
        ;;
esac

# Default to a libOpenSteamTool.dylib sitting next to this script (portable /
# distribution layout); fall back to the selected build output for dev builds.
if [ -n "${OST_DYLIB:-}" ]; then
    : # Explicit override already selected.
elif [ -f "$SCRIPT_DIR/libOpenSteamTool.dylib" ]; then
    OST_DYLIB="$SCRIPT_DIR/libOpenSteamTool.dylib"
else
    OST_DYLIB="$REPO_ROOT/build/macos/$CONFIG/libOpenSteamTool.dylib"
fi

if $RESTORE; then
    cp -p "$STEAM_BIN.ostbak" "$STEAM_BIN"
    rm -f "$STEAM_BIN.ostbak"
else
    if [ ! -f "$OST_DYLIB" ]; then
        echo "[ERROR] Missing $CONFIG artifact: $OST_DYLIB" >&2
        echo "[INFO] Run ./build.sh first, or set OST_DYLIB explicitly." >&2
        exit 1
    fi
    [ -f "$STEAM_BIN.ostbak" ] || cp -p "$STEAM_BIN" "$STEAM_BIN.ostbak"
    codesign -f -s - "$STEAM_BIN"
    codesign -f -s - "$OST_DYLIB"
    DYLD_INSERT_LIBRARIES="$OST_DYLIB" "$STEAM_BIN"
fi
