"""600 large, visually-distinct JPEGs for the boundary-safe cross-app navigation
benchmark (500 pure-Right transitions from index 0 must never hit the folder end).
Generates a modest number of unique base images at realistic large-photo
resolutions, then hardlinks them out to 600 sequential filenames so every
lexically-adjacent pair differs (base pattern cycles), keeping disk usage low."""
import os, sys
import numpy as np
from PIL import Image

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
sizes = [(6000, 4000), (5472, 3648), (7360, 4912), (6720, 4480), (6048, 4032), (7008, 4672)]
BASE_COUNT = 30
TOTAL = 600

tmp_base = os.path.join(out, "_base")
os.makedirs(tmp_base, exist_ok=True)
made = []
for i in range(BASE_COUNT):
    w, h = sizes[i % len(sizes)]
    x = np.arange(w, dtype=np.uint16)[None, :]
    y = np.arange(h, dtype=np.uint16)[:, None]
    rgb = np.empty((h, w, 3), np.uint8)
    rgb[:, :, 0] = ((x * (3 + i) + y * (1 + i % 5)) & 255).astype(np.uint8)
    rgb[:, :, 1] = ((x + y * (2 + i)) ^ (i * 37)).astype(np.uint8)
    rgb[:, :, 2] = ((x // (2 + i % 3) + y // (1 + i % 4) + i * 11) & 255).astype(np.uint8)
    p = os.path.join(tmp_base, f"base-{i:02d}.jpg")
    Image.fromarray(rgb, "RGB").save(p, quality=90, subsampling=1)
    made.append(p)
    print(f"base {i+1}/{BASE_COUNT}: {os.path.getsize(p)/1024/1024:.1f} MB", flush=True)

for k in range(TOTAL):
    dst = os.path.join(out, f"stress-{k:03d}.jpg")
    if os.path.exists(dst):
        os.remove(dst)
    os.link(made[k % BASE_COUNT], dst)

for p in made:
    os.remove(p)
os.rmdir(tmp_base)

total = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out) if f.startswith("stress-"))
print(f"{len([f for f in os.listdir(out) if f.startswith('stress-')])} files, {total/1024/1024:.0f} MB in {out}")
