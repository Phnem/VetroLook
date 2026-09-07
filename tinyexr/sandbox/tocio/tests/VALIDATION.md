# tocio validation against the real ACES OCIO configs

This harness validates the `tocio` engine against the actual
[AcademySoftwareFoundation ACES OCIO configs](https://github.com/AcademySoftwareFoundation/OpenColorIO-Config-ACES),
using [OpenColorIO](https://github.com/AcademySoftwareFoundation/OpenColorIO)
itself (via the `PyOpenColorIO` C++ reference engine) as the numerical oracle.

## Layout

| Path | What | Committed? |
|------|------|------------|
| `scripts/fetch_ocio_ref.sh` | downloads configs + OCIO/ACES repos into `ref/` | yes |
| `ref/` | fetched configs, repos, and the isolated PyOCIO install | **no** (gitignored) |
| `scripts/gen_golden.py` | applies real transforms with PyOCIO → golden TSV | yes |
| `tests/golden/aces_golden.tsv` | reference outputs for fixed input samples | yes |
| `tests/toc_validate.c` | parses configs, builds transforms, compares to golden | yes |

Only `gen_golden.py` needs PyOpenColorIO. The golden TSV is committed, so the
validation itself (`tests/toc_validate.c`) runs oracle-free.

## Running

```sh
make tocio-fetch-ref     # one-time: download configs into ref/ (needs network)
make tocio-validate      # build + run the harness against ref/configs
```

`tocio-validate` parses each config, builds every colorspace conversion and
display/view transform that has a golden row, applies the input samples, and
compares to the reference. It classifies each transform as **OK** (built and
verified), **UNSUPPORTED**/**NOT-FOUND** (a feature tocio doesn't implement —
reported, not a failure), or **ERROR** (an unexpected build failure — a real
bug). Sample comparisons are split into **core** (inputs in `[0,1]`) and
**extended-domain** (negative or `>1` inputs).

### Regenerating the golden values

The fetch script installs PyOpenColorIO into an isolated, gitignored location
(`ref/.pyoracle`). To rebuild the golden TSV after changing the samples or
configs:

```sh
make tocio-gen-golden
```

## Gate policy

`make tocio-validate` **fails** (exit 1) on:
- a config that fails to parse,
- an unexpected build **ERROR**, or
- a wrong result on a **core** (`[0,1]`) input.

It **reports but tolerates** extended-domain differences (see below) and
coverage gaps. Run with `TOC_VALIDATE_STRICT=1` to fail on extended-domain
differences too.

## Bugs this harness found (and fixed)

Running tocio against the real configs immediately surfaced five issues that the
small hand-written unit-test configs never exercised:

1. **YAML parser hang** on a flow sequence containing a colon-bearing scalar,
   e.g. `aliases: [a, b, ocio:c]` (the `ocio:`-namespaced aliases are pervasive
   in real configs). `flow_scalar_end` treated `:` as a delimiter unconditionally
   and the flow loop then spun without advancing. Fixed in `toc_yaml.c`
   (`:` separates only when followed by whitespace/flow-delimiter/end) plus a
   forward-progress guard so the parser can never hang on any input.
2. **`view_transforms` ignored** — tocio read the singular key `view_transform`,
   but the OCIO v2 spec and every real config use plural `view_transforms`.
   Fixed in `toc_config.c` (accept both).
3. **Display colorspaces silently became identity** — `toc_cfg_cs_transform`
   knew `to/from_scene_reference` but not `to/from_display_reference`, so display
   colorspaces produced wrong (near-identity) output instead of reporting their
   unimplemented builtin. Fixed in `toc_config.c`.
4. **`LogCameraTransform` parse error** on every camera-log colorspace (ARRI,
   Sony S-Log3, RED, BMD, Panasonic V-Log, DJI D-Log). tocio read the CLF/XML
   camelCase keys (`linSideBreak`); OCIO YAML uses snake_case (`lin_side_break`).
   Fixed in `toc_processor.c` (accept both).
5. **Data colorspaces not passed through** — a conversion touching an
   `isdata: true` colorspace (e.g. the `Raw` view) must be identity, but tocio
   still applied the non-data side's reference transform. Fixed in
   `toc_processor.c`.

It also made tocio **honest** about the scene↔display reference crossing: a plain
colorspace→colorspace request that bridges the scene and display reference needs
a view transform tocio can't synthesize, so it now returns `UNSUPPORTED` instead
of a silently-wrong matrix.

## Known limitations (reported, not failed)

- **ACES 2.0 output transforms** and other OCIO `BuiltinTransform` styles tocio
  doesn't implement → reported as UNSUPPORTED/NOT-FOUND. This is the bulk of the
  coverage gap on the v4 configs (which are built around ACES 2.0).
- **Negative-input handling**: tocio clamps negative inputs to 0 in the
  exponent/MonCurve ops, while OCIO mirror-extends (signed power). Surfaces as
  extended-domain differences on negative samples.
- **Out-of-[0,1] extrapolation** of inverse-log curves (feeding code values past
  the curve domain) diverges from OCIO; both extrapolate, differently.
