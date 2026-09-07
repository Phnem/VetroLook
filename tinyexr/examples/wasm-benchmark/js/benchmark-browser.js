// TinyEXR V3 WASM Benchmark - Browser Version

import { calculateStats, formatBytes, calculateThroughput, COMPRESSION_TYPES, IMAGE_SIZES, sleep } from './utils.js';
import { formatResultsJSON, formatResultsCSV } from './results-formatter.js';

// Global state
let Module = null;
let ctx = null;
const allResults = [];

// DOM elements
const statusEl = document.getElementById('status');
const memoryInfoEl = document.getElementById('memory-info');
const runBtn = document.getElementById('runBtn');
const runAllBtn = document.getElementById('runAllBtn');
const clearBtn = document.getElementById('clearBtn');
const exportBtn = document.getElementById('exportBtn');
const exportCSVBtn = document.getElementById('exportCSVBtn');
const imageSizeSelect = document.getElementById('imageSize');
const iterationsInput = document.getElementById('iterations');
const resultsBody = document.getElementById('results-body');
const progressContainer = document.getElementById('progress-container');
const progressBar = document.getElementById('progress-bar');
const progressText = document.getElementById('progress-text');

// Configuration
const WARMUP_ITERATIONS = 2;
const RANDOM_SEED = 42;
const NUM_CHANNELS = 4;

/**
 * Initialize WASM module
 */
async function initModule() {
    setStatus('Loading WASM module...');

    try {
        // Import the WASM module
        const createModule = (await import('../dist/tinyexr_v3_wasm.js')).default;
        Module = await createModule();

        // Create benchmark context
        ctx = new Module.V3BenchmarkContext();
        if (!ctx.isValid()) {
            throw new Error('Failed to create V3 context');
        }

        setStatus('Module loaded. Ready to benchmark.', 'success');
        enableControls(true);
        updateMemoryInfo();
    } catch (err) {
        setStatus(`Failed to load module: ${err.message}`, 'error');
        console.error(err);
    }
}

/**
 * Set status message
 */
function setStatus(message, type = '') {
    statusEl.textContent = message;
    statusEl.className = type;
}

/**
 * Enable/disable control buttons
 */
function enableControls(enabled) {
    runBtn.disabled = !enabled;
    runAllBtn.disabled = !enabled;
    exportBtn.disabled = allResults.length === 0;
    exportCSVBtn.disabled = allResults.length === 0;
}

/**
 * Update memory info display
 */
function updateMemoryInfo() {
    if (!Module) return;

    const memInfo = Module.V3BenchmarkContext.getMemoryInfo();
    let text = `WASM Heap: ${formatBytes(memInfo.heapUsed)} / ${formatBytes(memInfo.heapTotal)}`;

    // Browser memory API (Chrome only)
    if (performance.memory) {
        text += ` | JS Heap: ${formatBytes(performance.memory.usedJSHeapSize)} / ${formatBytes(performance.memory.totalJSHeapSize)}`;
    }

    memoryInfoEl.textContent = text;
}

/**
 * Show/update progress
 */
function updateProgress(current, total, message) {
    progressContainer.style.display = 'block';
    const percent = Math.round((current / total) * 100);
    progressBar.style.width = `${percent}%`;
    progressText.textContent = `${message} (${current}/${total})`;
}

/**
 * Hide progress bar
 */
function hideProgress() {
    progressContainer.style.display = 'none';
}

/**
 * Run benchmark for a specific image size
 */
async function runBenchmark(width, height, iterations = 5) {
    const sizeName = `${width}x${height}`;
    setStatus(`Generating ${sizeName} test image...`);
    await sleep(10); // Allow UI update

    let imageData;
    try {
        // Generate random image
        imageData = Module.V3BenchmarkContext.generateRandomImage(width, height, RANDOM_SEED);
    } catch (err) {
        setStatus(`Failed to generate image: ${err.message}`, 'error');
        return;
    }

    const rawSize = width * height * NUM_CHANNELS * 4;

    const totalTests = COMPRESSION_TYPES.length;
    let testsCompleted = 0;

    for (const compression of COMPRESSION_TYPES) {
        updateProgress(testsCompleted, totalTests, `Testing ${sizeName} - ${compression.name}`);
        await sleep(10);

        const encodeTimes = [];
        const decodeTimes = [];
        let encodedSize = 0;

        try {
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
            for (let i = 0; i < iterations; i++) {
                const encoded = ctx.encodeImageFromData(imageData, compression.value);
                if (!encoded.ok()) {
                    console.warn(`Encode failed for ${compression.name}: ${encoded.error()}`);
                    encoded.delete();
                    continue;
                }

                encodeTimes.push(encoded.encodeTimeMs());
                encodedSize = encoded.size();

                const bytes = encoded.getBytes();
                if (bytes && bytes.byteOffset !== undefined) {
                    const decoded = ctx.decodeFromBuffer(bytes.byteOffset, bytes.length);
                    if (decoded.ok()) {
                        decodeTimes.push(decoded.decodeTimeMs());
                    }
                    decoded.delete();
                }
                encoded.delete();
            }
        } catch (err) {
            console.error(`Error testing ${compression.name}:`, err);
        }

        if (encodeTimes.length > 0) {
            const encodeStats = calculateStats(encodeTimes);
            const decodeStats = calculateStats(decodeTimes);
            const ratio = (encodedSize / rawSize * 100).toFixed(1);
            const encMBps = calculateThroughput(rawSize, encodeStats.median);
            const decMBps = calculateThroughput(rawSize, decodeStats.median);

            const result = {
                imageSize: sizeName,
                width,
                height,
                compression: compression.name,
                rawSize,
                encodedSize,
                compressionRatio: parseFloat(ratio),
                encodeTimeMs: encodeStats.median,
                decodeTimeMs: decodeStats.median,
                encodeThroughputMBps: encMBps,
                decodeThroughputMBps: decMBps
            };

            allResults.push(result);
            addResultRow(result);
        }

        testsCompleted++;
        updateMemoryInfo();
    }

    // Cleanup
    if (imageData && imageData.delete) {
        imageData.delete();
    }

    updateProgress(totalTests, totalTests, 'Completed');
    await sleep(500);
    hideProgress();
}

/**
 * Add a result row to the table
 */
function addResultRow(result) {
    // Remove "no results" message if present
    const noResults = resultsBody.querySelector('.no-results');
    if (noResults) {
        noResults.parentElement.remove();
    }

    const row = document.createElement('tr');
    row.innerHTML = `
        <td>${result.imageSize}</td>
        <td class="comp-${result.compression.toLowerCase()}">${result.compression}</td>
        <td>${formatBytes(result.encodedSize)}</td>
        <td>${result.compressionRatio}%</td>
        <td>${result.encodeTimeMs.toFixed(2)}</td>
        <td>${result.decodeTimeMs.toFixed(2)}</td>
        <td>${result.encodeThroughputMBps.toFixed(1)}</td>
        <td>${result.decodeThroughputMBps.toFixed(1)}</td>
    `;
    resultsBody.appendChild(row);

    exportBtn.disabled = false;
    exportCSVBtn.disabled = false;
}

/**
 * Clear all results
 */
function clearResults() {
    allResults.length = 0;
    resultsBody.innerHTML = '<tr><td colspan="8" class="no-results">Run a benchmark to see results</td></tr>';
    exportBtn.disabled = true;
    exportCSVBtn.disabled = true;
}

/**
 * Export results as JSON file
 */
function exportJSON() {
    const json = formatResultsJSON(allResults, {
        userAgent: navigator.userAgent
    });

    downloadFile(json, 'application/json', `tinyexr-v3-benchmark-${Date.now()}.json`);
}

/**
 * Export results as CSV file
 */
function exportCSV() {
    const csv = formatResultsCSV(allResults);
    downloadFile(csv, 'text/csv', `tinyexr-v3-benchmark-${Date.now()}.csv`);
}

/**
 * Download a file
 */
function downloadFile(content, mimeType, filename) {
    const blob = new Blob([content], { type: mimeType });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
}

// Event handlers
runBtn.addEventListener('click', async () => {
    enableControls(false);
    const [width, height] = imageSizeSelect.value.split(',').map(Number);
    const iterations = parseInt(iterationsInput.value) || 5;

    try {
        await runBenchmark(width, height, iterations);
        setStatus(`Completed ${width}x${height} benchmark`, 'success');
    } catch (err) {
        setStatus(`Benchmark failed: ${err.message}`, 'error');
        console.error(err);
    }

    enableControls(true);
});

runAllBtn.addEventListener('click', async () => {
    enableControls(false);
    const iterations = parseInt(iterationsInput.value) || 5;

    try {
        // Only test 512x512 due to WASM memory limits
        for (const size of IMAGE_SIZES.slice(0, 1)) {
            await runBenchmark(size.width, size.height, iterations);
        }
        setStatus('Benchmark completed (512x512 only due to WASM memory limits)', 'success');
    } catch (err) {
        setStatus(`Benchmark failed: ${err.message}`, 'error');
        console.error(err);
    }

    enableControls(true);
});

clearBtn.addEventListener('click', clearResults);
exportBtn.addEventListener('click', exportJSON);
exportCSVBtn.addEventListener('click', exportCSV);

// Initialize
initModule().catch(console.error);
