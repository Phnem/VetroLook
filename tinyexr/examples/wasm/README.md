# TinyEXR v3 — WebAssembly (C API)

A minimal Emscripten build of the pure-C11 v3 EXR library. No filesystem, no
stdio — the core is freestanding and all I/O is in-memory.

## Build

```sh
# from the repo root, with emcc on PATH (emsdk activated)
make wasm
# -> build/exr_v3.mjs (ES6 module) + build/exr_v3.wasm
```

## Exports (`examples/wasm/exr_wasm.c`)

```c
float*   exrw_decode_rgba(const uint8_t* data, int size, int* w, int* h);
uint8_t* exrw_encode_rgba(const float* rgba, int w, int h, int comp,
                          int* out_size);
void     exrw_free(void* p);
```

- `exrw_decode_rgba` decodes part 0 of an EXR (any supported compression) into a
  freshly allocated `w*h*4` interleaved RGBA float buffer (missing channels
  default to RGB=0, A=1). Returns `NULL` on failure.
- `exrw_encode_rgba` encodes interleaved RGBA float into an EXR byte buffer
  (`comp` is an `exr_compression` value; 0=NONE, 3=ZIP, ...). Returns `NULL` on
  failure and writes the byte length to `*out_size`.
- Free either returned buffer with `exrw_free` (or `Module._free`).

`_malloc` / `_free` and the `HEAPU8` / `HEAPF32` / `HEAP32` views are exported so
JS can pass buffers across the boundary.

## Usage (Node / browser ES module)

```js
import createModule from './build/exr_v3.mjs';
const M = await createModule();

// decode
const bytes = /* Uint8Array of an .exr file */;
const inPtr = M._malloc(bytes.length);
M.HEAPU8.set(bytes, inPtr);
const wPtr = M._malloc(4), hPtr = M._malloc(4);
const rgbaPtr = M._exrw_decode_rgba(inPtr, bytes.length, wPtr, hPtr);
const w = M.HEAP32[wPtr >> 2], h = M.HEAP32[hPtr >> 2];
const rgba = M.HEAPF32.subarray(rgbaPtr >> 2, (rgbaPtr >> 2) + w * h * 4);
// ... use rgba ...
M._exrw_free(rgbaPtr); M._free(inPtr); M._free(wPtr); M._free(hPtr);
```

`examples/wasm/test.mjs` is a runnable Node smoke test (`node examples/wasm/test.mjs`).
