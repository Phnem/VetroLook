# Dependency and licence audit

Vetro Look is distributed under **GPL-3.0-or-later**. Every dependency below was
checked for licence compatibility with that, for what it costs to build on
Windows, and for whether it earns its place — a library added for one field
that existing code already reads reliably is a liability, not an asset.

Binary size is measured on the real Release build: `dist/VetroLook.exe`.

| Before this pass | After this pass | Delta |
|---|---|---|
| 3 674 624 bytes (3.50 MB) | 6 410 752 bytes (6.11 MB) | **+2.61 MB** |

Plus 5.2 MB of Lensfun XML installed beside the executable (see below).

---

## Added

### Exiv2 0.28.7 — **adopted**

* **Licence:** GPL-2.0-or-later. Compatible with GPL-3.0-or-later: the "or
  later" clause lets it be combined into a GPLv3 work. The combined binary is
  GPLv3.
* **Linking:** static. Building `exiv2.lib` (26.6 MB unstripped) and letting
  `/OPT:REF` discard what is unreachable is what the +2.61 MB above reflects,
  together with expat and the SIMD kernels.
* **Transitive dependencies:** expat only, and only because XMP needs it. PNG
  support (zlib), BMFF/AVIF containers (brotli), video, `inih` and NLS are all
  turned **off** — the application has its own decoders for those formats and
  only wants Exiv2 for metadata.
* **Windows build:** straightforward with the bundled CMake from Visual Studio
  Build Tools. No package manager, no MSYS2. Configured and built as an
  `ExternalProject`, matching how libjpeg-turbo is already handled.
* **Why it earns its place:** EXIF, IPTC, XMP and MakerNotes from one reader,
  and `easyaccess` knows where each manufacturer hides the lens name — a Nikon
  Z lens lives in the MakerNote, not in `Exif.Photo.LensModel`. The previous
  parser (`third_party/exif.cpp`, easyexif) read none of IPTC, XMP or
  MakerNotes. This is not one field.
* **NOTICE requirements:** GPL-2.0 requires the licence text and the source
  offer, both already satisfied by the project's own GPL distribution terms.
  Recorded in `THIRD_PARTY_NOTICES.txt`.

### Adobe XMP Toolkit SDK — **adopted, through Exiv2**

* **Licence:** BSD-3-Clause. Compatible.
* Exiv2 vendors a trimmed XMP Toolkit in `exiv2/xmpsdk` (1.1 MB of source) and
  builds it as `exiv2-xmp.lib` when `EXIV2_ENABLE_XMP=ON`. Integrating the
  upstream SDK separately would mean two copies of the same code in one binary,
  so the vendored one is used and no separate dependency was added.
* **Attribution is still required** and is recorded in
  `THIRD_PARTY_NOTICES.txt` under its own heading, not folded into Exiv2's.

### libexpat 2.7.1 — **adopted (required by XMP)**

* **Licence:** MIT. Compatible.
* Static, ~200 KB of object code. Built with `XML_STATIC` so its headers do not
  declare `dllimport`.
* Also used directly by `src/lensdb.cpp`, which needs an XML parser anyway — so
  it serves two purposes rather than one.

### NASM 2.16.03 — **build tool, not a dependency**

* Not linked and not distributed. It assembles libjpeg-turbo's SIMD kernels.
* `build.ps1` fetches it; if that fails the build **stops with an explanation**
  rather than quietly producing a scalar libjpeg-turbo. `VETRO_ALLOW_SCALAR_JPEG=1`
  opts into the slow build deliberately.

### Lensfun — **library rejected, database adopted**

* **Library licence:** LGPL-3.0. Compatible in principle.
* **Rejected because of its build footprint.** `lensfun/CMakeLists.txt` has
  `FIND_PACKAGE(GLIB2 REQUIRED)` — GLib is not optional. GLib on Windows means
  vcpkg or MSYS2, a second package manager in a tree that currently vendors
  only self-contained sources, and a runtime DLL beside the executable. That is
  a disproportionate price for lens identification.
* **Database adopted.** `lensfun/data/db` is licensed **CC-BY-SA 3.0**,
  separately from the library, and is redistributable with attribution. It is
  vendored at `third_party/lensfun-db/` (5.2 MB, 59 XML files, 1567 lenses and
  1051 camera bodies) and installed beside the executable.
* `src/lensdb.cpp` reads it with expat and implements Lensfun's own published
  correction models (`ptlens`/`poly3`/`poly5` distortion, `pa` vignetting,
  `poly3` transverse chromatic aberration), interpolated across focal length.
  No GLib, no DLL, and the same calibration data the library would have used.
* **CC-BY-SA 3.0 obligations:** attribution and share-alike on the database
  itself. It is shipped unmodified, with `COPYING.CC_BY-SA_3.0` alongside it,
  and credited in `THIRD_PARTY_NOTICES.txt`.

---

## Considered and not added

| Library | Why not |
|---|---|
| **OpenImageIO** | Pulls in Boost, OpenEXR, libtiff, libpng, and a plugin architecture. The viewer already decodes every format it supports. Explicitly out of scope. |
| **libvips** | GLib again, plus a threaded pipeline model that does not match a single-image viewer. |
| **GEGL** | An editing graph framework for an application with five drawing tools. |
| **zlib (for Exiv2 PNG)** | Exiv2's PNG support is only needed to read metadata out of PNG, which the Windows Imaging Component already does here. Left off. |
| **Brotli (for Exiv2 BMFF)** | BMFF is disabled; libavif and dav1d decode AVIF, and `Exiv2::enableBMFF(false)` is set explicitly. |

---

## Existing dependencies, unchanged

libjpeg-turbo (BSD-3-Clause + IJG), LibRaw (LGPL-2.1/CDDL), libwebp
(BSD-3-Clause), libavif (BSD-2-Clause), dav1d (BSD-2-Clause), TinyEXR
(BSD-3-Clause), miniz (MIT), Wuffs (Apache-2.0/MIT), easyexif (BSD-2-Clause).

`third_party/exif.cpp` (easyexif) is **retained**, not deleted: `src/decoders.cpp`
reads the EXIF orientation from it during a JPEG decode, on the decode worker,
without needing Exiv2's much larger machinery in that hot path. See
"What stayed on the old parser" in `REPORT.md`.

---

## Files to keep in step

* `THIRD_PARTY_NOTICES.txt` — one entry per redistributed component.
* `CMakeLists.txt` — the `ExternalProject_Add` calls for expat and Exiv2, the
  `find_program(VETRO_NASM ...)` probe, and the post-build copy of
  `third_party/lensfun-db`.
* `build.ps1` — NASM acquisition and the SIMD report.
* `build-packages.ps1` / `packaging/` — the installer must carry `lensfun-db/`
  beside `VetroLook.exe`, or lens matching silently reports "not installed".
