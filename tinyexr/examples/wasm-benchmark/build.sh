#!/bin/bash
# TinyEXR V3 WASM Benchmark Build Script
# Requires Emscripten SDK

set -e

# Configuration
EMCC="${EMCC:-emcc}"
EMCXX="${EMCXX:-em++}"
BUILD_DIR="dist"
SRC_DIR="src"

# TinyEXR paths (relative to wasm-benchmark directory)
TINYEXR_ROOT="../.."
MINIZ_DIR="${TINYEXR_ROOT}/deps/miniz"

# Check for Emscripten
if ! command -v "$EMCXX" &> /dev/null; then
    echo "Error: em++ not found. Please install and activate Emscripten SDK."
    echo "  https://emscripten.org/docs/getting_started/downloads.html"
    exit 1
fi

echo "=== TinyEXR V3 WASM Benchmark Build ==="
echo "Emscripten: $(${EMCXX} --version | head -1)"
echo ""

# Common compiler flags
COMMON_FLAGS=(
    -O2
    -I"${TINYEXR_ROOT}"
    -I"${MINIZ_DIR}"
    -DMINIZ_NO_STDIO
    -DTINYEXR_USE_MINIZ=1
    -DTINYEXR_USE_THREAD=0
)

# Link flags
LINK_FLAGS=(
    --bind
    -s ALLOW_MEMORY_GROWTH=1
    -s INITIAL_MEMORY=134217728     # 128MB initial
    -s MAXIMUM_MEMORY=1073741824    # 1GB max
    -s MODULARIZE=1
    -s EXPORT_ES6=1
    -s EXPORT_NAME="createTinyExrV3Module"
    -s ENVIRONMENT='web,node'
    -s WASM=1
    -s SINGLE_FILE=0
    -s NO_EXIT_RUNTIME=1
    -s FILESYSTEM=0
    -s DISABLE_EXCEPTION_CATCHING=1
)

# Create build directory
mkdir -p "${BUILD_DIR}"

echo "1. Compiling miniz..."
"${EMCC}" "${COMMON_FLAGS[@]}" \
    -c "${MINIZ_DIR}/miniz.c" \
    -o "${BUILD_DIR}/miniz.o"

echo "2. Compiling TinyEXR C implementation..."
"${EMCC}" "${COMMON_FLAGS[@]}" \
    -c "${TINYEXR_ROOT}/tinyexr_c_impl.c" \
    -o "${BUILD_DIR}/tinyexr_c_impl.o"

echo "3. Compiling V3 benchmark bindings..."
"${EMCXX}" "${COMMON_FLAGS[@]}" \
    -std=c++17 \
    -c "${SRC_DIR}/v3_benchmark_bindings.cc" \
    -o "${BUILD_DIR}/v3_benchmark_bindings.o"

echo "4. Linking WASM module..."
"${EMCXX}" "${LINK_FLAGS[@]}" \
    "${BUILD_DIR}/miniz.o" \
    "${BUILD_DIR}/tinyexr_c_impl.o" \
    "${BUILD_DIR}/v3_benchmark_bindings.o" \
    -o "${BUILD_DIR}/tinyexr_v3_wasm.js"

# Generate TypeScript declarations
echo "5. Generating TypeScript declarations..."
cat > "${BUILD_DIR}/tinyexr_v3_wasm.d.ts" << 'EOF'
// TypeScript declarations for TinyEXR V3 WASM Benchmark

export interface EncodedImage {
    ok(): boolean;
    error(): string;
    getBytes(): Uint8Array | null;
    size(): number;
    encodeTimeMs(): number;
    delete(): void;
}

export interface DecodedImage {
    ok(): boolean;
    error(): string;
    getBytes(): Float32Array | null;
    width(): number;
    height(): number;
    numChannels(): number;
    decodeTimeMs(): number;
    delete(): void;
}

export interface MemoryInfo {
    heapTotal: number;
    heapUsed: number;
}

export interface V3BenchmarkContext {
    isValid(): boolean;
    encodeImage(width: number, height: number, numChannels: number,
                pixelData: string, compressionType: number): EncodedImage;
    decodeImage(exrData: string): DecodedImage;
    delete(): void;
}

export interface V3BenchmarkContextConstructor {
    new(): V3BenchmarkContext;
    generateRandomImage(width: number, height: number, seed: number): string;
    getCompressionName(type: number): string;
    getMemoryInfo(): MemoryInfo;
}

export interface TinyExrV3Module {
    V3BenchmarkContext: V3BenchmarkContextConstructor;
    COMPRESSION_NONE: number;
    COMPRESSION_RLE: number;
    COMPRESSION_ZIPS: number;
    COMPRESSION_ZIP: number;
    COMPRESSION_PIZ: number;
    COMPRESSION_PXR24: number;
    COMPRESSION_B44: number;
    COMPRESSION_B44A: number;
    HEAPU8: Uint8Array;
}

declare function createTinyExrV3Module(): Promise<TinyExrV3Module>;
export default createTinyExrV3Module;
EOF

echo ""
echo "=== Build Complete ==="
echo "Output files:"
echo "  ${BUILD_DIR}/tinyexr_v3_wasm.js"
echo "  ${BUILD_DIR}/tinyexr_v3_wasm.wasm"
echo "  ${BUILD_DIR}/tinyexr_v3_wasm.d.ts"
echo ""
echo "Run benchmarks:"
echo "  Node.js: node js/benchmark.mjs"
echo "  Browser: python3 -m http.server 8080"
