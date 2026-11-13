#!/usr/bin/env bash
# Unified build script for macOS native and Windows x64 (Zig cross-compile).
# Usage:
#   ./scripts/build_all.sh mac      # macOS plugins + app + tests
#   ./scripts/build_all.sh win      # Windows x64 plugins + exe + portable package
#   ./scripts/build_all.sh all      # Both macOS and Windows x64 (default)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-all}"

build_mac() {
    echo "=== [macOS] Building native decoder plugins (plugins/*.dylib) ==="
    cmake -B "${ROOT_DIR}/native/build-mac" -S "${ROOT_DIR}/native" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${ROOT_DIR}/native/build-mac" -j4

    echo "=== [macOS] Building ulv_viewer and .app bundle ==="
    cmake -B "${ROOT_DIR}/cpp/build" -S "${ROOT_DIR}/cpp" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${ROOT_DIR}/cpp/build" -j4
    local app_plugins="${ROOT_DIR}/砺见智行激光雷达测试工具.app/Contents/Resources/plugins"
    mkdir -p "${app_plugins}"
    rm -f "${app_plugins}"/*.dll "${app_plugins}"/*.dylib
    cp "${ROOT_DIR}/plugins/"*.dylib "${app_plugins}/"

    echo "=== [macOS] Building C++ tests ==="
    cmake -B "${ROOT_DIR}/cpp/build-tests" -S "${ROOT_DIR}/cpp/tests" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${ROOT_DIR}/cpp/build-tests" -j4
}

build_win() {
    local tc="${ROOT_DIR}/cpp/cmake/zig-windows-x64-toolchain.cmake"

    echo "=== [Windows x64] Cross-compiling native decoder plugins (plugins/*.dll) ==="
    cmake -B "${ROOT_DIR}/native/build-win" -S "${ROOT_DIR}/native" \
        -DCMAKE_TOOLCHAIN_FILE="${tc}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${ROOT_DIR}/native/build-win" -j4

    echo "=== [Windows x64] Cross-compiling ulv_viewer.exe ==="
    cmake -B "${ROOT_DIR}/cpp/build-win" -S "${ROOT_DIR}/cpp" \
        -DCMAKE_TOOLCHAIN_FILE="${tc}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${ROOT_DIR}/cpp/build-win" -j4

    echo "=== [Windows x64] Assembling cpp/dist_windows_x64 ==="
    local out="${ROOT_DIR}/cpp/dist_windows_x64"
    rm -rf "${out}"
    mkdir -p "${out}/plugins" "${out}/corrections" "${out}/resources"
    cp "${ROOT_DIR}/cpp/build-win/bin/ulv_viewer.exe" "${out}/"
    cp "${ROOT_DIR}/plugins/"*.dll "${out}/plugins/"
    cp -R "${ROOT_DIR}/corrections/." "${out}/corrections/"
    cp -R "${ROOT_DIR}/cpp/resources/." "${out}/resources/"
    # Remove CMake/Zig stdlib probe artifact '-' and separate PDBs from plugins/
    rm -f "${ROOT_DIR}/-" "${ROOT_DIR}/plugins/"*.pdb
}

case "${MODE}" in
    mac) build_mac ;;
    win) build_win ;;
    all) build_mac && build_win ;;
    *)
        echo "Usage: $0 [mac|win|all]" >&2
        exit 1
        ;;
esac
