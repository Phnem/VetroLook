# attic/

This directory holds the **previous TinyEXR v3 C/C++ implementation**, retired in
favour of the pure-C11 rewrite under `include/exr.h` + `src/`.

It also holds the retired experimental TinyEXR v2 API. Everything in this
directory is kept for reference only; it is no longer built, tested, or
maintained as part of the active tree.

The old v3 was a Vulkan-style command-buffer API whose `tinyexr_c_impl.c` was
actually compiled as C++ (it depended on `tinyexr_piz.hh`,
`tinyexr_huffman.hh`, `tinyexr_simd.hh`, `tinyexr_v2_impl.hh`) and delegated
PIZ decoding to C++.

Notable salvage references used by the rewrite:

- `tinyexr_deflate.h`     - pure-C11 inflate (decode) ported into `src/exr_deflate.c`
- `tinyexr_b44.h`         - B44/B44A tables ported into `src/exr_b44.c`
- `tinyexr_c_impl.c`      - predictor/reorder, RLE, scalar PIZ huffman + wav2,
                            half<->float tables (ported into the new codecs)
- `tinyexr_c.h`           - the old (now-superseded) public API

The legacy single-header v1 library (`tinyexr.h`, `tinyexr.cc`) remains at the
repository root and is untouched by the rewrite.
