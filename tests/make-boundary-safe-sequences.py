"""Generates boundary-safe navigation-key sequences: none of them ever ask a
non-wrapping viewer to step past the first or last file in its fixture folder,
which was the root cause of the sub-500-sample navigation runs in the previous
pass (see BENCHMARK_RESULTS.txt / SAMPLE COUNT SHORTFALL)."""
import os, random

OUT_DIR = r"D:\AndroidStudioProjects\Vetro_Look\release\Benchmarks"
TRANSITIONS = 500
SEED = 1102026


def write_header(f, **kw):
    for k, v in kw.items():
        f.write(f"# {k}={v}\n")


def bounded_random_walk(seed, start, safe_min, safe_max, count):
    """Deterministic seeded walk. On each step, flip a coin for L/R; if that
    step would exit [safe_min, safe_max], take the opposite direction instead
    (so the walk can never leave the safe range, no matter how unlucky)."""
    rng = random.Random(seed)
    pos = start
    keys = []
    for _ in range(count):
        want_right = rng.random() < 0.5
        if want_right and pos + 1 > safe_max:
            want_right = False
        elif not want_right and pos - 1 < safe_min:
            want_right = True
        pos += 1 if want_right else -1
        keys.append("Right" if want_right else "Left")
    return keys


def oscillating_walk(start, amplitude, count):
    """R x amplitude, L x amplitude, repeating. Never leaves
    [start, start+amplitude]. Deterministic, no RNG needed."""
    keys = []
    pos = start
    going_right = True
    while len(keys) < count:
        for _ in range(amplitude):
            if len(keys) >= count:
                break
            if going_right:
                keys.append("Right")
                pos += 1
            else:
                keys.append("Left")
                pos -= 1
        going_right = not going_right
    return keys


# --- Common (JPEG) random navigation: 600 files in CommonCrossApp, indices 0..599 ---
common_start, common_min, common_max = 300, 100, 500
common_keys = bounded_random_walk(SEED, common_start, common_min, common_max, TRANSITIONS)
with open(os.path.join(OUT_DIR, "common-random-keys.txt"), "w") as f:
    write_header(f, seed=SEED, startIndex=common_start, safeMin=common_min, safeMax=common_max,
                 transitions=TRANSITIONS, fixtureFolder="CommonCrossApp", fixtureCount=600,
                 startFile=f"stress-{common_start:03d}.jpg")
    f.write("\n".join(common_keys) + "\n")
lo, hi = min([common_start] + list(__import__("itertools").accumulate(
    (1 if k == "Right" else -1 for k in common_keys), initial=common_start))), \
    max([common_start] + list(__import__("itertools").accumulate(
        (1 if k == "Right" else -1 for k in common_keys), initial=common_start)))
print(f"common-random: visited range [{lo},{hi}] (safe [{common_min},{common_max}])")

# --- RAW random navigation: 30 files in Raw/browse-r-*, indices 0..29 ---
raw_start, raw_min, raw_max = 15, 5, 25
raw_keys = bounded_random_walk(SEED, raw_start, raw_min, raw_max, TRANSITIONS)
with open(os.path.join(OUT_DIR, "raw-random-keys.txt"), "w") as f:
    write_header(f, seed=SEED, startIndex=raw_start, safeMin=raw_min, safeMax=raw_max,
                 transitions=TRANSITIONS, fixtureFolder="Raw", fixtureCount=30,
                 startFile=f"browse-r-{raw_start:02d}.*")
    f.write("\n".join(raw_keys) + "\n")
lo, hi = min([raw_start] + list(__import__("itertools").accumulate(
    (1 if k == "Right" else -1 for k in raw_keys), initial=raw_start))), \
    max([raw_start] + list(__import__("itertools").accumulate(
        (1 if k == "Right" else -1 for k in raw_keys), initial=raw_start)))
print(f"raw-random: visited range [{lo},{hi}] (safe [{raw_min},{raw_max}])")

# --- RAW sequential: oscillating R10/L10 around the middle of the 30-file folder ---
raw_seq_start, raw_seq_amp = 10, 10
raw_seq_keys = oscillating_walk(raw_seq_start, raw_seq_amp, TRANSITIONS)
with open(os.path.join(OUT_DIR, "raw-sequential-keys.txt"), "w") as f:
    write_header(f, pattern=f"R{raw_seq_amp}/L{raw_seq_amp} oscillation", startIndex=raw_seq_start,
                 amplitude=raw_seq_amp, transitions=TRANSITIONS, fixtureFolder="Raw", fixtureCount=30,
                 startFile=f"browse-r-{raw_seq_start:02d}.*",
                 note="deterministic, no RNG - always within [start,start+amplitude]")
    f.write("\n".join(raw_seq_keys) + "\n")
print(f"raw-sequential: visited range [{raw_seq_start},{raw_seq_start+raw_seq_amp}] "
      f"(folder bounds [0,29])")

print("Common-sequential: no sequence file needed - pure Right x500 from "
      "CommonCrossApp/stress-000.jpg stays within [0,500] of 600 files (margin 99).")
