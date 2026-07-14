#!/bin/bash
set -e

ROOT=$(cd "$(dirname "$0")" && pwd)
cd "$ROOT"

: "${ANDROID_NDK:=/opt/android-sdk/ndk/27.0.12077973}"
: "${ANDROID_ABI:=arm64-v8a}"
: "${ANDROID_PLATFORM:=android-27}"

# Build the host-side patcher tool
PATCHER_BUILD=src/patcher/build
cmake -S src/patcher -B "$PATCHER_BUILD"
cmake --build "$PATCHER_BUILD" --parallel
echo "Patcher built: $PATCHER_BUILD/pvr-patcher"

# Build the shim .so (needs an APK for symbol extraction, or use the patcher)
if [ -n "$1" ]; then
    APK="$1"
    python3 tools/extract_pvr_functions.py \
        --apk "$APK" \
        --out src/generated/functions.json

    python3 tools/generate_forward_stubs.py \
        --functions src/generated/functions.json \
        --hooks tools/hooks.json \
        --stubs src/generated/forward_stubs.S \
        --vars src/generated/forward_vars.cpp

    BUILD_DIR=build
    rm -rf "$BUILD_DIR"
    cmake \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
        -S . -B "$BUILD_DIR"

    cmake --build "$BUILD_DIR" --parallel
    echo "Shim built: $BUILD_DIR/libPvr_UnitySDK.so"
else
    echo ""
    echo "To patch a game, run:"
    echo "  $PATCHER_BUILD/pvr-patcher <input.apk> [output.apk]"
    echo ""
    echo "The patcher will extract symbols, build the shim, patch the APK, and sign it."
fi
