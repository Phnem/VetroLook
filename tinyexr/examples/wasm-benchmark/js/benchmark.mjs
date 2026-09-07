#!/usr/bin/env node
// TinyEXR V3 WASM Benchmark - Node.js CLI

import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

import { calculateStats, formatBytes, calculateThroughput, COMPRESSION_TYPES, IMAGE_SIZES } from './utils.js';
import { formatResultsTable, formatResultsJSON } from './results-formatter.js';

// Get directory of this script
const __dirname = dirname(fileURLToPath(import.meta.url));

// Configuration
const NUM_CHANNELS = 4; // RGBA
const NUM_ITERATIONS = 5;
const WARMUP_ITERATIONS = 2;
const RANDOM_SEED = 42;

// Parse command line arguments
const args = process.argv.slice(2);
const outputJSON = args.includes('--json');
const verbose = args.includes('-v') || args.includes('--verbose');
const helpRequested = args.includes('-h') || args.includes('--help');

if (helpRequested) {
    console.log(`
TinyEXR V3 WASM Performance Benchmark

Usage: node benchmark.mjs [options]

Options:
  --json      Output results as JSON
  -v, --verbose  Verbose output
  -h, --help     Show this help message

The benchmark tests all 8 compression types across 3 image sizes:
  - 512x512, 1920x1080, 4096x4096
  - NONE, RLE, ZIPS, ZIP, PIZ, PXR24, B44, B44A
`);
    process.exit(0);
}

async function loadWasmModule() {
    // Dynamic import of the WASM module
    const wasmPath = join(__dirname, '..', 'dist', 'tinyexr_v3_wasm.js');

    try {
        const createModule = (await import(wasmPath)).default;
        return await createModule();
    } catch (err) {
        console.error('Failed to load WASM module.');
        console.error('Make sure you have built the module first:');
        console.error('  cd examples/wasm-benchmark && ./build.sh');
        console.error('');
        console.error('Error:', err.message);
        process.exit(1);
    }
}

async function runBenchmark() {
    console.log('TinyEXR V3 WASM Performance Benchmark');
    console.log('=====================================');
    console.log('');

    // Load WASM module
    console.log('Loading WASM module...');
    const Module = await loadWasmModule();

    // Create benchmark context
    const ctx = new Module.V3BenchmarkContext();
    if (!ctx.isValid()) {
        console.error('Failed to create V3 context');
        process.exit(1);
    }

    console.log('Context created successfully');
    console.log(`Iterations: ${NUM_ITERATIONS} (+ ${WARMUP_ITERATIONS} warmup)`);
    console.log('');

    const allResults = [];

    // Only test 512x512 to avoid memory issues with WASM
    // 1920x1080 and 4096x4096 require more than 1GB heap
    const testSizes = IMAGE_SIZES.slice(0, 1);

    for (const size of testSizes) {
        console.log(`\n=== Testing ${size.name} ===`);

        // Generate random test image
        console.log(`Generating random ${size.name} RGBA float image...`);
        const imageData = Module.V3BenchmarkContext.generateRandomImage(
            size.width, size.height, RANDOM_SEED
        );
        const pixelCount = size.width * size.height;
        const rawSize = pixelCount * NUM_CHANNELS * 4; // 4 bytes per float
        console.log(`Raw image size: ${formatBytes(rawSize)}`);

        for (const compression of COMPRESSION_TYPES) {
            if (verbose) {
                console.log(`\n  Testing ${compression.name}...`);
            } else {
                process.stdout.write(`  ${compression.name.padEnd(6)} `);
            }

            const encodeTimes = [];
            const decodeTimes = [];
            let encodedSize = 0;
            let encodeError = null;
            let decodeError = null;

            // Warmup
            for (let i = 0; i < WARMUP_ITERATIONS; i++) {
                const encoded = ctx.encodeImageFromData(imageData, compression.value);
                if (encoded.ok()) {
                    const bytes = encoded.getBytes();
                    if (bytes && bytes.byteOffset !== undefined) {
                        const decoded = ctx.decodeFromBuffer(bytes.byteOffset, bytes.length);
                        decoded.delete();
                    }
                }
                encoded.delete();
            }

            // Benchmark iterations
            for (let i = 0; i < NUM_ITERATIONS; i++) {
                // Encode
                const encoded = ctx.encodeImageFromData(imageData, compression.value);

                if (!encoded.ok()) {
                    encodeError = encoded.error();
                    encoded.delete();
                    continue;
                }

                encodeTimes.push(encoded.encodeTimeMs());
                encodedSize = encoded.size();

                // Get bytes for decoding
                const bytes = encoded.getBytes();
                if (bytes && bytes.byteOffset !== undefined) {
                    // Decode using pointer
                    const decoded = ctx.decodeFromBuffer(bytes.byteOffset, bytes.length);
                    if (decoded.ok()) {
                        decodeTimes.push(decoded.decodeTimeMs());
                    } else {
                        decodeError = decoded.error();
                    }
                    decoded.delete();
                }

                encoded.delete();
            }

            if (encodeTimes.length > 0) {
                const encodeStats = calculateStats(encodeTimes);
                const decodeStats = calculateStats(decodeTimes);
                const compressionRatio = (encodedSize / rawSize * 100).toFixed(1);
                const encodeThroughput = calculateThroughput(rawSize, encodeStats.median);
                const decodeThroughput = calculateThroughput(rawSize, decodeStats.median);

                const result = {
                    imageSize: size.name,
                    width: size.width,
                    height: size.height,
                    compression: compression.name,
                    rawSize,
                    encodedSize,
                    compressionRatio: parseFloat(compressionRatio),
                    encodeTimeMs: encodeStats.median,
                    encodeTimeStdDev: encodeStats.stdDev,
                    decodeTimeMs: decodeStats.median,
                    decodeTimeStdDev: decodeStats.stdDev,
                    encodeThroughputMBps: encodeThroughput,
                    decodeThroughputMBps: decodeThroughput
                };

                allResults.push(result);

                if (verbose) {
                    console.log(`    Encode: ${encodeStats.median.toFixed(2)}ms (${encodeThroughput.toFixed(1)} MB/s)`);
                    console.log(`    Decode: ${decodeStats.median.toFixed(2)}ms (${decodeThroughput.toFixed(1)} MB/s)`);
                    console.log(`    Size: ${formatBytes(encodedSize)} (${compressionRatio}% of raw)`);
                } else {
                    console.log(
                        `enc=${encodeStats.median.toFixed(2).padStart(8)}ms ` +
                        `dec=${decodeStats.median.toFixed(2).padStart(8)}ms ` +
                        `size=${formatBytes(encodedSize).padStart(10)} ` +
                        `(${compressionRatio.padStart(5)}%)`
                    );
                }
            } else {
                console.log(`  FAILED: ${encodeError || 'Unknown error'}`);
            }

            // Memory info
            if (verbose) {
                const memInfo = Module.V3BenchmarkContext.getMemoryInfo();
                console.log(`    Memory: heap ${formatBytes(memInfo.heapUsed)} / ${formatBytes(memInfo.heapTotal)}`);
            }
        }

        // Cleanup image data
        imageData.delete();
    }

    // Cleanup
    ctx.delete();

    // Output results
    console.log('\n\n=== Summary ===\n');
    console.log(formatResultsTable(allResults));

    if (outputJSON) {
        console.log('\n--- JSON Output ---');
        console.log(formatResultsJSON(allResults, {
            nodeVersion: process.version,
            platform: process.platform,
            arch: process.arch
        }));
    }

    // Memory usage
    const memUsage = process.memoryUsage();
    console.log(`\nNode.js memory: heap ${formatBytes(memUsage.heapUsed)} / ${formatBytes(memUsage.heapTotal)}`);
}

// Run benchmark
runBenchmark().catch(err => {
    console.error('Benchmark failed:', err);
    process.exit(1);
});
