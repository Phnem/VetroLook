"""Rewrite the merged composite of a real PSD fixture using Adobe compression 2/3.

The layers/resources remain from the source document; only the flattened image
data codec is changed.  This supplements (rather than replaces) Photoshop-made
fixtures when exercising ZIP and ZIP-with-prediction.
"""
from pathlib import Path
import sys

from psd_tools import PSDImage
from psd_tools.constants import Compression

source = Path(sys.argv[1])
output = Path(sys.argv[2])
compression = Compression(int(sys.argv[3]))
psd = PSDImage.open(source)
record = psd._record
planes = record.image_data.get_data(record.header)
record.image_data.compression = compression
record.image_data.set_data(planes, record.header)
output.parent.mkdir(parents=True, exist_ok=True)
psd.save(output)
