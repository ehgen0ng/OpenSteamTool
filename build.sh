#!/usr/bin/env bash
set -euo pipefail

# macOS counterpart of build.bat. Dobby's DOBBY_DEBUG option is configure-time,
# so Debug and Release intentionally use separate single-config build trees.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

CONFIGS="${CONFIGS:-Release Debug}"
GENERATOR="${GENERATOR:-Unix Makefiles}"
ARCH="${ARCH:-$(uname -m)}"
if [ -n "${JOBS:-}" ]; then
    BUILD_JOBS="$JOBS"
else
    BUILD_JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
    BUILD_JOBS="${BUILD_JOBS:-4}"
fi

for config in $CONFIGS; do
    case "$config" in
        Debug|Release) ;;
        *)
            echo "[ERROR] Unsupported configuration: $config" >&2
            exit 1
            ;;
    esac

    build_dir="build/macos/$config"
    echo "[INFO] Configuring $config ($ARCH) with generator: $GENERATOR"
    cmake -S src -B "$build_dir" \
        -G "$GENERATOR" \
        -DCMAKE_BUILD_TYPE="$config" \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH"

    echo "[INFO] Building: $config"
    cmake --build "$build_dir" --parallel "$BUILD_JOBS"
done

echo "[OK] Requested macOS builds completed successfully."
