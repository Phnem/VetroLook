import struct
from pathlib import Path
w,h=64,48
pixels=b''.join(bytes((x*4,y*5,180)) for y in range(h) for x in range(w))
header=b'BM'+struct.pack('<IHHI',54+len(pixels),0,0,54)+struct.pack('<IiiHHIIiiII',40,w,h,1,24,0,len(pixels),2835,2835,0,0)
Path(__file__).with_name('images').joinpath('07-pattern.bmp').write_bytes(header+pixels)
