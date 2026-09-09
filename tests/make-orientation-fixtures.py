"""One JPEG per EXIF orientation, from a deliberately asymmetric source.

The source has a distinct colour in each corner and a bar along one edge, so
any of the eight transforms — including the four mirrored ones, which a
rotate-only implementation gets wrong in a way a symmetric test never notices —
produces a different image.
"""
import sys
from pathlib import Path
import numpy as np
from PIL import Image
import piexif

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
W, H = 160, 100
rgb = np.zeros((H, W, 3), np.uint8)
rgb[:, :, 2] = 40
rgb[: H // 2, : W // 2] = (220, 30, 30)      # top-left    red
rgb[: H // 2, W // 2:] = (30, 200, 30)       # top-right   green
rgb[H // 2:, : W // 2] = (30, 60, 230)       # bottom-left blue
rgb[H // 2:, W // 2:] = (230, 210, 40)       # bottom-right yellow
rgb[:6, :] = (255, 255, 255)                 # a white bar along the top edge
rgb[:, :4] = (0, 0, 0)                       # a black bar down the left edge
base = Image.fromarray(rgb, "RGB")
base.save(out / "orient-source.png")

for orientation in range(1, 9):
    exif = piexif.dump({"0th": {piexif.ImageIFD.Orientation: orientation},
                        "Exif": {}, "GPS": {}, "1st": {}, "thumbnail": None})
    base.save(out / f"orient-{orientation}.jpg", quality=98, subsampling=0, exif=exif)
print("orientation fixtures written to", out)
