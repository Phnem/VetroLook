#!/usr/bin/env python3
"""
ZIP-decode throughput: TinyEXR v3 in-tree DEFLATE vs vendored libdeflate.

Builds two `bench_decode` binaries from the same tree - one using the in-tree
pure-C inflate (default), one routing ZIP/ZIPS/PXR24 through the vendored
libdeflate (`make ... LIBDEFLATE=1`) - then decodes the same random sample of
ALab texture_pack EXRs (all tiled ZIP) through each and reports decode-only
throughput plus the in-tree / libdeflate speedup.

Goal of the comparison: the in-tree decoder should be at least as fast as
libdeflate. Run after any change to `src/exr_deflate.c`.

Builds touch the repo `build/` dir (the Makefile object flags are not tracked,
so toggling LIBDEFLATE needs `make clean`); the in-tree `build/` is restored at
the end. Stdlib only.

Examples:
    benchmark/alab/zip_vs_libdeflate.py                # 20-file sample, 5 reps
    benchmark/alab/zip_vs_libdeflate.py --sample 40 --reps 7
    benchmark/alab/zip_vs_libdeflate.py /other/exr/tree
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_ROOT = "/mnt/disk1/data/usd/alab/texture_pack"
BENCH_SRC = os.path.join(REPO_ROOT, "test", "v3", "bench_decode.c")
LIB = os.path.join(REPO_ROOT, "build", "libtinyexr3.a")

_TTY = sys.stdout.isatty()
def _c(code, s): return f"\033[{code}m{s}\033[0m" if _TTY else s
BOLD = lambda s: _c("1", s)
GREEN = lambda s: _c("32", s)
RED = lambda s: _c("31", s)


def sh(cmd, **kw):
    return subprocess.run(cmd, cwd=REPO_ROOT, check=True,
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, **kw)


def find_exrs(root):
    out = []
    for d, _, fns in os.walk(root):
        for n in fns:
            if n.lower().endswith(".exr"):
                out.append(os.path.join(d, n))
    out.sort()
    return out


def build_variant(libdeflate, bench_out):
    """make clean + make lib [LIBDEFLATE=1], then compile bench_decode."""
    sh(["make", "clean"])
    libargs = ["LIBDEFLATE=1"] if libdeflate else []
    sh(["make", "lib"] + libargs)
    cc = os.environ.get("CC", "cc")
    inc = ["-Iinclude"] + (["-Ideps/libdeflate"] if libdeflate else [])
    sh([cc, "-std=c11", *inc, "-O2", BENCH_SRC, LIB, "-lm", "-o", bench_out])


def run_bench(bench, listfile, reps):
    """Best (max) TOTAL Mpix/s and its decode seconds over `reps` runs."""
    best_mpix, best_s = 0.0, None
    with open(listfile) as fp:
        payload = fp.read()
    for _ in range(reps):
        p = subprocess.run([bench], input=payload.encode(),
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        for line in p.stdout.decode("utf-8", "replace").splitlines():
            f = line.split()
            if len(f) == 7 and f[0] == "TOTAL":
                mpix = float(f[5])
                if mpix > best_mpix:
                    best_mpix, best_s = mpix, float(f[3])
    return best_mpix, best_s


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", nargs="?", default=DEFAULT_ROOT,
                    help=f"corpus root (default: {DEFAULT_ROOT})")
    ap.add_argument("--sample", type=int, default=20,
                    help="random sample size (default: 20)")
    ap.add_argument("--seed", type=int, default=20,
                    help="RNG seed (default: 20)")
    ap.add_argument("--reps", type=int, default=5,
                    help="timed repetitions per variant; best is kept (default: 5)")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.root):
        print(RED(f"corpus root not found: {args.root}"))
        return 2
    files = find_exrs(args.root)
    if not files:
        print(RED(f"no .exr under {args.root}"))
        return 2
    rng = random.Random(args.seed)
    rng.shuffle(files)
    sample = sorted(files[:args.sample])
    nbytes = sum(os.path.getsize(f) for f in sample)

    tmp = tempfile.mkdtemp(prefix="zipbench-")
    listfile = os.path.join(tmp, "list.txt")
    with open(listfile, "w") as fp:
        fp.write("\n".join(sample) + "\n")
    bench_in = os.path.join(tmp, "bench_intree")
    bench_ld = os.path.join(tmp, "bench_libdeflate")

    print(BOLD("ZIP decode: in-tree vs libdeflate"))
    print(f"  corpus : {args.root}")
    print(f"  sample : {len(sample)} files ({nbytes/1e6:.1f} MB), seed {args.seed}")
    print(f"  reps   : {args.reps} (best kept)\n")

    try:
        print("building in-tree bench_decode ...")
        build_variant(False, bench_in)
        print("building libdeflate bench_decode ...")
        build_variant(True, bench_ld)
        print("restoring in-tree build/ ...")
        sh(["make", "clean"]); sh(["make", "lib"])

        print("\ntiming ...\n")
        in_mpix, in_s = run_bench(bench_in, listfile, args.reps)
        ld_mpix, ld_s = run_bench(bench_ld, listfile, args.reps)
    except subprocess.CalledProcessError as e:
        msg = e.stderr.decode("utf-8", "replace") if e.stderr else str(e)
        print(RED(f"build/run failed: {msg}"))
        return 2
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(BOLD(f"{'variant':<14} {'decode_s':>10} {'Mpix/s':>10}"))
    print("-" * 36)
    print(f"{'in-tree':<14} {in_s:>10.3f} {in_mpix:>10.1f}")
    print(f"{'libdeflate':<14} {ld_s:>10.3f} {ld_mpix:>10.1f}")
    print("-" * 36)
    if ld_mpix > 0:
        ratio = in_mpix / ld_mpix
        verdict = (GREEN(f"in-tree is {ratio:.2f}x libdeflate "
                         f"({(ratio-1)*100:+.1f}%)") if ratio >= 1.0
                   else RED(f"in-tree is {ratio:.2f}x libdeflate "
                            f"({(ratio-1)*100:+.1f}%) - SLOWER"))
        print(verdict)
    return 0 if in_mpix >= ld_mpix else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
