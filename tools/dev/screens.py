"""Hatari's screenshots, read and compared. Standard library only.

A screenshot is Hatari's PNG of the whole display, borders included: 8-bit
palette (what Hatari writes), RGB or RGBA, any of PNG's five row filters.
"""
import struct
import zlib
from pathlib import Path


class Screen:
    def __init__(self, width, height, rows):
        self.width, self.height, self.rows = width, height, rows  # rows: RGB bytes

    def pixel(self, x, y):
        row = self.rows[y]
        return tuple(row[3 * x:3 * x + 3])


def load(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, width, palette = 8, b"", 0, b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            if depth != 8 or color not in (2, 3, 6):
                raise ValueError(f"{path}: depth {depth} color type {color} not handled")
            bpp = {2: 3, 3: 1, 6: 4}[color]
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if bpp == 3:
            rows.append(bytes(line))
        elif bpp == 1:
            rows.append(b"".join(palette[3 * v:3 * v + 3] for v in line))
        else:
            rows.append(bytes(v for i, v in enumerate(line) if i % 4 != 3))
    return Screen(width, height, rows)


def difference(a, b):
    """The share of pixels that differ, 0 to 1 (1 if the sizes differ)."""
    if (a.width, a.height) != (b.width, b.height):
        return 1.0
    differ = 0
    for ra, rb in zip(a.rows, b.rows):
        if ra != rb:
            differ += sum(1 for i in range(0, len(ra), 3) if ra[i:i + 3] != rb[i:i + 3])
    return differ / (a.width * a.height)


def colours(screen, step=4):
    """{rgb: share} over a grid of pixels, every step-th one each way."""
    counts, n = {}, 0
    for y in range(0, screen.height, step):
        row = screen.rows[y]
        for x in range(0, screen.width, step):
            rgb = tuple(row[3 * x:3 * x + 3])
            counts[rgb] = counts.get(rgb, 0) + 1
            n += 1
    return {rgb: c / n for rgb, c in counts.items()}
