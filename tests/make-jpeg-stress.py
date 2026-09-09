"""A hundred large JPEGs, for the sequential-navigation stress run."""
import os, shutil, sys
import numpy as np
from PIL import Image

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
sizes = [(6000, 4000), (5472, 3648), (7360, 4912), (6720, 4480)]
made = []
for i in range(12):
    w, h = sizes[i % len(sizes)]
    x = np.arange(w, dtype=np.uint16)[None, :]
    y = np.arange(h, dtype=np.uint16)[:, None]
    rgb = np.empty((h, w, 3), np.uint8)
    rgb[:, :, 0] = ((x * (3 + i) + y) & 255).astype(np.uint8)
    rgb[:, :, 1] = ((x + y * (2 + i)) & 255).astype(np.uint8)
    rgb[:, :, 2] = ((x // (2 + i % 3) + y // 2) & 255).astype(np.uint8)
    p = os.path.join(out, f"src-{i:02d}.jpg")
    Image.fromarray(rgb, "RGB").save(p, quality=90, subsampling=1)
    made.append(p)
for k in range(100):
    dst = os.path.join(out, f"stress-{k:03d}.jpg")
    if not os.path.exists(dst):
        shutil.copyfile(made[k % len(made)], dst)
for p in made:
    os.remove(p)
total = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
print(f"{len(os.listdir(out))} files, {total/1024/1024:.0f} MB in {out}")
