#!/usr/bin/env python3
"""
TinyEXR v3 parse tester.

Globs a directory tree of .exr files, runs each through the v3 parse harness
(`test/v3/parse_harness`, which uses the pure-C11 public API in include/exr.h),
and classifies every file:

    PASS   - full load succeeded.
    XFAIL  - load failed but the failure is expected. Currently the only
             expected failure is DWA (DWAA / DWAB) compression, which v3
             intentionally does not support (see AGENTS.md).
    XPASS  - a file we expected to fail actually loaded. Surfaced loudly
             because it means an assumption is now stale.
    FAIL   - load failed and the failure is NOT expected. Reported in detail.

Built-in modules only. No third-party dependencies.

Exit status: 0 if there are no FAIL/XPASS results, 1 otherwise.

Examples:
    test/v3/parse-tester.py
    test/v3/parse-tester.py ~/work/openexr-images
    test/v3/parse-tester.py --harness build/parse_harness /path/to/images
    test/v3/parse-tester.py --build              # (re)build the harness first
    test/v3/parse-tester.py -v                   # list every PASS too
"""

import argparse
import fnmatch
import os
import subprocess
import sys

# Compression names (as emitted by the harness) whose decode failure is an
# expected, documented limitation rather than a bug.
EXPECTED_FAIL_COMPRESSIONS = {"DWAA", "DWAB"}

# Known-bad input files whose decode failure is the *file's* fault, not v3's.
# Each entry is (glob, reason); the glob is matched against the path relative
# to the images root (posix slashes). These are XFAILed so they don't drown
# out real regressions.
#
# The v2/.../htj2k*_{Balls,Ground,Leaves,Trunks}.exr deep files were produced by
# OpenEXR's `exr2exr`, which truncates the deep part: OpenEXR's own `exrcheck`
# reports them "bad" and `exrheader` marks them "(incomplete)". The plain
# (non-htj2k) twins are valid and load fine, so v3's deep-scanline ZIPS decode
# is correct - the inputs are corrupt on the write side. Ignored until the
# upstream exr2exr deep bug is fixed and the corpus regenerated.
_EXR2EXR_DEEP = ("deep file truncated by OpenEXR exr2exr "
                 "(exrcheck reports 'bad'); write-side bug, not v3")
KNOWN_BAD_INPUTS = [
    ("v2/*/htj2k*_Balls.exr", _EXR2EXR_DEEP),
    ("v2/*/htj2k*_Ground.exr", _EXR2EXR_DEEP),
    ("v2/*/htj2k*_Leaves.exr", _EXR2EXR_DEEP),
    ("v2/*/htj2k*_Trunks.exr", _EXR2EXR_DEEP),
]


def expected_fail_reason(res, rel):
    """Return a human reason if this file's failure is expected, else None.

    Known-bad globs are matched as a suffix of the full path, so they fire
    whether the tester is pointed at the corpus root or a subdirectory.
    """
    full_posix = os.path.abspath(res.path).replace(os.sep, "/")
    for glob, reason in KNOWN_BAD_INPUTS:
        if fnmatch.fnmatch(full_posix, "*/" + glob):
            return reason
    for c in res.compressions:
        if c in EXPECTED_FAIL_COMPRESSIONS:
            return f"{c} compression intentionally unsupported"
    return None

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_IMAGES = os.path.expanduser("~/work/openexr-images")
DEFAULT_HARNESS = os.path.join(REPO_ROOT, "test", "v3", "parse_harness")

# ANSI colors, disabled when stdout is not a tty.
_TTY = sys.stdout.isatty()


def _c(code, text):
    return f"\033[{code}m{text}\033[0m" if _TTY else text


GREEN = lambda s: _c("32", s)
RED = lambda s: _c("31", s)
YELLOW = lambda s: _c("33", s)
CYAN = lambda s: _c("36", s)
BOLD = lambda s: _c("1", s)


class ParseResult:
    """Parsed output of one parse_harness invocation."""

    def __init__(self, path):
        self.path = path
        self.header_code = None
        self.header_msg = ""
        self.load_code = None
        self.load_msg = ""
        self.compressions = []   # per-part compression names
        self.parts = []          # raw PART lines for diagnostics
        self.harness_exit = None
        self.harness_stderr = ""
        self.xfail_reason = None  # set by classify() when failure is expected

    @property
    def loaded_ok(self):
        return self.load_code is not None and self.load_code >= 0


def run_harness(harness, path, timeout):
    """Invoke the harness on one file and parse its key/value output."""
    res = ParseResult(path)
    try:
        proc = subprocess.run(
            [harness, path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        res.harness_exit = -1
        res.harness_stderr = f"timed out after {timeout}s"
        return res
    except OSError as e:
        res.harness_exit = -1
        res.harness_stderr = f"failed to run harness: {e}"
        return res

    res.harness_exit = proc.returncode
    res.harness_stderr = proc.stderr.decode("utf-8", "replace").strip()

    for line in proc.stdout.decode("utf-8", "replace").splitlines():
        parts = line.split(None, 2)
        if not parts:
            continue
        key = parts[0]
        if key == "HEADER_RESULT" and len(parts) >= 2:
            res.header_code = _to_int(parts[1])
            res.header_msg = parts[2] if len(parts) >= 3 else ""
        elif key == "LOAD_RESULT" and len(parts) >= 2:
            res.load_code = _to_int(parts[1])
            res.load_msg = parts[2] if len(parts) >= 3 else ""
        elif key == "PART":
            res.parts.append(line)
            comp = _field(line, "compression=")
            if comp:
                res.compressions.append(comp)
    return res


def _to_int(s):
    try:
        return int(s)
    except ValueError:
        return None


def _field(line, prefix):
    """Extract a 'prefix<value>' token's value from a whitespace-split line."""
    for tok in line.split():
        if tok.startswith(prefix):
            return tok[len(prefix):]
    return None


def classify(res, rel):
    """Return one of PASS / XPASS / XFAIL / FAIL and stash any xfail reason."""
    res.xfail_reason = expected_fail_reason(res, rel)
    expected = res.xfail_reason is not None
    if res.loaded_ok:
        return "XPASS" if expected else "PASS"
    return "XFAIL" if expected else "FAIL"


def find_exrs(root):
    out = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in filenames:
            if fn.lower().endswith(".exr"):
                out.append(os.path.join(dirpath, fn))
    out.sort()
    return out


def build_harness(harness):
    """(Re)build the v3 library and the parse harness."""
    print(BOLD("Building v3 library (make lib)..."))
    subprocess.run(["make", "lib"], cwd=REPO_ROOT, check=True)
    lib = os.path.join(REPO_ROOT, "build", "libtinyexr3.a")
    src = os.path.join(REPO_ROOT, "test", "v3", "parse_harness.c")
    inc = os.path.join(REPO_ROOT, "include")
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", f"-I{inc}", "-O2",
           src, lib, "-lm", "-o", harness]
    print(BOLD("Compiling harness: ") + " ".join(cmd))
    subprocess.run(cmd, cwd=REPO_ROOT, check=True)


def describe_failure(res):
    """Human-readable detail for a FAIL (or XPASS) result."""
    lines = []
    lines.append(f"    harness exit : {res.harness_exit}")
    lines.append(f"    header       : {res.header_code} ({res.header_msg})")
    lines.append(f"    load         : {res.load_code} ({res.load_msg})")
    if res.compressions:
        uniq = sorted(set(res.compressions))
        lines.append(f"    compression  : {', '.join(uniq)}")
    if res.parts:
        for p in res.parts:
            lines.append(f"    {p}")
    if res.harness_stderr:
        lines.append(f"    stderr       : {res.harness_stderr}")
    return "\n".join(lines)


def main(argv):
    ap = argparse.ArgumentParser(
        description="TinyEXR v3 parse tester (PASS / XFAIL / FAIL).")
    ap.add_argument("images", nargs="?", default=DEFAULT_IMAGES,
                    help=f"root dir to glob for *.exr (default: {DEFAULT_IMAGES})")
    ap.add_argument("--harness", default=DEFAULT_HARNESS,
                    help="path to the parse_harness binary")
    ap.add_argument("--build", action="store_true",
                    help="(re)build the v3 lib + harness before running")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="per-file timeout in seconds (default: 120)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="also list PASS and XFAIL files")
    ap.add_argument("--progress", action="store_true",
                    help="print per-file progress to stderr")
    ap.add_argument("-j", "--jobs", type=int, default=(os.cpu_count() or 1),
                    help="parallel harness invocations (default: all cores)")
    args = ap.parse_args(argv)

    if args.build:
        try:
            build_harness(args.harness)
        except subprocess.CalledProcessError as e:
            print(RED(f"build failed: {e}"))
            return 2

    if not os.path.exists(args.harness):
        print(RED(f"harness not found: {args.harness}"))
        print("Build it with:")
        print(f"    {sys.argv[0]} --build")
        print("or manually:")
        print("    make lib")
        print("    cc -std=c11 -Iinclude -O2 test/v3/parse_harness.c "
              "build/libtinyexr3.a -lm -o test/v3/parse_harness")
        return 2

    if not os.path.isdir(args.images):
        print(RED(f"images dir not found: {args.images}"))
        return 2

    files = find_exrs(args.images)
    if not files:
        print(RED(f"no .exr files found under {args.images}"))
        return 2

    print(BOLD(f"Parsing {len(files)} EXR files under {args.images}\n"))

    total = len(files)

    # Run the harness on each file. The harness is a separate process per file,
    # so a thread pool (which releases the GIL across subprocess calls) scales
    # well; the HTJ2K decode is CPU-bound, so jobs ~= cores is the sweet spot.
    results = {}  # path -> ParseResult
    if args.jobs > 1:
        from concurrent.futures import ThreadPoolExecutor, as_completed
        done = 0
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(run_harness, args.harness, p, args.timeout): p
                    for p in files}
            for fut in as_completed(futs):
                p = futs[fut]
                results[p] = fut.result()
                done += 1
                if args.progress:
                    print(f"[{done}/{total}] {os.path.relpath(p, args.images)}",
                          file=sys.stderr, flush=True)
    else:
        for idx, p in enumerate(files, 1):
            if args.progress:
                print(f"[{idx}/{total}] {os.path.relpath(p, args.images)}",
                      file=sys.stderr, flush=True)
            results[p] = run_harness(args.harness, p, args.timeout)

    buckets = {"PASS": [], "XFAIL": [], "XPASS": [], "FAIL": []}
    for path in files:  # report in stable (sorted) order
        res = results[path]
        rel = os.path.relpath(path, args.images)
        status = classify(res, rel)
        buckets[status].append(res)

        if status == "PASS":
            if args.verbose:
                print(f"  {GREEN('PASS ')} {rel}")
        elif status == "XFAIL":
            line = f"  {YELLOW('XFAIL')} {rel}  ({res.xfail_reason})"
            if args.verbose:
                print(line)
        elif status == "XPASS":
            print(f"  {CYAN('XPASS')} {rel}  "
                  f"(expected to fail [{res.xfail_reason}], but it loaded)")
        else:  # FAIL
            print(f"  {RED('FAIL ')} {rel}")
            print(describe_failure(res))

    # ---- Summary ----
    print()
    print(BOLD("==== Summary ===="))
    print(f"  total : {len(files)}")
    print(f"  {GREEN('PASS')}  : {len(buckets['PASS'])}")
    print(f"  {YELLOW('XFAIL')} : {len(buckets['XFAIL'])}  (expected failures)")
    if buckets["XPASS"]:
        print(f"  {CYAN('XPASS')} : {len(buckets['XPASS'])}  "
              f"(unexpected passes - update expectations)")
    print(f"  {RED('FAIL')}  : {len(buckets['FAIL'])}")

    if buckets["FAIL"]:
        print()
        print(BOLD(RED("Failing files:")))
        for res in buckets["FAIL"]:
            print(f"  - {os.path.relpath(res.path, args.images)} "
                  f"(load {res.load_code}: {res.load_msg})")

    # XPASS counts as a problem (stale expectation) too.
    return 0 if not buckets["FAIL"] and not buckets["XPASS"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
