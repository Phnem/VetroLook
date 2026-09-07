#!/bin/sh
# Build the TinyEXR v3 browser viewer WASM module (MinSizeRel + -Oz).
#
# Requires the Emscripten SDK on PATH (emcmake / emcc). Override the build dir
# with BUILD_DIR=... if you like.
set -e

cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build}"

emcmake cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build "$BUILD_DIR"

# Place the module + wasm next to index.html so it can be served directly.
cp "$BUILD_DIR/exr_viewer.mjs" "$BUILD_DIR/exr_viewer.wasm" .

echo
echo "Built exr_viewer.mjs + exr_viewer.wasm"
ls -lh exr_viewer.wasm
echo "Serve with:  python3 -m http.server   (then open http://localhost:8000/)"
