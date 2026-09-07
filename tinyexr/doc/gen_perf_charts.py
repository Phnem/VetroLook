#!/usr/bin/env python3
"""
Generate the hand-styled perf SVGs in doc/ from inline data (stdlib only).

The charts in doc/ were originally hand-authored SVG; this reproduces that exact
minimal style (620x400, blue=in-tree / green=libdeflate / amber=OpenEXR, value
labels above bars, light gridlines) so the data-bearing ones can be regenerated.

    python3 doc/gen_perf_charts.py        # writes the *.svg next to this script

Colors:  in-tree #2563eb, libdeflate #16a34a, OpenEXR #f59e0b
"""

import os

W, H = 620, 400
X0, X1 = 60.0, 602.0          # plot x-range
Y0, YTOP = 328.0, 64.0        # y of value 0 and of y-max
CY = {"intree": "#2563eb", "libdeflate": "#16a34a", "openexr": "#f59e0b",
      "before": "#9ca3af", "zstd": "#7c3aed"}


def _fmt(v):
    return ("%.1f" % v).rstrip("0").rstrip(".") if v % 1 else "%d" % v


def chart(path, title, subtitle, cats, series, ymax, ytick):
    """series: list of (label, color, [value per cat]). cats: list of names."""
    span = Y0 - YTOP
    def y(v):
        return Y0 - (v / ymax) * span
    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="0 0 %d %d" font-family="-apple-system,Segoe UI,Roboto,'
               'sans-serif">' % (W, H, W, H))
    out.append('<rect width="%d" height="%d" fill="#fff"/>' % (W, H))
    out.append('<text x="60" y="26" font-size="17" font-weight="700" '
               'fill="#111">%s</text>' % title)
    out.append('<text x="60" y="44" font-size="12" fill="#666">%s</text>' % subtitle)
    # gridlines + y labels
    t = 0
    while t <= ymax + 1e-9:
        gy = y(t)
        out.append('<line x1="60" y1="%.1f" x2="602" y2="%.1f" stroke="#eee"/>'
                   % (gy, gy))
        out.append('<text x="52" y="%.1f" font-size="11" fill="#888" '
                   'text-anchor="end">%s</text>' % (gy + 4, _fmt(t)))
        t += ytick
    out.append('<text x="16" y="196" font-size="12" fill="#666" '
               'transform="rotate(-90 16 196)" text-anchor="middle">'
               'megapixels / s</text>')
    # bars
    ncat = len(cats)
    nser = len(series)
    group_w = (X1 - X0) / ncat
    bw = min(42.6, group_w * 0.78 / nser)
    for ci, cat in enumerate(cats):
        gx = X0 + ci * group_w
        inner = nser * bw
        start = gx + (group_w - inner) / 2.0
        for si, (label, color, vals) in enumerate(series):
            v = vals[ci]
            bx = start + si * bw
            by = y(v)
            out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" '
                       'fill="%s" rx="2"/>' % (bx, by, bw, Y0 - by, color))
            out.append('<text x="%.1f" y="%.1f" font-size="9" fill="#444" '
                       'text-anchor="middle">%s</text>'
                       % (bx + bw / 2.0, by - 4, _fmt(v)))
        out.append('<text x="%.1f" y="346" font-size="12" fill="#333" '
                   'text-anchor="middle">%s</text>'
                   % (gx + group_w / 2.0, cat))
    out.append('<line x1="60" y1="328.0" x2="602" y2="328.0" stroke="#bbb"/>')
    # legend
    lx = 60
    for label, color, _ in series:
        out.append('<rect x="%d" y="370" width="12" height="12" fill="%s" '
                   'rx="2"/>' % (lx, color))
        out.append('<text x="%d" y="380" font-size="12" fill="#333">%s</text>'
                   % (lx + 17, label))
        lx += 24 + int(7.0 * len(label))
    out.append('</svg>')
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", path)


def wide_chart(path, title, subtitle, cats, series, ymax, ytick):
    """Write the 2x README chart used for the Ryzen comparison headline."""
    w, h = 1520, 800
    x0, x1 = 120.0, 1460.0
    y0, ytop = 656.0, 128.0
    span = y0 - ytop

    def y(v):
        return y0 - (v / ymax) * span

    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d" font-family="-apple-system,Segoe UI,Roboto,sans-serif">'
        % (w, h, w, h),
        '<rect width="%d" height="%d" fill="#fff"/>' % (w, h),
        '<text x="120" y="52" font-size="34" font-weight="700" '
        'fill="#111">%s</text>' % title,
        '<text x="120" y="88" font-size="24" fill="#666">%s</text>' % subtitle,
    ]
    tick = 0
    while tick <= ymax + 1e-9:
        gy = y(tick)
        out.append('<line x1="120" y1="%.1f" x2="1460" y2="%.1f" '
                   'stroke="#eee"/>' % (gy, gy))
        out.append('<text x="104" y="%.1f" font-size="22" fill="#888" '
                   'text-anchor="end">%s</text>' % (gy + 8, _fmt(tick)))
        tick += ytick
    out.append('<text x="32" y="392" font-size="24" fill="#666" '
               'transform="rotate(-90 32 392)" text-anchor="middle">'
               'megapixels / s</text>')

    group_w = (x1 - x0) / len(cats)
    bw = min(72.0, group_w * 0.78 / len(series))
    for ci, cat in enumerate(cats):
        gx = x0 + ci * group_w
        inner = len(series) * bw
        start = gx + (group_w - inner) / 2.0
        for si, (label, color, values) in enumerate(series):
            value = values[ci]
            bx = start + si * bw
            by = y(value)
            out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" '
                       'fill="%s" rx="4"/>' % (bx, by, bw, y0 - by, color))
            out.append('<text x="%.1f" y="%.1f" font-size="18" fill="#444" '
                       'text-anchor="middle">%s</text>'
                       % (bx + bw / 2.0, by - 8, _fmt(value)))
        out.append('<text x="%.1f" y="692" font-size="24" fill="#333" '
                   'text-anchor="middle">%s</text>' %
                   (gx + group_w / 2.0, cat))
    out.append('<line x1="120" y1="656.0" x2="1460" y2="656.0" '
               'stroke="#bbb"/>')
    lx = 120
    for label, color, _ in series:
        out.append('<rect x="%d" y="740" width="24" height="24" fill="%s" '
                   'rx="4"/>' % (lx, color))
        out.append('<text x="%d" y="760" font-size="24" fill="#333">%s</text>'
                   % (lx + 34, label))
        lx += 48 + int(14.0 * len(label))
    out.append('</svg>')
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", path)


def arm_chart(path, title, subtitle, cats, series, ymax=350, ytick=70):
    """Generate the Apple-Silicon chart used by README and the ARM section."""
    w, h = 760, 400
    x0, x1 = 60.0, 742.0
    y0, ytop = 328.0, 64.0
    span = y0 - ytop

    def y(v):
        return y0 - (v / ymax) * span

    out = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="760" height="400" '
        'viewBox="0 0 760 400" font-family="-apple-system,Segoe UI,Roboto,sans-serif">',
        '<rect width="760" height="400" fill="#fff"/>',
        '<text x="60" y="26" font-size="17" font-weight="700" fill="#111">%s</text>' % title,
        '<text x="60" y="44" font-size="12" fill="#666">%s</text>' % subtitle,
    ]
    tick = 0
    while tick <= ymax + 1e-9:
        gy = y(tick)
        out.append('<line x1="60" y1="%.1f" x2="742" y2="%.1f" stroke="#eee"/>' % (gy, gy))
        out.append('<text x="52" y="%.1f" font-size="11" fill="#888" text-anchor="end">%s</text>' % (gy + 4, _fmt(tick)))
        tick += ytick
    out.append('<text x="16" y="196" font-size="12" fill="#666" transform="rotate(-90 16 196)" text-anchor="middle">megapixels / s</text>')
    group_w = (x1 - x0) / len(cats)
    bw = min(29.5, group_w * 0.78 / len(series))
    for ci, cat in enumerate(cats):
        gx = x0 + ci * group_w
        inner = len(series) * bw
        start = gx + (group_w - inner) / 2.0
        for si, (label, color, values) in enumerate(series):
            value = values[ci]
            bx, by = start + si * bw, y(value)
            out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" rx="2"/>' % (bx, by, bw, y0 - by, color))
            out.append('<text x="%.1f" y="%.1f" font-size="9" fill="#444" text-anchor="middle">%s</text>' % (bx + bw / 2.0, by - 4, _fmt(value)))
        out.append('<text x="%.1f" y="346" font-size="12" fill="#333" text-anchor="middle">%s</text>' % (gx + group_w / 2.0, cat))
    out.append('<line x1="60" y1="328.0" x2="742" y2="328.0" stroke="#bbb"/>')
    lx = 60
    for label, color, _ in series:
        out.append('<rect x="%d" y="370" width="12" height="12" fill="%s" rx="2"/>' % (lx, color))
        out.append('<text x="%d" y="380" font-size="12" fill="#333">%s</text>' % (lx + 17, label))
        lx += 24 + int(7.0 * len(label))
    out.append('</svg>')
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", path)


def main():
    here = os.path.dirname(os.path.abspath(__file__))

    # Decode throughput, in-tree vs libdeflate, openexr-images natural corpus.
    # Measured with test/v3/bench_decode (single thread, decode-only, Mpix/s).
    chart(
        os.path.join(here, "perf-libdeflate-corpus-decode.svg"),
        "Decode throughput - in-tree vs libdeflate (natural-image corpus)",
        "openexr-images; libdeflate is the DEFLATE=auto hosted default.",
        ["zip", "zips", "pxr24"],
        [("tinyexr (in-tree)", CY["intree"], [226, 171, 309]),
         ("tinyexr (libdeflate)", CY["libdeflate"], [391, 278, 479])],
        ymax=500, ytick=100,
    )

    # PIZ decode before/after the in-tree optimization (best-of-8 on the 36
    # openexr-images PIZ files; inline Huffman literal + tighter canonical scan).
    chart(
        os.path.join(here, "perf-piz-decode.svg"),
        "PIZ decode - in-tree optimization (+14%)",
        "openexr-images; inline Huffman literal store + tighter canonical scan.",
        ["piz decode"],
        [("before", CY["before"], [80.2]),
         ("after", CY["intree"], [91.6])],
        ymax=100, ytick=20,
    )

    # ZSTD vs ZIP decode on identical images (6 openexr-images ScanLines files
    # re-encoded to each; both decoded with the default hosted build).
    chart(
        os.path.join(here, "perf-zstd-decode.svg"),
        "ZSTD vs ZIP decode - identical images",
        "6 openexr-images ScanLines files re-encoded to each codec.",
        ["decode"],
        [("zstd (vendored)", CY["zstd"], [410]),
         ("zip (libdeflate)", CY["libdeflate"], [272])],
        ymax=450, ytick=90,
    )

    # README headline chart. Values are a pinned Ryzen 9 3950X run of
    # benchmark/bench_compare on asakusa.exr with V3_OPT=-O3. The in-tree
    # series uses DEFLATE=intree; the green series uses the hosted default
    # DEFLATE=auto/libdeflate. HTJ2K has no deflate backend.
    wide_chart(
        os.path.join(here, "perf-libdeflate-htj2k-decode.svg"),
        "Decode throughput - libdeflate on/off vs OpenEXR (single thread)",
        "AMD Ryzen 9 3950X (Zen2), asakusa.exr 660x440, in-memory. Higher is better.",
        ["zip", "zips", "pxr24", "htj2k256", "htj2k32"],
        [("tinyexr (in-tree)", CY["intree"], [32.2, 17.0, 29.9, 23.2, 23.5]),
         ("tinyexr (libdeflate)", CY["libdeflate"], [42.4, 31.8, 43.7, 24.1, 23.5]),
         ("OpenEXR", CY["openexr"], [30.7, 24.4, 43.8, 29.8, 27.9])],
        ymax=90, ytick=18,
    )

    # Apple M1 decode comparison. The tuned series uses the opt-in
    # EXR_JPH_HOST_NEON=1 build; the portable series is the default arm64
    # build. Values are the latest repeated in-memory asakusa.exr run.
    arm_chart(
        os.path.join(here, "perf-arm-decode.svg"),
        "Decode throughput - Apple M1 (higher is better)",
        "Apple M1, asakusa.exr 660x440, in-memory, single thread; NEON auto-dispatch.",
        ["rle", "zips", "zip", "piz", "pxr24", "b44", "htj2k256", "htj2k32"],
        [("tinyexr (portable)", CY["intree"], [159, 56, 71, 68, 82, 310, 35.7, 35.7]),
         ("tinyexr (M1 tuned)", "#7c3aed", [159, 56, 71, 68, 82, 310, 37.6, 37.7]),
         ("OpenEXR/OpenJPH", CY["openexr"], [85, 60, 78, 64, 81, 213, 38.7, 37.5])],
        ymax=350, ytick=70,
    )


if __name__ == "__main__":
    main()
