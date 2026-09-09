"""A PSB fixture: Photoshop Large Document Format.

PSB differs from PSD in exactly the places the decoder branches on — version 2
in the header, 64-bit layer/mask length, and 32-bit RLE scanline counts — so a
synthetic one exercises the real difference rather than approximating it.
"""
import struct, sys, zlib
from pathlib import Path
import numpy as np

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
W, H = 800, 600
x = np.arange(W, dtype=np.uint16)[None, :]
y = np.arange(H, dtype=np.uint16)[:, None]
planes = [np.ascontiguousarray(np.broadcast_to(p, (H, W))) for p in (
    ((x * 3 + y) & 255).astype(np.uint8),
    ((x + y * 2) & 255).astype(np.uint8),
    ((x // 3 + y // 2) & 255).astype(np.uint8))]

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
            out.append(i - start - 1); out += row[start:i]
    return bytes(out)

def build(compression):
    b = b"8BPS" + struct.pack(">H", 2) + b"\0" * 6          # version 2 == PSB
    b += struct.pack(">HIIHH", 3, H, W, 8, 3)
    b += struct.pack(">I", 0)                                # colour mode data
    b += struct.pack(">I", 0)                                # image resources
    b += struct.pack(">Q", 0)                                # 64-bit layer/mask length
    b += struct.pack(">H", compression)
    if compression == 0:
        b += b"".join(p.tobytes() for p in planes)
    elif compression == 1:
        rows, counts = [], []
        for p in planes:
            for r in range(H):
                packed = packbits(p[r].tobytes())
                rows.append(packed); counts.append(len(packed))
        b += b"".join(struct.pack(">I", c) for c in counts)  # 32-bit counts in PSB
        b += b"".join(rows)
    else:
        b += zlib.compress(b"".join(p.tobytes() for p in planes), 6)
    return b

for name, comp in (("psb-raw.psb", 0), ("psb-rle.psb", 1), ("psb-zip.psb", 2)):
    (out / name).write_bytes(build(comp))
    print(f"  {name:16} {len((out/name).read_bytes()):>10,}  compression={comp}")

rgba = np.zeros((H, W, 4), np.uint8)
rgba[:, :, 2] = planes[0]; rgba[:, :, 1] = planes[1]; rgba[:, :, 0] = planes[2]
rgba[:, :, 3] = 255
print("expected first pixel BGRA:", list(rgba[0, 0]))
