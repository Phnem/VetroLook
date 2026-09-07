# TinyEXR V3 WASM Performance Benchmark

Performance benchmark for TinyEXR V3 API compiled to WebAssembly (WASM).
Tests encoding and decoding performance across all supported compression types.

## Features

- Tests 8 compression types: NONE, RLE, ZIPS, ZIP, PIZ, PXR24, B44, B44A
- Tests 3 image sizes: 512x512, 1920x1080, 4096x4096
- Measures encode time, decode time, file size, compression ratio, throughput (MB/s)
- Tracks memory usage
- Works in Node.js CLI and browser
- Generates random HDR test images for realistic compression behavior

## Prerequisites

- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (3.0+)
- Node.js 18+ (for CLI benchmark)
- Python 3 (for browser HTTP server)

## Building

```bash
# Navigate to benchmark directory
cd examples/wasm-benchmark

# Build with shell script
./build.sh

# Or use Make
make
```

This produces:
- `dist/tinyexr_v3_wasm.js` - Emscripten JS glue code
- `dist/tinyexr_v3_wasm.wasm` - WebAssembly binary
- `dist/tinyexr_v3_wasm.d.ts` - TypeScript declarations

## Running Benchmarks

### Node.js CLI

```bash
# Run benchmark
node js/benchmark.mjs

# With JSON output
node js/benchmark.mjs --json

# Verbose mode
node js/benchmark.mjs -v

# Help
node js/benchmark.mjs --help
```

### Browser

```bash
# Start HTTP server
python3 -m http.server 8080

# Or use Make
make run-browser
```

Then open http://localhost:8080/html/ in your browser.

## Example Output

```
TinyEXR V3 WASM Performance Benchmark
=====================================

=== Testing 1920x1080 ===
Generating random 1920x1080 RGBA float image...
Raw image size: 31.64 MB
  NONE   enc=   12.45ms dec=    8.32ms size=  31.64 MB (100.0%)
  RLE    enc=   28.76ms dec=   15.43ms size=  18.25 MB ( 57.7%)
  ZIPS   enc=  125.32ms dec=   42.18ms size=   8.92 MB ( 28.2%)
  ZIP    enc=  118.54ms dec=   38.65ms size=   8.78 MB ( 27.8%)
  PIZ    enc=   85.21ms dec=   52.34ms size=   6.45 MB ( 20.4%)
  PXR24  enc=   35.18ms dec=   18.92ms size=  23.73 MB ( 75.0%)
  B44    enc=   22.45ms dec=   12.67ms size=  12.55 MB ( 39.7%)
  B44A   enc=   23.12ms dec=   13.01ms size=  12.55 MB ( 39.7%)

=== Summary ===

Size         | Comp     | Encoded      | Ratio    | Enc(ms)    | Dec(ms)    | Enc MB/s   | Dec MB/s
-------------|----------|--------------|----------|------------|------------|------------|----------
1920x1080    | NONE     | 31.64 MB     | 100.0%   | 12.45      | 8.32       | 2541.0     | 3802.4
1920x1080    | RLE      | 18.25 MB     | 57.7%    | 28.76      | 15.43      | 1100.3     | 2051.2
...
```

## API Reference

### JavaScript (Browser/Node.js)

```javascript
import createTinyExrV3Module from './dist/tinyexr_v3_wasm.js';

// Initialize module
const Module = await createTinyExrV3Module();

// Create context
const ctx = new Module.V3BenchmarkContext();

// Generate random test image (returns RGBA float data as string)
const imageData = Module.V3BenchmarkContext.generateRandomImage(width, height, seed);

// Encode image
const encoded = ctx.encodeImage(width, height, 4, imageData, Module.COMPRESSION_PIZ);
if (encoded.ok()) {
    console.log('Encode time:', encoded.encodeTimeMs(), 'ms');
    console.log('Size:', encoded.size(), 'bytes');
    const bytes = encoded.getBytes(); // Uint8Array
}
encoded.delete();

// Decode image
const decoded = ctx.decodeImage(exrDataString);
if (decoded.ok()) {
    console.log('Decode time:', decoded.decodeTimeMs(), 'ms');
    console.log('Dimensions:', decoded.width(), 'x', decoded.height());
    const pixels = decoded.getBytes(); // Float32Array
}
decoded.delete();

// Get memory info
const mem = Module.V3BenchmarkContext.getMemoryInfo();
console.log('Heap:', mem.heapUsed, '/', mem.heapTotal);

// Cleanup
ctx.delete();
```

### Compression Constants

```javascript
Module.COMPRESSION_NONE    // 0 - Uncompressed
Module.COMPRESSION_RLE     // 1 - Run-length encoding
Module.COMPRESSION_ZIPS    // 2 - ZIP single scanline
Module.COMPRESSION_ZIP     // 3 - ZIP 16 scanlines
Module.COMPRESSION_PIZ     // 4 - Wavelet + Huffman
Module.COMPRESSION_PXR24   // 5 - 24-bit float
Module.COMPRESSION_B44     // 6 - Lossy 4x4 blocks
Module.COMPRESSION_B44A    // 7 - B44 with alpha
```

## File Structure

```
examples/wasm-benchmark/
├── README.md           # This file
├── build.sh            # Build script
├── Makefile            # Alternative build
├── src/
│   └── v3_benchmark_bindings.cc   # C++ Emscripten bindings
├── js/
│   ├── benchmark.mjs              # Node.js CLI benchmark
│   ├── benchmark-browser.js       # Browser benchmark
│   ├── utils.js                   # Shared utilities
│   └── results-formatter.js       # Result formatting
├── html/
│   ├── index.html      # Browser UI
│   └── style.css       # Styles
└── dist/               # Build output (generated)
    ├── tinyexr_v3_wasm.js
    ├── tinyexr_v3_wasm.wasm
    └── tinyexr_v3_wasm.d.ts
```

## Notes

- The benchmark uses a fixed random seed (42) for reproducible results
- HDR test images have exposure range from 2^-6 to 2^6 with gradient + noise pattern
- 4096x4096 images require ~268MB raw pixel data; WASM heap is configured for 512MB max
- B44/B44A are lossy compression formats (for HDR images)
- DWAA/DWAB compression is not yet implemented in V3 API

## Troubleshooting

### WASM module fails to load

Make sure you've built the module first:
```bash
./build.sh
```

### Memory errors with 4096x4096

The default WASM heap may be too small. Check build.sh for memory settings:
```
-s INITIAL_MEMORY=67108864   # 64MB initial
-s MAXIMUM_MEMORY=536870912  # 512MB max
```

### Browser doesn't allow ES modules from file://

Use an HTTP server instead of opening the HTML file directly:
```bash
python3 -m http.server 8080
```
