"""PSD fixtures for the composite decoder: one per compression scheme.

The decoder was reworked to write straight into the destination image and to
stream ZIP a scanline at a time, so every scheme needs a file that proves the
pixels still come out identical.
"""
import sys, zlib, struct
from pathlib import Path
import numpy as np

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
W, H = 640, 400

x = np.arange(W, dtype=np.uint16)[None, :]
y = np.arange(H, dtype=np.uint16)[:, None]
planes = [
    ((x * 3 + y) & 255).astype(np.uint8) * np.ones((H, 1), np.uint8),
    ((x + y * 2) & 255).astype(np.uint8) * np.ones((H, 1), np.uint8),
    ((x // 3 + y // 2) & 255).astype(np.uint8) * np.ones((H, 1), np.uint8),
]
planes = [np.ascontiguousarray(np.broadcast_to(p, (H, W))) for p in planes]

def header(channels=3, depth=8, mode=3):
    b = b"8BPS" + struct.pack(">H", 1) + b"\0" * 6
    b += struct.pack(">HIIHH", channels, H, W, depth, mode)
    b += struct.pack(">I", 0)   # colour mode data
    b += struct.pack(">I", 0)   # image resources
    b += struct.pack(">I", 0)   # layer and mask
    return b

def packbits(row):
    out = bytearray(); i = 0; n = len(row)
    while i < n:
        run = 1
        while i + run < n and run < 128 and row[i + run] == row[i]:
            run += 1
        if run >= 3:
            out.append(256 - (run - 1)); out.append(row[i]); i += run
        else:
            start = i; i += 1
            while i < n and i - start < 128:
                if i + 2 < n and row[i] == row[i + 1] == row[i + 2]:
                    break
                i += 1
            length = i - start
            out.append(length - 1); out += row[start:i]
    return bytes(out)

def write(name, compression):
    body = b""
    if compression == 0:
        body = b"".join(p.tobytes() for p in planes)
    elif compression == 1:
        rows, counts = [], []
        for p in planes:
            for r in range(H):
                packed = packbits(p[r].tobytes())
                rows.append(packed); counts.append(len(packed))
        body = b"".join(struct.pack(">H", c) for c in counts) + b"".join(rows)
    elif compression in (2, 3):
        raw = [p.copy() for p in planes]
        if compression == 3:   # delta per scanline, which the decoder undoes
            for p in raw:
                p[:, 1:] = (p[:, 1:].astype(np.int16) - p[:, :-1].astype(np.int16)).astype(np.uint8)
        body = zlib.compress(b"".join(p.tobytes() for p in raw), 6)
    data = header() + struct.pack(">H", compression) + body
    (out / name).write_bytes(data)
    print(f"  {name:26} {len(data):>10,}  compression={compression}")

write("psd-raw.psd", 0)
write("psd-rle.psd", 1)
write("psd-zip.psd", 2)
write("psd-zip-prediction.psd", 3)

# A deliberately damaged document: the ZIP stream is cut in half.
damaged = (out / "psd-zip.psd").read_bytes()
(out / "psd-truncated.psd").write_bytes(damaged[: len(damaged) // 2])
print(f"  psd-truncated.psd          {len(damaged)//2:>10,}  (deliberately damaged)")

# The expected pixels, as a raw BGRA dump the decode probe can compare against.
rgba = np.zeros((H, W, 4), np.uint8)
rgba[:, :, 2] = planes[0]; rgba[:, :, 1] = planes[1]; rgba[:, :, 0] = planes[2]
rgba[:, :, 3] = 255
(out / "psd-expected.bgra").write_bytes(rgba.tobytes())
print("fixtures written to", out)
