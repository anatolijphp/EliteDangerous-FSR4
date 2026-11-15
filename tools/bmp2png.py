# usage: bmp2png.py in.bmp out.png [x y w h scale]
import sys, struct, zlib
d = open(sys.argv[1], 'rb').read()
off = struct.unpack_from('<I', d, 10)[0]; w, h = struct.unpack_from('<ii', d, 18); bpp = struct.unpack_from('<H', d, 28)[0]
bot = h > 0; h = abs(h); bp = bpp // 8; stride = (w * bp + 3) & ~3
def px(x, y):
    yy = (h - 1 - y) if bot else y; i = off + yy * stride + x * bp
    return d[i + 2], d[i + 1], d[i]
x0, y0, cw, ch, sc = (0, 0, w, h, 1) if len(sys.argv) < 8 else tuple(int(v) for v in sys.argv[3:8])
rows = []
for y in range(ch * sc):
    r = bytearray([0])
    for x in range(cw * sc): r += bytes(px(x0 + x // sc, y0 + y // sc))
    rows.append(bytes(r))
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', cw * sc, ch * sc, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
