# TinyEXR v3 — WASM EXR Viewer

A self-contained browser viewer built on the **pure-C11 v3** TinyEXR API
(`include/exr.h`). It decodes EXR files entirely in WebAssembly and renders them
with WebGL2, with live HDR tone-mapping. This is a fresh example and does not use
the legacy v1 embind sample in `experimental/js/`.

## Features

- **Drag-and-drop** or **Open .exr** upload.
- **OpenEXR sample-image browser** — the *Browse OpenEXR images…* button opens a
  collapsible tree of the official
  [openexr-images](https://github.com/AcademySoftwareFoundation/openexr-images)
  library (one GitHub tree-API call lists all ~100 `.exr`; type to filter). Each
  row shows a **thumbnail** (the repo's sibling `.jpg` preview, lazy-loaded on
  expand). Click any image to fetch it over HTTP (from `raw.githubusercontent.com`,
  with download progress) and decode it — a quick way to exercise every codec,
  tiling mode, multipart, and multiview file.
- **Load progress** driven by the v3 *streaming block API*
  (`exr_reader_num_blocks` / `exr_reader_block_info` / `exr_reader_decode_block`):
  the file is decoded one scanline-block / tile at a time and the progress bar
  reflects blocks decoded.
- **WebGL2 rendering** with GPU tone-mapping — **exposure** (EV), **gamma /
  sRGB** curve, and **channel isolation** (RGB / R / G / B / A / luminance) are
  all instant (no re-decode). A single isolated channel can be shown as a
  **false-color** (viridis) heatmap, with a colormap legend.
- **Non-RGB parts** — parts/images without conventional `R`/`G`/`B` still render
  instead of showing black: a `Y` (luminance) channel maps to grayscale
  (multiview / luminance-chroma; subsampled `RY`/`BY` chroma is not
  reconstructed), a single data channel (depth `Z`, mask) is replicated to
  grayscale, and multi-channel data (motion vectors, disparity) maps its leading
  channels to R/G/B. Pair with **false color** to read depth/data parts.
- **Zoom / pan** — wheel zooms to the cursor; drag pans; *Fit* / *1:1* buttons.
- **Region windows** — the **display window** is drawn as an overlay over the
  **data window**, with a *crop to display window* toggle; **shift-drag** marks a
  region of interest (coordinates shown, *Zoom region* button).
- **Pixel picker** — hover to read the raw float R/G/B/A at a pixel (and its
  absolute data-window coordinates).
- **Header / info panel** — file name, part type, compression, line order,
  data/display windows, pixel aspect, and the channel list.
- **Deep images in 3D** — deep (deepscanline/deeptiled) parts are shown as a
  **3D point cloud**: every deep sample is placed at its pixel (x, y) and its `Z`
  (depth), colored by depth (viridis) or by RGB. Orbit (drag), zoom (wheel), pan
  (shift-/right-drag); point size, exposure, and a reset-view control. The
  **Deep scanline (3D)** and **Deep tiled (3D)** buttons load the repo's
  `data/deepscanline.exr` and `data/deep_tiled_sample.exr` over HTTP. (Decoded
  via the v3 high-level `exr_load_from_memory`, which fills the per-part deep
  sample arrays; deep scanline and deep tiled are both supported.)
- **Part / mip / channel-view selectors** — switch parts in a multipart EXR,
  mip/ripmap levels in a tiled EXR, and the **channel view** (auto RGB(A), named
  layers like `diffuse.{R,G,B}`, or any single channel as grayscale) for parts
  with non-standard channel layouts. Switching any selector clears the canvas
  immediately so the previous image never lingers during the new decode.

All v3 decode codecs are supported (ZIP/ZIPS, RLE, PIZ, PXR24, B44/B44A, ZSTD,
HTJ2K). Deep parts are detected and reported as unsupported by this viewer.

> Note: custom/standard EXR attributes (`exr_header.attrs`) are opaque in the
> public v3 API with no iteration entry point, so the info panel shows the
> structured header fields and channels only.

## Live demo

This viewer is deployed to GitHub Pages from the `release` branch by
[`.github/workflows/gh-pages.yml`](../../.github/workflows/gh-pages.yml):

➡️ <https://syoyo.github.io/tinyexr/>

The deploy is a plain static-file upload — the generated `exr_viewer.mjs`,
`exr_viewer.wasm`, and a small `sample.exr` are committed to the repo, so no
Emscripten build runs in CI. After changing the C binding or UI, rebuild
locally (below) and commit the regenerated `.mjs` / `.wasm`.

## Build

Requires the [Emscripten SDK](https://emscripten.org/) on `PATH`
(`emcmake`, `emcc`) and CMake ≥ 3.13.

```sh
./build.sh
```

This runs an Emscripten CMake build (`MinSizeRel` + `-Oz`) and copies
`exr_viewer.mjs` + `exr_viewer.wasm` next to `index.html`. Equivalent manual
steps:

```sh
emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build
cp build/exr_viewer.mjs build/exr_viewer.wasm .
```

## Run

ES6 modules and `.wasm` must be served over HTTP (not `file://`):

```sh
python3 -m http.server
# open http://localhost:8000/
```

Then drag an `.exr` onto the window, or click **Open .exr**.

The **Sample** button fetches `sample.exr` from this folder; the **Asakusa**
button fetches the repo's `asakusa.exr` over HTTP from
`raw.githubusercontent.com` (a demo of loading from a remote URL, with download
progress). The committed `sample.exr` is a small HDR test pattern produced by
`gen_sample.c`:

```sh
c++ -x c++ -DTINYEXR_USE_MINIZ=0 -DTINYEXR_USE_STB_ZLIB=0 -include zlib.h \
    gen_sample.c -o gen_sample -lz -lm
./gen_sample          # writes sample.exr
```

To use the repo's full-size test image instead:

```sh
cp ../../asakusa.exr sample.exr
```

## Files

| File | Purpose |
|------|---------|
| `exr_viewer_wasm.c` | C binding: a session over the v3 streaming block API (open, header JSON, select part/level, per-block decode, RGBA buffer). |
| `CMakeLists.txt` | Emscripten build (MinSizeRel + `-Oz`), exports + ES6 module. |
| `build.sh` | Convenience wrapper around `emcmake cmake` + copy. |
| `index.html`, `style.css`, `viewer.js` | The WebGL2 viewer UI. |
| `exr_viewer.mjs`, `exr_viewer.wasm` | Committed build outputs served by GitHub Pages. |
| `gen_sample.c`, `sample.exr` | One-off HDR test-pattern generator and its output. |
