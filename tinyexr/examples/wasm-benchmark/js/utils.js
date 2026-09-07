// TinyEXR V3 WASM Benchmark - Utility Functions

/**
 * Calculate statistics from an array of values
 * @param {number[]} values - Array of numeric values
 * @returns {Object} Statistics: median, mean, stdDev, min, max
 */
export function calculateStats(values) {
    if (!values || values.length === 0) {
        return { median: 0, mean: 0, stdDev: 0, min: 0, max: 0 };
    }

    const sorted = [...values].sort((a, b) => a - b);
    const n = sorted.length;

    // Median
    const median = n % 2 === 0
        ? (sorted[n / 2 - 1] + sorted[n / 2]) / 2
        : sorted[Math.floor(n / 2)];

    // Mean
    const mean = values.reduce((a, b) => a + b, 0) / n;

    // Standard deviation
    const variance = values.reduce((sum, v) => sum + Math.pow(v - mean, 2), 0) / n;
    const stdDev = Math.sqrt(variance);

    return {
        median,
        mean,
        stdDev,
        min: sorted[0],
        max: sorted[n - 1]
    };
}

/**
 * Format bytes to human-readable string
 * @param {number} bytes - Number of bytes
 * @returns {string} Formatted string (e.g., "1.5 MB")
 */
export function formatBytes(bytes) {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
    return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

/**
 * Format throughput to human-readable string
 * @param {number} mbps - Megabytes per second
 * @returns {string} Formatted string (e.g., "150.5 MB/s")
 */
export function formatThroughput(mbps) {
    if (mbps < 1) return `${(mbps * 1024).toFixed(1)} KB/s`;
    if (mbps < 1024) return `${mbps.toFixed(1)} MB/s`;
    return `${(mbps / 1024).toFixed(2)} GB/s`;
}

/**
 * Format time in milliseconds
 * @param {number} ms - Time in milliseconds
 * @returns {string} Formatted string
 */
export function formatTime(ms) {
    if (ms < 1) return `${(ms * 1000).toFixed(0)} us`;
    if (ms < 1000) return `${ms.toFixed(2)} ms`;
    return `${(ms / 1000).toFixed(2)} s`;
}

/**
 * Calculate throughput in MB/s
 * @param {number} bytes - Number of bytes processed
 * @param {number} timeMs - Time in milliseconds
 * @returns {number} Throughput in MB/s
 */
export function calculateThroughput(bytes, timeMs) {
    if (timeMs <= 0) return 0;
    return (bytes / (1024 * 1024)) / (timeMs / 1000);
}

/**
 * Compression type information
 */
export const COMPRESSION_TYPES = [
    { name: 'NONE', value: 0, description: 'Uncompressed' },
    { name: 'RLE', value: 1, description: 'Run-length encoding' },
    { name: 'ZIPS', value: 2, description: 'ZIP single scanline' },
    { name: 'ZIP', value: 3, description: 'ZIP 16 scanlines' },
    { name: 'PIZ', value: 4, description: 'Wavelet + Huffman' },
    { name: 'PXR24', value: 5, description: '24-bit float' },
    { name: 'B44', value: 6, description: 'Lossy 4x4 blocks' },
    { name: 'B44A', value: 7, description: 'B44 with alpha' }
];

/**
 * Image size configurations
 */
export const IMAGE_SIZES = [
    { name: '512x512', width: 512, height: 512 },
    { name: '1920x1080', width: 1920, height: 1080 },
    { name: '4096x4096', width: 4096, height: 4096 }
];

/**
 * Sleep for specified milliseconds (for UI updates)
 * @param {number} ms - Milliseconds to sleep
 * @returns {Promise<void>}
 */
export function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}
