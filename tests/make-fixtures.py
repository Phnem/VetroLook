"""Fixtures for the metadata and performance passes.

Everything here is synthetic and deterministic: the repository must not carry
somebody's photographs, and a benchmark that depends on a file nobody else has
is not a benchmark anyone can reproduce.
"""
import io, os, struct, sys, zlib
from pathlib import Path
import numpy as np
from PIL import Image
import piexif

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)

def gradient(w, h):
    x = np.arange(w, dtype=np.uint16)[None, :]
    y = np.arange(h, dtype=np.uint16)[:, None]
    rgb = np.empty((h, w, 3), dtype=np.uint8)
    rgb[:, :, 0] = ((x * 3 + y) & 255).astype(np.uint8)
    rgb[:, :, 1] = ((x + y * 2) & 255).astype(np.uint8)
    rgb[:, :, 2] = ((x // 3 + y // 2) & 255).astype(np.uint8)
    return Image.fromarray(rgb, "RGB")

def exif_bytes(orientation=1, gps=False, camera=True, lens=True):
    zeroth, exif, gpsd = {}, {}, {}
    zeroth[piexif.ImageIFD.Orientation] = orientation
    if camera:
        zeroth[piexif.ImageIFD.Make] = b"NIKON CORPORATION"
        zeroth[piexif.ImageIFD.Model] = b"NIKON Z 6"
        zeroth[piexif.ImageIFD.Artist] = b"Vetro Look Test Suite"
        zeroth[piexif.ImageIFD.Copyright] = b"(c) 2026 Vetro Look"
    if lens:
        exif[piexif.ExifIFD.LensModel] = b"NIKKOR Z 50mm f/1.8 S"
        exif[piexif.ExifIFD.LensMake] = b"NIKON"
        exif[piexif.ExifIFD.FocalLength] = (50, 1)
        exif[piexif.ExifIFD.FNumber] = (18, 10)
        exif[piexif.ExifIFD.ExposureTime] = (1, 250)
        exif[piexif.ExifIFD.ISOSpeedRatings] = 400
        exif[piexif.ExifIFD.DateTimeOriginal] = b"2026:03:14 09:26:53"
        exif[piexif.ExifIFD.Flash] = 0
        exif[piexif.ExifIFD.MeteringMode] = 5
        exif[piexif.ExifIFD.ExposureBiasValue] = (-3, 10)
    if gps:
        gpsd[piexif.GPSIFD.GPSLatitudeRef] = b"N"
        gpsd[piexif.GPSIFD.GPSLatitude] = ((35, 1), (39, 1), (2938, 100))
        gpsd[piexif.GPSIFD.GPSLongitudeRef] = b"E"
        gpsd[piexif.GPSIFD.GPSLongitude] = ((139, 1), (41, 1), (3016, 100))
    return piexif.dump({"0th": zeroth, "Exif": exif, "GPS": gpsd, "1st": {}, "thumbnail": None})

XMP_TEMPLATE = """<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Vetro Look fixtures">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:xmp="http://ns.adobe.com/xap/1.0/"
    xmlns:dc="http://purl.org/dc/elements/1.1/"
    xmlns:photoshop="http://ns.adobe.com/photoshop/1.0/"
    xmlns:aux="http://ns.adobe.com/exif/1.0/aux/"
   xmp:Rating="{rating}"{label}
   xmp:CreatorTool="Adobe Lightroom Classic 13.2"
   aux:Lens="NIKKOR Z 50mm f/1.8 S">
   <dc:subject>
    <rdf:Bag>
     <rdf:li>fixture</rdf:li>
     <rdf:li>vetro</rdf:li>
     <rdf:li>{keyword}</rdf:li>
    </rdf:Bag>
   </dc:subject>
   <dc:creator><rdf:Seq><rdf:li>Vetro Look Test Suite</rdf:li></rdf:Seq></dc:creator>
   <dc:rights><rdf:Alt><rdf:li xml:lang="x-default">(c) 2026 Vetro Look</rdf:li></rdf:Alt></dc:rights>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>"""

def write_sidecar(path, rating, label=None, keyword="sidecar"):
    lab = f'\n   xmp:Label="{label}"' if label else ""
    path.write_text(XMP_TEMPLATE.format(rating=rating, label=lab, keyword=keyword), encoding="utf-8")

def jpeg(name, w, h, **kw):
    p = out / name
    gradient(w, h).save(p, quality=91, subsampling=0, exif=exif_bytes(**kw))
    return p

# --- performance fixtures -------------------------------------------------
jpeg("bench-36mp.jpg", 7360, 4912)
jpeg("bench-45mp-portrait.jpg", 5504, 8256, orientation=6)
jpeg("bench-12mp.jpg", 4000, 3000)

# --- metadata fixtures ----------------------------------------------------
# A: plain JPEG with EXIF                     G: GPS      H: camera/lens
jpeg("meta-a-plain.jpg", 1600, 1200)
jpeg("meta-g-gps.jpg", 1024, 768, gps=True)
# I: no metadata at all
gradient(800, 600).save(out / "meta-i-bare.jpg", quality=85)
# C/D/E/F: external XMP as a sidecar next to a RAW-like host file
for name, rating, label in [("meta-c-rating3", 3, None), ("meta-d-rating5", 5, "Select"),
                            ("meta-e-rejected", -1, None), ("meta-f-label", 0, "Red")]:
    host = jpeg(name + ".jpg", 1200, 800)
    write_sidecar(out / (name + ".xmp"), rating, label, keyword=name)
# J: corrupted XMP sidecar — truncated mid-tag
host = jpeg("meta-j-broken.jpg", 900, 600)
(out / "meta-j-broken.xmp").write_text(
    '<?xpacket begin="" id="W5M0Mp"?><x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="htt',
    encoding="utf-8")
# Embedded (not sidecar) XMP packet inside a JPEG APP1 segment.
def embed_xmp(src, dst, xmp):
    data = src.read_bytes()
    payload = b"http://ns.adobe.com/xap/1.0/\x00" + xmp.encode("utf-8")
    seg = b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload
    # insert after SOI, before the first existing marker
    dst.write_bytes(data[:2] + seg + data[2:])
host = jpeg("meta-k-embedded-src.jpg", 1200, 800)
embed_xmp(host, out / "meta-k-embedded.jpg",
          XMP_TEMPLATE.format(rating=4, label='\n   xmp:Label="Green"', keyword="embedded"))
host.unlink()

print("fixtures written to", out)
for f in sorted(out.iterdir()):
    print(f"  {f.name:32} {f.stat().st_size:>12,}")
