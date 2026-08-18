#!/usr/bin/env bash
# Local build helper.
#   ./scripts/build.sh            full build, plugin + tests
#   ./scripts/build.sh dsp        DSP and tests only, no JUCE fetch (seconds)
set -euo pipefail

cd "$(dirname "$0")/.."

MODE="${1:-all}"
BUILD_DIR="build"
CMAKE_ARGS=(-S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release)

if command -v ninja >/dev/null 2>&1; then
    CMAKE_ARGS+=(-G Ninja)
fi

if [ "$MODE" = "dsp" ]; then
    BUILD_DIR="build-dsp"
    CMAKE_ARGS=("${CMAKE_ARGS[@]/-B build/-B $BUILD_DIR}")
    CMAKE_ARGS+=(-DFBK_BUILD_PLUGIN=OFF -DFBK_BUILD_TESTS=ON)
else
    CMAKE_ARGS+=(-DFBK_BUILD_PLUGIN=ON -DFBK_BUILD_TESTS=ON)
fi

cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --parallel

echo
echo "--- running DSP tests ---"
"./$BUILD_DIR/tests/fbk_tests"

if [ "$MODE" != "dsp" ]; then
    echo
    echo "--- artefacts ---"
    find "$BUILD_DIR/src/plugin/FBKSuppressor_artefacts" -maxdepth 2 -mindepth 2 2>/dev/null || true
fi
