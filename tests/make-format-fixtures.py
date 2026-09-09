"""Fixtures for the format decode pass: large PNG / WebP / AVIF / TIFF, and
PSD variants that carry Photoshop's own embedded preview.

Everything here is synthesised. The brief forbids publishing benchmarks made
from personal photographs, and a generated scene has the further advantage of
being reproducible on another machine.

The scene is built to behave like a photograph under a codec: broad smooth
gradients so the DC coefficients matter, fine high-frequency detail so the AC
coefficients do too, and grain so nothing compresses to nothing.

Usage:  python tests/make-format-fixtures.py tests/fixtures
"""
import struct, sys, zlib
from pathlib import Path

import numpy as np
from PIL import Image


def scene(width, height, seed=7):
    """A photograph-shaped image: gradients, edges, fine detail, grain."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    u, v = x / width, y / height

    # Broad tonal structure, the part a downsampler must preserve exactly.
    r = 120 + 90 * np.sin(6.0 * u + 1.1) * np.cos(2.5 * v)
    g = 130 + 80 * np.sin(3.0 * v + 0.4) + 25 * u
    b = 140 + 70 * np.cos(4.5 * u * v + 0.9)

    # Fine detail: concentric rings and diagonal hatching. These are what
    # alias into moire when a resampler drops rows instead of averaging them.
    radius = np.hypot(u - 0.5, v - 0.5)
    rings = 40 * np.sin(radius * min(width, height) * 0.35)
    hatch = 30 * np.sin((x + y) * 0.7) * (u > 0.55) * (v < 0.45)
    r, g, b = r + rings + hatch, g + rings * 0.7 + hatch, b + rings * 0.4

    # Hard edges, so a decoder that blurs is visible in a difference image.
    block = ((x.astype(np.int64) // 97 + y.astype(np.int64) // 89) % 2).astype(np.float32)
    mask = ((u > 0.08) & (u < 0.30) & (v > 0.62) & (v < 0.88)).astype(np.float32)
    for channel in (r, g, b):
        channel += mask * (block * 150 - 75)

    stack = np.stack([r, g, b], axis=-1)
    stack += rng.normal(0.0, 3.0, stack.shape).astype(np.float32)   # grain
    return np.clip(stack, 0, 255).astype(np.uint8)


def thumbnail_resource(rgb, longest=160):
    """Image Resource 1036: Photoshop's own preview, a JPEG behind a header."""
    from io import BytesIO
    image = Image.fromarray(rgb)
    image.thumbnail((longest, longest), Image.LANCZOS)
    buffer = BytesIO()
    image.save(buffer, "JPEG", quality=80)
    jpeg = buffer.getvalue()
    width, height = image.size
    # format 1 (kJpegRGB), then width, height, row bytes, total, compressed,
    # bit depth, planes -- the 28 byte descriptor the decoder skips over.
    header = struct.pack(">IIIIIIHH", 1, width, height, width * 4,
                         width * height * 3, len(jpeg), 24, 1)
    body = header + jpeg
    block = b"8BIM" + struct.pack(">H", 1036) + b"\0\0" + struct.pack(">I", len(body)) + body
    return block + (b"\0" if len(body) & 1 else b"")


def packbits(row):
    out = bytearray()
    i, n = 0, len(row)
    while i < n:
        run = 1
        while i + run < n and run < 128 and row[i + run] == row[i]:
            run += 1
        if run >= 3:
            out.append(256 - (run - 1))
            out.append(row[i])
            i += run
        else:
            start = i
            i += 1
            while i < n and i - start < 128:
                if i + 2 < n and row[i] == row[i + 1] == row[i + 2]:
                    break
                i += 1
            out.append(i - start - 1)
            out += row[start:i]
    return bytes(out)


def write_psd(path, rgb, compression, psb=False, preview=True):
    """A flattened Photoshop composite. compression: 0 raw, 1 RLE, 2 ZIP, 3 ZIP+delta."""
    height, width = rgb.shape[:2]
    version = 2 if psb else 1
    body = b"8BPS" + struct.pack(">H", version) + b"\0" * 6
    body += struct.pack(">HIIHH", 3, height, width, 8, 3)
    body += struct.pack(">I", 0)                              # colour mode data

    resources = thumbnail_resource(rgb) if preview else b""
    body += struct.pack(">I", len(resources)) + resources
    body += struct.pack(">Q" if psb else ">I", 0)             # layer and mask

    planes = [np.ascontiguousarray(rgb[:, :, c]) for c in range(3)]
    body += struct.pack(">H", compression)
    if compression == 0:
        for plane in planes:
            body += plane.tobytes()
    elif compression == 1:
        counts, payload = [], bytearray()
        for plane in planes:
            for y in range(height):
                packed = packbits(plane[y].tobytes())
                counts.append(len(packed))
                payload += packed
        table = b"".join(struct.pack(">I" if psb else ">H", c) for c in counts)
        body += table + bytes(payload)
    else:
        rows = bytearray()
        for plane in planes:
            data = plane.copy()
            if compression == 3:
                # The predictor stores each row as first-order differences.
                data = np.concatenate(
                    [data[:, :1], np.diff(data.astype(np.int16), axis=1).astype(np.uint8)],
                    axis=1)
            rows += np.ascontiguousarray(data).tobytes()
        body += zlib.compress(bytes(rows), 6)
    Path(path).write_bytes(body)
    return len(body)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures")
    common = out / "common"
    common.mkdir(parents=True, exist_ok=True)
    psd = out / "psd"
    psd.mkdir(parents=True, exist_ok=True)

    print("building the 24 MP scene...")
    big = scene(6000, 4000)
    small = scene(3000, 2000, seed=11)
    image = Image.fromarray(big)

    made = []

    def note(path):
        made.append((path.relative_to(out), path.stat().st_size))

    # ---- ordinary formats -------------------------------------------------
    print("PNG...")
    path = common / "large-24mp.png"
    image.save(path, "PNG", compress_level=6)
    note(path)
    path = common / "medium-6mp.png"
    Image.fromarray(small).save(path, "PNG", compress_level=6)
    note(path)

    print("WebP...")
    # libwebp refuses dimensions past 16383, so the WebP fixtures are 3000 px.
    path = common / "large-lossy.webp"
    Image.fromarray(small).save(path, "WEBP", quality=82, method=4)
    note(path)
    path = common / "large-lossless.webp"
    Image.fromarray(small).save(path, "WEBP", lossless=True, method=4)
    note(path)
    # Keep the alpha fixture larger than the 2560 px benchmark viewport so the
    # decoder-side scaled path, not the already-small shortcut, is exercised.
    alpha = np.dstack([small,
                       np.clip(np.mgrid[0:2000, 0:3000][1] * 255 // 3000, 0, 255).astype(np.uint8)])
    path = common / "large-alpha.webp"
    Image.fromarray(alpha, "RGBA").save(path, "WEBP", quality=85, method=4)
    note(path)

    print("AVIF...")
    try:
        path = common / "large-12mp.avif"
        Image.fromarray(small).save(path, "AVIF", quality=70)
        note(path)
    except Exception as error:                       # pragma: no cover
        print("  AVIF unavailable:", error)

    print("TIFF...")
    path = common / "large-24mp-uncompressed.tif"
    image.save(path, "TIFF", compression=None)
    note(path)
    path = common / "large-24mp-lzw.tif"
    image.save(path, "TIFF", compression="tiff_lzw")
    note(path)
    # A pyramidal TIFF: page 0 full, then halvings flagged NewSubfileType=1.
    # This is the classic reduced-resolution convention, which is what a WIC
    # decoder exposes as additional frames.
    path = common / "large-24mp-pyramid.tif"
    levels = [image]
    while max(levels[-1].size) > 400:
        levels.append(levels[-1].resize(
            (max(1, levels[-1].width // 2), max(1, levels[-1].height // 2)), Image.LANCZOS))
    levels[0].save(path, "TIFF", compression="tiff_lzw",
                   save_all=True, append_images=levels[1:])
    patch_reduced_resolution(path)
    note(path)

    print("JPEG (45 MP, for the decode-frame comparison)...")
    path = common / "large-45mp.jpg"
    Image.fromarray(scene(8256, 5504, seed=3)).save(path, "JPEG", quality=88)
    note(path)

    # ---- Photoshop --------------------------------------------------------
    print("PSD 24 MP, four compressions, with embedded preview...")
    for name, compression in (("raw", 0), ("rle", 1), ("zip", 2), ("zip-prediction", 3)):
        path = psd / f"preview-24mp-{name}.psd"
        write_psd(path, big, compression, preview=True)
        note(path)
    path = psd / "nopreview-24mp-rle.psd"
    write_psd(path, big, 1, preview=False)
    note(path)

    print("PSB 48 MP...")
    huge = scene(8000, 6000, seed=5)
    for name, compression in (("rle", 1), ("zip", 2)):
        path = psd / f"large-48mp-{name}.psb"
        write_psd(path, huge, compression, psb=True, preview=True)
        note(path)

    print()
    for name, size in made:
        print(f"  {str(name):44} {size / 1024 / 1024:8.1f} MB")


def patch_reduced_resolution(path):
    """Flag every page after the first as a reduced-resolution level.

    PIL writes multi-page TIFF but leaves NewSubfileType at 0, which claims
    each page is a separate image rather than a smaller copy of page one.
    """
    data = bytearray(Path(path).read_bytes())
    little = data[:2] == b"II"
    unpack = "<" if little else ">"
    offset = struct.unpack_from(unpack + "I", data, 4)[0]
    first = True
    while offset:
        count = struct.unpack_from(unpack + "H", data, offset)[0]
        for i in range(count):
            at = offset + 2 + i * 12
            tag = struct.unpack_from(unpack + "H", data, at)[0]
            if tag == 254 and not first:
                struct.pack_into(unpack + "I", data, at + 8, 1)
        offset = struct.unpack_from(unpack + "I", data, offset + 2 + count * 12)[0]
        first = False
    Path(path).write_bytes(bytes(data))


if __name__ == "__main__":
    main()
