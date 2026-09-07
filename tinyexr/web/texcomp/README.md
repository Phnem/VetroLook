# texcomp browser demo

Live GPU texture compression: **resize → compress → decompress → compare**, on
your own image, entirely in the browser.

**[▶ Open the demo](https://syoyo.github.io/tinyexr/texcomp/)**

One ~600 KB WebAssembly module links five pure-C11 pieces of TinyEXR:

| | |
|---|---|
| `src/` | TinyEXR v3 — EXR decode, so scene-linear HDR sources load directly |
| `tools/resize` | **tir** — content-aware resize |
| `tools/texcomp` | **texcomp** — BC1/3/5/6H/7, ETC2, EAC, ASTC LDR+HDR, uni |
| `tools/texpipe` | **texpipe** — mip chains, KTX2 / DDS containers |
| `tools/envmap` | **envmap** — equirect → cubemap / octahedral |

Nothing is uploaded. Every byte stays in the tab.

## Panels

- **Resize & compress** — pick a filter and a codec, see the decompressed result
  and the amplified error next to the original. Download a real `.ktx2` or `.dds`
  (mips and all) that a GPU tool will open.
- **HDR** — BC6H and ASTC HDR vs BC7 on a scene-linear EXR. Sweep the exposure and
  watch BC7's highlights go flat white: an 8-bit codec clips above 1.0 at *encode*
  time, so the data is gone, not merely quantised.
- **Normal maps** — ranks codecs by **mean angular error in degrees**, which is
  what actually matters for a normal map. BC5 beats BC7 despite storing fewer
  channels, because BC7 wastes bits on a blue channel that carries no information.
- **Cubemap / octahedral** — reproject an equirect latlong EXR and compress it
  with BC6H.
- **Mip chain** — texpipe's content-aware pyramid, including alpha-coverage
  preservation.

Use the **openexr-images…** button to pull real test images from
[AcademySoftwareFoundation/openexr-images](https://github.com/AcademySoftwareFoundation/openexr-images).

## Building

Needs the Emscripten SDK on PATH.

```sh
./build.sh              # baseline
SIMD=1 ./build.sh       # -msimd128; noticeably faster ASTC/BC7 encode
EMCC=/path/to/emcc ./build.sh
```

Or from the repo root: `make texcomp-web`.

Then serve **from the repo root** and open `/web/texcomp/`:

```sh
python3 -m http.server
```

`texcomp_web.mjs` and `texcomp_web.wasm` are committed so the demo can be served
straight from a checkout (same as `web/viewer`).

## The wasm API

`texcomp_web.c` exposes a small pipeline-shaped API (`tcw_*`) rather than
mirroring the C libraries one-for-one, so the JS side never has to shuttle big
buffers around: the module keeps a source image, a working image (after resize),
the last compressed payload and the last decode, and the browser just asks for
tonemapped previews of whichever it wants to look at.

See [`doc/texcomp.md`](../../doc/texcomp.md) for the library itself.
