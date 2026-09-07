#!/usr/bin/env python3
"""Generate deterministic malformed TinyEXR v3 regression inputs.

Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
SPDX-License-Identifier: BSD-3-Clause
"""

import pathlib
import struct
import sys

OUT = pathlib.Path(sys.argv[1])
OUT.mkdir(parents=True, exist_ok=True)


def attr(name, kind, payload):
    return (name.encode() + b"\0" + kind.encode() + b"\0" +
            struct.pack("<i", len(payload)) + payload)


def chlist(channels):
    out = b""
    for name, pixel_type, xs, ys in channels:
        out += name.encode() + b"\0" + struct.pack(
            "<iB3xii", pixel_type, 0, xs, ys)
    return out + b"\0"


def header(attrs, flags=0):
    return struct.pack("<II", 0x01312F76, 2 | flags) + b"".join(attrs) + b"\0"


def box(name, values):
    return attr(name, "box2i", struct.pack("<4i", *values))


def base(channels=(("R", 1, 1, 1),), window=(0, 0, 7, 7), extra=()):
    return [attr("channels", "chlist", chlist(channels)),
            attr("compression", "compression", b"\0"),
            box("dataWindow", window), box("displayWindow", window),
            attr("lineOrder", "lineOrder", b"\0"), *extra]


cases = []
for tx in (0x7FFFFFFF, 0x40000000, 0x00FFFFFF):
    tile = attr("tiles", "tiledesc", struct.pack("<IIB", tx, 1, 0))
    cases.append((f"tile-x-{tx:08x}", header(base(extra=(tile,)), 1 << 9)))
for tx, ty in ((24, 24), (100, 4), (4, 100)):
    tile = attr("tiles", "tiledesc", struct.pack("<IIB", tx, ty, 0))
    cases.append((f"tile-nonpow2-{tx}-{ty}", header(base(extra=(tile,)), 1 << 9)))
for mode in (1, 2):
    tile = attr("tiles", "tiledesc", struct.pack("<IIB", 1, 1, mode))
    cases.append((f"tile-level-mode-{mode}",
                  header(base(window=(0, 0, 1023, 1023), extra=(tile,)), 1 << 9)))
tile = attr("tiles", "tiledesc", struct.pack("<IIB", 1, 1, 0))
cases.append(("tile-1x1-big", header(base(window=(0, 0, 4095, 4095), extra=(tile,)), 1 << 9)))
for count in (0, 1, 0x7FFFFFFF, -1, 3):
    cases.append((f"chunk-count-{count & 0xffffffff:08x}",
                  header(base(extra=(attr("chunkCount", "int",
                                          struct.pack("<i", count)),)))))
for name, win in (("dims-million", (0, 0, (1 << 20) - 1, (1 << 20) - 1)),
                  ("dims-seven-gpx", (1, 11008, 130820, 65280))):
    channels = (("R", 1, 1, 1), ("G", 1, 1, 1), ("B", 1, 1, 1))
    cases.append((name, header(base(channels=channels, window=win))))
for name, samp in (("sampling-huge", (1 << 30, 1 << 30)),
                   ("sampling-zero", (0, 0)), ("sampling-negative", (-1, -3))):
    cases.append((name, header(base(channels=(("R", 1, *samp),)))))
for name, win in (("window-wrapped", (-2147483647, -2147483647,
                                       2147483647, 2147483647)),
                  ("window-max", (2147483647, 0, 2147483647, 0)),
                  ("window-negative", (-100, -100, 99, 99))):
    cases.append((name, header(base(window=win))))
for value in (12, 13, 255):
    attrs = base()
    attrs[1] = attr("compression", "compression", bytes((value,)))
    cases.append((f"compression-{value}", header(attrs)))
cases.extend([
    ("attr-channels-empty", header([attr("channels", "chlist", b""), *base()[1:]])),
    ("attr-box-short", header([base()[0], base()[1],
                               attr("dataWindow", "box2i", b"\0" * 8), *base()[3:]])),
    ("attr-name-255", header(base(extra=(attr("name", "string", b"N" * 255),)))),
    ("attr-name-300", header(base(extra=(attr("name", "string", b"N" * 300),)))),
    ("attr-duplicate-channels", header([base()[0], base()[0], *base()[1:]])),
    ("multipart-zero", header([], 1 << 12)),
    ("multipart-tiled-mismatch", header(base(), (1 << 12) | (1 << 9))),
    ("multipart-deep-mismatch", header(base(), (1 << 12) | (1 << 11))),
    ("multipart-type-mismatch", header(base(extra=(attr("type", "string",
                                                          b"deepscanline"),)), 1 << 12)),
])
complete = header(base()) + struct.pack("<Q", 200) + b"\0" * 32
for cut in (0, 4, 8, 9, 20, 30, len(complete) - 1):
    cases.append((f"truncated-{cut:03d}", complete[:cut]))
for version in (0, 1, 2, 3, 0xFFFFFFFF, 0x02000002, 0x02000802):
    payload = struct.pack("<II", 0x01312F76, version) + b"".join(base()) + b"\0"
    cases.append((f"version-{version:08x}", payload))

assert len(cases) == 48
for name, payload in cases:
    (OUT / name).write_bytes(payload)
