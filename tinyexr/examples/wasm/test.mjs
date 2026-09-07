// Node smoke test for the TinyEXR v3 WASM build.
//   make wasm && node examples/wasm/test.mjs
import createModule from '../../build/exr_v3.mjs';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const M = await createModule();

// ---- decode asakusa.exr ----
const bytes = new Uint8Array(readFileSync(join(root, 'asakusa.exr')));
const inPtr = M._malloc(bytes.length);
M.HEAPU8.set(bytes, inPtr);
const wPtr = M._malloc(4), hPtr = M._malloc(4);
const rgbaPtr = M._exrw_decode_rgba(inPtr, bytes.length, wPtr, hPtr);
if (!rgbaPtr) { console.error('FAIL: decode returned null'); process.exit(1); }
const w = M.HEAP32[wPtr >> 2], h = M.HEAP32[hPtr >> 2];
console.log(`decoded asakusa.exr: ${w}x${h}`);
if (w !== 660 || h !== 440) { console.error('FAIL: unexpected dims'); process.exit(1); }

// ---- re-encode (ZIP=3) and check round-trip dims ----
const szPtr = M._malloc(4);
const outPtr = M._exrw_encode_rgba(rgbaPtr, w, h, 3, szPtr);
const sz = M.HEAP32[szPtr >> 2];
if (!outPtr || sz <= 0) { console.error('FAIL: encode'); process.exit(1); }
console.log(`re-encoded EXR: ${sz} bytes`);

// decode the re-encoded buffer to confirm it is valid
const inPtr2 = M._malloc(sz);
M.HEAPU8.copyWithin(inPtr2, outPtr, outPtr + sz);
const w2Ptr = M._malloc(4), h2Ptr = M._malloc(4);
const rgba2 = M._exrw_decode_rgba(inPtr2, sz, w2Ptr, h2Ptr);
const w2 = M.HEAP32[w2Ptr >> 2], h2 = M.HEAP32[h2Ptr >> 2];
const ok = rgba2 && w2 === w && h2 === h;
console.log(`${ok ? 'ok' : 'FAIL'}: round-trip ${w2}x${h2}`);

M._exrw_free(rgbaPtr); M._exrw_free(outPtr); M._exrw_free(rgba2);
M._free(inPtr); M._free(inPtr2);
M._free(wPtr); M._free(hPtr); M._free(szPtr); M._free(w2Ptr); M._free(h2Ptr);
process.exit(ok ? 0 : 1);
