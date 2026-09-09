"""Create a deterministic 36 MP JPEG used only by the decode benchmark."""
from pathlib import Path
import sys

import numpy as np
from PIL import Image

output = Path(sys.argv[1])
width, height = 7360, 4912
x = np.arange(width, dtype=np.uint16)[None, :]
y = np.arange(height, dtype=np.uint16)[:, None]
rgb = np.empty((height, width, 3), dtype=np.uint8)
rgb[:, :, 0] = ((x * 3 + y) & 255).astype(np.uint8)
rgb[:, :, 1] = ((x + y * 2) & 255).astype(np.uint8)
rgb[:, :, 2] = ((x // 3 + y // 2) & 255).astype(np.uint8)
output.parent.mkdir(parents=True, exist_ok=True)
Image.fromarray(rgb, "RGB").save(output, quality=91, subsampling=0)
