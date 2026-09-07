# tocio web demo — EXR + OCIO (ACES 2.0) on WebGL2

Loads an OpenEXR image, applies an OCIO colour transform (including the **ACES 2.0**
output transforms), and renders it on **WebGL2**. The OCIO config is editable in the
page: press **JIT compile** to build the processor and regenerate the GPU shader
live ("the JIT outputs GLSL"). Wide-gamut **Display-P3** output is used when the
device/browser supports it (sRGB fallback otherwise).

Everything is one self-contained WASM module (`tocio_demo.mjs` + `.wasm`) that
bundles the tinyexr v3 EXR decoder and the tocio OCIO engine. No external JS deps.

## Build

From the repo root (needs the Emscripten SDK; `EMCC` defaults to `emcc` on PATH):

```sh
make wasm-tocio-demo          # -> web/tocio/tocio_demo.mjs + .wasm
```

Regenerate the bundled synthetic test image (optional; it is committed):

```sh
cc -Iinclude -Isrc -Ideps/zstd -std=c11 -O2 web/tocio/gen_scene_linear.c \
   $(ls src/exr_*.c | grep -vE 'exr_freestanding|exr_spectral') \
   deps/zstd/tinyexr_zstd.c -lm -o build/gen_scene_linear
./build/gen_scene_linear web/tocio/scene_linear.exr
```

## Run

Serve over HTTP (ES modules + WASM need a server, not `file://`):

```sh
# from the repo root
python3 -m http.server 8000
# open http://localhost:8000/web/tocio/
```

On load it shows a bundled synthetic scene-linear (ACEScg) EXR rendered through the
ACES 2.0 SDR view. Then you can:

- **Open .exr** or **drag-and-drop** any EXR onto the viewport.
- Pick the **input colour space**, **display**, and **view** (populated from the
  config). The default config offers ACEScg/ACEScct/ACES2065-1 inputs and the
  ACES 2.0 SDR view for sRGB and Display-P3.
- Adjust **exposure** (applied in scene-linear before the transform).
- Toggle **wide-gamut (Display-P3)** output where supported.
- Edit the **OCIO config** and press **JIT compile** to rebuild the GPU shader.

### Loading from openexr-images

The sample dropdown includes a few `openexr-images` paths. Those are resolved
relative to the server root, so serve a directory that contains both this repo and
the `openexr-images` corpus as siblings, e.g.:

```sh
# if /work/tinyexr-tocio and /work/openexr-images are siblings:
cd /work && python3 -m http.server 8000
# open http://localhost:8000/tinyexr-tocio/web/tocio/
```

Otherwise just drag-and-drop a file.

## How it works

1. `exrw_decode_rgba` decodes the EXR bytes to interleaved float RGBA (uploaded to
   an `RGBA32F` texture).
2. `tocw_parse` + `tocw_processor_view` build a flat op list for
   `input → display/view`. For the ACES 2.0 views this expands to the CAM16 JMh
   tonescale + gamut-compression output transform and the display encoding.
3. `tocw_jit_glsl` emits a GLSL ES 3.00 `OCIOMain(vec4)` (with the ACES 2.0 cusp/
   reach tables baked in as constants — no LUT textures needed). On WASM the
   machine-code JIT is unavailable, so the JIT's output is this shader.
4. The fragment shader samples the float texture, applies exposure, calls
   `OCIOMain`, and writes the display-encoded result.

Transforms tocio does not implement (some camera/utility builtins) report
"unsupported"; pipelines that need LUT textures fall back to a one-shot CPU apply.

The ACES 2.0 math is validated against OpenColorIO — see
`sandbox/tocio/tests/VALIDATION.md` and `make tocio-validate`.

## Files

| File | Role |
|------|------|
| `index.html`, `style.css`, `app.js` | the demo UI + WebGL2 renderer |
| `default.ocio` | editable starter config (ACES 2.0 SDR for sRGB / Display-P3) |
| `gen_scene_linear.c` | generator for the bundled synthetic test EXR |
| `scene_linear.exr` | bundled synthetic scene-linear (ACEScg) test image |
| `tocio_demo.mjs`, `.wasm` | combined EXR-decoder + tocio WASM module (built) |
