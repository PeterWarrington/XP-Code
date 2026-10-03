"""Build res/icon.ico (16, 24, 32 and 48 px, 32-bit with alpha) from res/icon.png.

The artwork source is res/icon.svg; export it to res/icon.png (square, RGBA) and run

    python tools\\make_icon.py

Pure Python (zlib and struct only) so it runs on the Python 3.4 that ships for Windows XP:
it decodes the PNG, downsamples with area averaging on premultiplied alpha (so transparent edges
don't darken), and writes the classic BMP-based icon format that Windows XP understands.
"""
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = (16, 24, 32, 48)


def read_png(path):
    """Decode an 8-bit RGBA or RGB, non-interlaced PNG into (size, bytearray of RGBA rows)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG: " + path)
    pos, idat = 8, b""
    while pos < len(data):
        length, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise SystemExit("icon.png must be 8-bit RGB/RGBA and not interlaced")
    if w != h:
        raise SystemExit("icon.png must be square")
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    for y in range(h):
        ftype = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if ftype == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ftype == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 255
        elif ftype == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        if bpp == 4:
            out[y * w * 4:(y + 1) * w * 4] = line
        else:
            for x in range(w):
                out[(y * w + x) * 4:(y * w + x) * 4 + 4] = line[x * 3:x * 3 + 3] + b"\xff"
        prev = line
    return w, out


def premultiplied(size, rgba):
    px = []
    for i in range(0, size * size * 4, 4):
        a = rgba[i + 3] / 255.0
        px.append((rgba[i] * a, rgba[i + 1] * a, rgba[i + 2] * a, rgba[i + 3]))
    return px


def resize(src_size, px, size):
    """Area-average resample a square premultiplied image to size x size (separable box filter)."""
    scale = src_size / float(size)

    def weights(i):
        lo, hi = i * scale, (i + 1) * scale
        return [(j, min(hi, j + 1) - max(lo, j)) for j in range(int(lo), min(src_size, int(hi + 0.999999)))
                if min(hi, j + 1) - max(lo, j) > 0]
    cols = [weights(i) for i in range(size)]
    rows_tmp = []
    for y in range(src_size):
        row = px[y * src_size:(y + 1) * src_size]
        out = []
        for ws in cols:
            r = g = b = a = 0.0
            for j, wgt in ws:
                p = row[j]
                r += p[0] * wgt; g += p[1] * wgt; b += p[2] * wgt; a += p[3] * wgt
            out.append((r / scale, g / scale, b / scale, a / scale))
        rows_tmp.append(out)
    result = []
    for ws in cols:
        line = []
        for x in range(size):
            r = g = b = a = 0.0
            for j, wgt in ws:
                p = rows_tmp[j][x]
                r += p[0] * wgt; g += p[1] * wgt; b += p[2] * wgt; a += p[3] * wgt
            r, g, b, a = r / scale, g / scale, b / scale, a / scale
            if a > 0:   # un-premultiply
                k = 255.0 / a
                r, g, b = r * k, g * k, b * k
            line.append(tuple(max(0, min(255, int(v + 0.5))) for v in (r, g, b, a)))
        result.append(line)
    return result


def shrink(src_size, px, factor):
    """Exact integer box downsample, used to bring a large source close to icon sizes quickly."""
    size = src_size // factor
    out = []
    n = float(factor * factor)
    for y in range(size):
        for x in range(size):
            r = g = b = a = 0.0
            for yy in range(y * factor, (y + 1) * factor):
                base = yy * src_size
                for xx in range(x * factor, (x + 1) * factor):
                    p = px[base + xx]
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]
            out.append((r / n, g / n, b / n, a / n))
    return size, out


def dib_bytes(rows):
    """32-bit BGRA DIB as stored inside .ico files: bottom-up, double height, plus a 1-bit AND mask."""
    size = len(rows)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b"".join(bytes((b, g, r, a)) for row in reversed(rows) for (r, g, b, a) in row)
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(rows):
        bits = bytearray(stride)
        for x, px in enumerate(row):
            if px[3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + pixels + bytes(mask)


def main():
    src = os.path.join(ROOT, "res", "icon.png")
    size, rgba = read_png(src)
    px = premultiplied(size, rgba)
    while size >= 256 and size % 2 == 0:      # halve large sources first; exact and much faster
        size, px = shrink(size, px, 2)
    images = [dib_bytes(resize(size, px, s)) for s in SIZES]
    out = struct.pack("<HHH", 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    for s, img in zip(SIZES, images):
        out += struct.pack("<BBBBHHII", s, s, 0, 0, 1, 32, len(img), offset)
        offset += len(img)
    with open(os.path.join(ROOT, "res", "icon.ico"), "wb") as f:
        f.write(out + b"".join(images))
    print("wrote res\\icon.ico (%s px) from res\\icon.png" % ", ".join(str(s) for s in SIZES))


if __name__ == "__main__":
    main()
