# TinyEXR v3 parse test foundation

Parse/load every `*.exr` under a directory tree (default: `~/work/openexr-images`)
through the pure-C11 v3 public API (`include/exr.h`) and classify each file.

## Pieces

- **`parse_harness.c`** — a tiny C program linked against `build/libtinyexr3.a`.
  For one file it prints machine-readable lines:

  ```
  FILE <path>
  HEADER_RESULT <code> <string>
  PARTS <n>
  PART <i> type=<scanline|tiled|deep_scanline|deep_tiled> compression=<NAME> channels=<n> size=<w>x<h>
  LOAD_RESULT <code> <string>
  ```

  It parses headers first (so the per-part compression is reported even when the
  full decode fails), then attempts a full `exr_load_from_memory()`. Exit 0 iff
  the load succeeded.

- **`parse-tester.py`** — stdlib-only Python driver. Globs `*.exr`, runs the
  harness on each, and buckets results:

  | Status  | Meaning                                                              |
  |---------|----------------------------------------------------------------------|
  | `PASS`  | Full load succeeded.                                                  |
  | `XFAIL` | Load failed, but the failure is **expected**. Currently DWA only.    |
  | `XPASS` | A file we expected to fail actually loaded (stale expectation).      |
  | `FAIL`  | Load failed and the failure is **not** expected — reported in detail.|

  Expected failures are driven by `EXPECTED_FAIL_COMPRESSIONS = {DWAA, DWAB}`.
  v3 intentionally does not implement the lossy DCT DWA codecs (see `AGENTS.md`),
  so any part using DWAA/DWAB is classified `XFAIL`.

## Usage

```sh
# Build the harness (also builds build/libtinyexr3.a):
test/v3/parse-tester.py --build

# Run over the default corpus (~/work/openexr-images):
test/v3/parse-tester.py

# Or point at any tree / subset:
test/v3/parse-tester.py ~/work/openexr-images/ScanLines
test/v3/parse-tester.py -v --progress ~/work/openexr-images   # list every file + live progress
test/v3/parse-tester.py --timeout 600 ~/work/openexr-images    # per-file timeout (s)
```

Exit status is non-zero if there is any `FAIL` or `XPASS`.

## Notes

- A full single-threaded sweep of the 299-file corpus takes ~35s; `-j` (default:
  all cores) speeds it up further. Use `--progress` to watch and point the tester
  at a single subdirectory for quick iteration.
- Building the harness by hand:

  ```sh
  make lib
  cc -std=c11 -Iinclude -O2 test/v3/parse_harness.c build/libtinyexr3.a -lm \
     -o test/v3/parse_harness
  ```
