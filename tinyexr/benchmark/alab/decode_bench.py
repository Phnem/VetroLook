#!/usr/bin/env python3
"""
TinyEXR v3 decode benchmark runner for the Animal Logic ALab texture_pack.

Drives the two existing v3 C harnesses over a corpus of *.exr files and reports:

  * decode SPEED   - per-codec and total throughput (Mpix/s, in MB/s),
                     measured by `test/v3/bench_decode` (in-memory, decode-only).
  * decode FAILURES - which specific files fail to load, with the load message
                     and compression, classified via `test/v3/parse_harness`.

The two passes are independent on purpose: `bench_decode` is the authoritative
timing engine (it slurps each file into RAM and times only
`exr_load_from_memory`), while `parse_harness` gives a reliable per-file
PASS/FAIL with a reason. The chosen sample is therefore decoded twice - once for
timing, once for correctness - which is cheap at the default sample size and lets
each tool do the one thing it does well. Pass `--all` for a full corpus sweep
(slow: the ALab texture_pack is ~74 GB / 6831 files).

Stdlib only. No third-party dependencies.

Examples:
    benchmark/alab/decode_bench.py                 # 200-file sample, default corpus
    benchmark/alab/decode_bench.py --sample 50 --progress
    benchmark/alab/decode_bench.py --all           # whole corpus (~74 GB)
    benchmark/alab/decode_bench.py --build         # (re)build the harnesses first
    benchmark/alab/decode_bench.py /some/other/tree
"""

import argparse
import os
import random
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_ROOT = "/mnt/disk1/data/usd/alab/texture_pack"
DEFAULT_BENCH = os.path.join(REPO_ROOT, "test", "v3", "bench_decode")
DEFAULT_HARNESS = os.path.join(REPO_ROOT, "test", "v3", "parse_harness")
DEFAULT_REPORT = os.path.normpath(
    os.path.join(os.path.dirname(__file__), "last-report.md"))

# Compressions whose decode failure is an expected, documented v3 limitation
# (the lossy DCT DWA codecs are intentionally unimplemented). Mirrors
# test/v3/parse-tester.py so a stray DWA file is XFAIL, not a spurious FAIL.
EXPECTED_FAIL_COMPRESSIONS = {"DWAA", "DWAB"}

_TTY = sys.stdout.isatty()


def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _TTY else text


GREEN = lambda s: _c("32", s)
RED = lambda s: _c("31", s)
YELLOW = lambda s: _c("33", s)
BOLD = lambda s: _c("1", s)


# --------------------------------------------------------------------------
# corpus discovery / sampling
# --------------------------------------------------------------------------
def find_exrs(root):
    out = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if fn.lower().endswith(".exr"):
                out.append(os.path.join(dirpath, fn))
    out.sort()
    return out


def select(files, sample, seed):
    """Return the working subset: a seeded random sample, or all of them."""
    if sample is None or sample >= len(files):
        return list(files)
    rng = random.Random(seed)
    picked = rng.sample(files, sample)
    picked.sort()
    return picked


def total_bytes(files):
    total = 0
    for f in files:
        try:
            total += os.path.getsize(f)
        except OSError:
            pass
    return total


# --------------------------------------------------------------------------
# speed pass: pipe the file list to bench_decode, parse its table
# --------------------------------------------------------------------------
def run_bench_decode(bench, files):
    """Feed paths to bench_decode on stdin; return (rows, total_row, raw)."""
    payload = "".join(f + "\n" for f in files)
    proc = subprocess.run(
        [bench], input=payload.encode("utf-8"),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    raw = proc.stdout.decode("utf-8", "replace")
    rows = []
    total_row = None
    for line in raw.splitlines():
        parts = line.split()
        # data rows look like: <codec> <files> <fail> <secs> <mpix> <mps> <mbs>
        if len(parts) == 7 and parts[0] != "codec":
            codec = parts[0]
            try:
                rec = {
                    "codec": codec,
                    "files": int(parts[1]),
                    "fail": int(parts[2]),
                    "decode_s": float(parts[3]),
                    "mpix": float(parts[4]),
                    "mpix_s": float(parts[5]),
                    "mb_s": float(parts[6]),
                }
            except ValueError:
                continue
            if codec == "TOTAL":
                total_row = rec
            else:
                rows.append(rec)
    return rows, total_row, raw


# --------------------------------------------------------------------------
# failure pass: run parse_harness per file (thread pool), collect FAILs
# --------------------------------------------------------------------------
class FileResult:
    def __init__(self, path):
        self.path = path
        self.load_code = None
        self.load_msg = ""
        self.compressions = []
        self.harness_exit = None
        self.stderr = ""

    @property
    def loaded_ok(self):
        return self.load_code is not None and self.load_code >= 0


def _field(line, prefix):
    for tok in line.split():
        if tok.startswith(prefix):
            return tok[len(prefix):]
    return None


def run_harness(harness, path, timeout):
    res = FileResult(path)
    try:
        proc = subprocess.run(
            [harness, path], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        res.harness_exit = -1
        res.stderr = f"timed out after {timeout}s"
        return res
    except OSError as e:
        res.harness_exit = -1
        res.stderr = f"failed to run harness: {e}"
        return res

    res.harness_exit = proc.returncode
    res.stderr = proc.stderr.decode("utf-8", "replace").strip()
    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        parts = line.split(None, 2)
        if not parts:
            continue
        if parts[0] == "LOAD_RESULT" and len(parts) >= 2:
            try:
                res.load_code = int(parts[1])
            except ValueError:
                res.load_code = None
            res.load_msg = parts[2] if len(parts) >= 3 else ""
        elif parts[0] == "PART":
            comp = _field(line, "compression=")
            if comp:
                res.compressions.append(comp)
    return res


def classify(res):
    """Return 'PASS', 'XFAIL', 'XPASS', or 'FAIL'."""
    expected = any(c in EXPECTED_FAIL_COMPRESSIONS for c in res.compressions)
    if res.loaded_ok:
        return "XPASS" if expected else "PASS"
    return "XFAIL" if expected else "FAIL"


def collect_failures(harness, files, jobs, timeout, progress):
    """Run the harness over every file; return classified buckets."""
    buckets = {"PASS": [], "XFAIL": [], "XPASS": [], "FAIL": []}
    total = len(files)
    results = {}
    if jobs > 1:
        from concurrent.futures import ThreadPoolExecutor, as_completed
        done = 0
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            futs = {ex.submit(run_harness, harness, p, timeout): p
                    for p in files}
            for fut in as_completed(futs):
                p = futs[fut]
                results[p] = fut.result()
                done += 1
                if progress:
                    print(f"[harness {done}/{total}] {os.path.basename(p)}",
                          file=sys.stderr, flush=True)
    else:
        for idx, p in enumerate(files, 1):
            if progress:
                print(f"[harness {idx}/{total}] {os.path.basename(p)}",
                      file=sys.stderr, flush=True)
            results[p] = run_harness(harness, p, timeout)

    for p in files:  # stable order
        buckets[classify(results[p])].append(results[p])
    return buckets


# --------------------------------------------------------------------------
# build (mirrors parse-tester.py)
# --------------------------------------------------------------------------
def build_binaries(bench, harness):
    print(BOLD("Building v3 library (make lib)..."))
    subprocess.run(["make", "lib"], cwd=REPO_ROOT, check=True)
    lib = os.path.join(REPO_ROOT, "build", "libtinyexr3.a")
    inc = os.path.join(REPO_ROOT, "include")
    cc = os.environ.get("CC", "cc")
    targets = [
        (os.path.join(REPO_ROOT, "test", "v3", "bench_decode.c"), bench),
        (os.path.join(REPO_ROOT, "test", "v3", "parse_harness.c"), harness),
    ]
    for src, out in targets:
        cmd = [cc, "-std=c11", "-Wall", "-Wextra", f"-I{inc}", "-O2",
               src, lib, "-lm", "-o", out]
        print(BOLD("Compiling: ") + " ".join(cmd))
        subprocess.run(cmd, cwd=REPO_ROOT, check=True)


# --------------------------------------------------------------------------
# reporting
# --------------------------------------------------------------------------
def format_table(rows, total_row):
    hdr = f"{'codec':<10} {'files':>7} {'fail':>6} {'decode_s':>10} " \
          f"{'Mpix-ch':>10} {'Mpix/s':>10} {'in_MB/s':>10}"
    sep = "-" * len(hdr)

    def fmt(r):
        return (f"{r['codec']:<10} {r['files']:>7} {r['fail']:>6} "
                f"{r['decode_s']:>10.3f} {r['mpix']:>10.1f} "
                f"{r['mpix_s']:>10.1f} {r['mb_s']:>10.1f}")

    out = [hdr, sep]
    out.extend(fmt(r) for r in rows)
    if total_row:
        out.append(sep)
        out.append(fmt(total_row))
    return "\n".join(out)


def fail_line(res):
    comp = ",".join(sorted(set(res.compressions))) or "?"
    return (f"{res.path}  (load {res.load_code}: {res.load_msg or 'n/a'}; "
            f"compression={comp}; harness_exit={res.harness_exit})")


def write_report(path, ctx):
    lines = []
    lines.append("# ALab texture_pack decode benchmark\n")
    lines.append(f"- corpus root: `{ctx['root']}`")
    lines.append(f"- total .exr found: {ctx['total_found']}")
    scope = "full corpus" if ctx["is_all"] else f"random sample, seed {ctx['seed']}"
    lines.append(f"- decoded this run: {ctx['n']} ({scope})")
    lines.append(f"- input size of decoded set: {ctx['gib']:.2f} GiB")
    lines.append("")
    lines.append("## Throughput (decode-only, single-threaded; via bench_decode)\n")
    lines.append("```")
    lines.append(ctx["table"])
    lines.append("```")
    lines.append("")
    n_fail = len(ctx["buckets"]["FAIL"])
    n_xfail = len(ctx["buckets"]["XFAIL"])
    n_xpass = len(ctx["buckets"]["XPASS"])
    lines.append("## Decode failures (via parse_harness)\n")
    lines.append(f"- PASS : {len(ctx['buckets']['PASS'])}")
    lines.append(f"- XFAIL: {n_xfail} (expected, e.g. DWAA/DWAB)")
    if n_xpass:
        lines.append(f"- XPASS: {n_xpass} (expected to fail but loaded)")
    lines.append(f"- FAIL : {n_fail}")
    lines.append("")
    if n_fail:
        lines.append("### Failing files\n")
        for res in ctx["buckets"]["FAIL"]:
            lines.append(f"- {fail_line(res)}")
    else:
        lines.append("No decode failures. ✅")
    lines.append("")
    with open(path, "w") as fp:
        fp.write("\n".join(lines))


# --------------------------------------------------------------------------
def main(argv):
    ap = argparse.ArgumentParser(
        description="TinyEXR v3 decode benchmark over the ALab texture_pack.")
    ap.add_argument("root", nargs="?", default=DEFAULT_ROOT,
                    help=f"corpus root to glob for *.exr (default: {DEFAULT_ROOT})")
    ap.add_argument("--sample", type=int, default=200,
                    help="random sample of N files (default: 200)")
    ap.add_argument("--all", action="store_true",
                    help="decode every *.exr under root (overrides --sample)")
    ap.add_argument("--seed", type=int, default=0,
                    help="RNG seed for reproducible sampling (default: 0)")
    ap.add_argument("--bench-decode", default=DEFAULT_BENCH,
                    help="path to the bench_decode binary")
    ap.add_argument("--harness", default=DEFAULT_HARNESS,
                    help="path to the parse_harness binary")
    ap.add_argument("--build", action="store_true",
                    help="(re)build the v3 lib + both harnesses first")
    ap.add_argument("--jobs", type=int, default=(os.cpu_count() or 1),
                    help="parallel parse_harness workers (default: all cores)")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="per-file harness timeout in seconds (default: 300)")
    ap.add_argument("--report", default=DEFAULT_REPORT,
                    help=f"markdown report path (default: {DEFAULT_REPORT})")
    ap.add_argument("--progress", action="store_true",
                    help="print live per-file progress to stderr")
    args = ap.parse_args(argv)

    if args.build:
        try:
            build_binaries(args.bench_decode, args.harness)
        except subprocess.CalledProcessError as e:
            print(RED(f"build failed: {e}"))
            return 2

    for label, p in (("bench_decode", args.bench_decode),
                     ("parse_harness", args.harness)):
        if not os.path.exists(p):
            print(RED(f"{label} not found: {p}"))
            print("Build both with:  benchmark/alab/decode_bench.py --build")
            return 2

    if not os.path.isdir(args.root):
        print(RED(f"corpus root not found: {args.root}"))
        return 2

    all_files = find_exrs(args.root)
    if not all_files:
        print(RED(f"no .exr files found under {args.root}"))
        return 2

    sample = None if args.all else args.sample
    files = select(all_files, sample, args.seed)
    is_all = sample is None or sample >= len(all_files)
    gib = total_bytes(files) / (1024.0 ** 3)

    print(BOLD(f"ALab decode benchmark"))
    print(f"  root           : {args.root}")
    print(f"  total .exr     : {len(all_files)}")
    print(f"  decoding       : {len(files)} "
          f"({'all' if is_all else f'sample, seed {args.seed}'}), "
          f"{gib:.2f} GiB")
    print()

    # ---- speed pass ----
    print(BOLD("Timing decode (bench_decode, single-threaded)..."))
    rows, total_row, _raw = run_bench_decode(args.bench_decode, files)
    table = format_table(rows, total_row)
    print(table)
    print()

    # ---- failure pass ----
    print(BOLD(f"Classifying loads (parse_harness, {args.jobs} workers)..."))
    buckets = collect_failures(args.harness, files, args.jobs,
                               args.timeout, args.progress)

    n_fail = len(buckets["FAIL"])
    n_xpass = len(buckets["XPASS"])
    print()
    print(BOLD("==== Decode results ===="))
    print(f"  {GREEN('PASS')}  : {len(buckets['PASS'])}")
    print(f"  {YELLOW('XFAIL')} : {len(buckets['XFAIL'])}  (expected, e.g. DWA)")
    if n_xpass:
        print(f"  XPASS : {n_xpass}  (expected to fail but loaded)")
    print(f"  {RED('FAIL')}  : {n_fail}")
    if n_fail:
        print()
        print(BOLD(RED("Failing files:")))
        for res in buckets["FAIL"]:
            print(f"  - {fail_line(res)}")

    # ---- report ----
    ctx = {
        "root": args.root, "total_found": len(all_files), "n": len(files),
        "is_all": is_all, "seed": args.seed, "gib": gib,
        "table": table, "buckets": buckets,
    }
    try:
        write_report(args.report, ctx)
        print()
        print(f"report written: {args.report}")
    except OSError as e:
        print(RED(f"could not write report {args.report}: {e}"))

    return 0 if not n_fail and not n_xpass else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
