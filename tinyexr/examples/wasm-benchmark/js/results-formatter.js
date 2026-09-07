// TinyEXR V3 WASM Benchmark - Results Formatter

import { formatBytes, formatThroughput } from './utils.js';

/**
 * Format results as ASCII table
 * @param {Array} results - Array of benchmark results
 * @returns {string} Formatted table string
 */
export function formatResultsTable(results) {
    if (!results || results.length === 0) {
        return 'No results to display.';
    }

    // Column definitions with widths
    const cols = [
        { name: 'Size', width: 12, key: 'imageSize' },
        { name: 'Comp', width: 8, key: 'compression' },
        { name: 'Encoded', width: 12, format: r => formatBytes(r.encodedSize) },
        { name: 'Ratio', width: 8, format: r => `${r.compressionRatio}%` },
        { name: 'Enc(ms)', width: 10, format: r => r.encodeTimeMs.toFixed(2) },
        { name: 'Dec(ms)', width: 10, format: r => r.decodeTimeMs.toFixed(2) },
        { name: 'Enc MB/s', width: 10, format: r => r.encodeThroughputMBps.toFixed(1) },
        { name: 'Dec MB/s', width: 10, format: r => r.decodeThroughputMBps.toFixed(1) }
    ];

    // Build header
    const header = cols.map(c => c.name.padEnd(c.width)).join(' | ');
    const separator = cols.map(c => '-'.repeat(c.width)).join('-+-');

    // Build rows
    const rows = results.map(result => {
        return cols.map(col => {
            let value;
            if (col.format) {
                value = col.format(result);
            } else {
                value = String(result[col.key] || '');
            }
            return value.padEnd(col.width);
        }).join(' | ');
    });

    return [header, separator, ...rows].join('\n');
}

/**
 * Format results as JSON
 * @param {Array} results - Array of benchmark results
 * @param {Object} metadata - Optional metadata
 * @returns {string} JSON string
 */
export function formatResultsJSON(results, metadata = {}) {
    const output = {
        timestamp: new Date().toISOString(),
        platform: typeof process !== 'undefined' ? 'node' : 'browser',
        ...metadata,
        results: results
    };

    return JSON.stringify(output, null, 2);
}

/**
 * Format a single result as a one-line summary
 * @param {Object} result - Benchmark result
 * @returns {string} Summary string
 */
export function formatResultSummary(result) {
    return [
        `${result.imageSize} ${result.compression}:`,
        `enc=${result.encodeTimeMs.toFixed(2)}ms`,
        `dec=${result.decodeTimeMs.toFixed(2)}ms`,
        `size=${formatBytes(result.encodedSize)}`,
        `(${result.compressionRatio}%)`
    ].join(' ');
}

/**
 * Format results as CSV
 * @param {Array} results - Array of benchmark results
 * @returns {string} CSV string
 */
export function formatResultsCSV(results) {
    if (!results || results.length === 0) {
        return '';
    }

    const headers = [
        'imageSize', 'width', 'height', 'compression',
        'rawSize', 'encodedSize', 'compressionRatio',
        'encodeTimeMs', 'decodeTimeMs',
        'encodeThroughputMBps', 'decodeThroughputMBps'
    ];

    const rows = results.map(r =>
        headers.map(h => String(r[h] || '')).join(',')
    );

    return [headers.join(','), ...rows].join('\n');
}

/**
 * Generate HTML table from results (for browser)
 * @param {Array} results - Array of benchmark results
 * @returns {string} HTML table string
 */
export function formatResultsHTML(results) {
    if (!results || results.length === 0) {
        return '<tr><td colspan="8">No results</td></tr>';
    }

    return results.map(r => `
        <tr>
            <td>${r.imageSize}</td>
            <td>${r.compression}</td>
            <td>${formatBytes(r.encodedSize)}</td>
            <td>${r.compressionRatio}%</td>
            <td>${r.encodeTimeMs.toFixed(2)}</td>
            <td>${r.decodeTimeMs.toFixed(2)}</td>
            <td>${r.encodeThroughputMBps.toFixed(1)}</td>
            <td>${r.decodeThroughputMBps.toFixed(1)}</td>
        </tr>
    `).join('');
}
