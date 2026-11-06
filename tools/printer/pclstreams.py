#!/usr/bin/env python3
"""pclstreams.py: our own PCL streams for the LaserJet renderer's host test.

  RASTER.PCL  a block of a pattern at each raster resolution (75, 100, 150,
              300 dpi) in each compression mode (0 none, 1 run-length, 2 TIFF,
              3 delta row), each placed with ESC *p#X / #Y and started at the
              cursor (ESC *r1A); a row of no data (ESC *b0W), rows skipped
              (ESC *b#Y), a row wider than the page, rows past its foot
  MOVES.PCL   a small mark (a raster at the cursor) after each way of moving
              the cursor: decipoints, dots, columns, rows, absolute and
              relative; CR, LF, BS, HT, the half line feed; the HMI and VMI,
              lines an inch, the top and left margins; characters, which move
              the cursor although they are not drawn yet; commands combined
              (ESC &a10c5R)
  PAGES.PCL   the ways a page ends: a form feed, a reset with something on
              the page, line feeds past the text's foot, and with the
              perforation skip off past the paper's; the cursor after each

Usage:  pclstreams.py OUTDIR
"""
import sys
from pathlib import Path

ESC = b"\x1b"


def cmd(text):
    return ESC + text.encode()


def pattern(width, rows, seed):
    """Rows of width pixels (a multiple of 8): stripes and a frame."""
    out = []
    for r in range(rows):
        bits = []
        for c in range(width):
            frame = r in (0, rows - 1) or c in (0, width - 1)
            stripe = (c + r * (seed % 3 + 1)) % (6 + seed % 4) < 2
            bits.append(frame or stripe)
        out.append(bytes(sum(bits[i + k] << (7 - k) for k in range(8)) for i in range(0, width, 8)))
    return out


def rle(row):
    out, i = bytearray(), 0
    while i < len(row):
        n = 1
        while i + n < len(row) and row[i + n] == row[i] and n < 256:
            n += 1
        out += bytes([n - 1, row[i]])
        i += n
    return bytes(out)


def tiff(row):
    out, i = bytearray(), 0
    while i < len(row):
        n = 1
        while i + n < len(row) and row[i + n] == row[i] and n < 128:
            n += 1
        if n >= 3:
            out += bytes([257 - n, row[i]])
            i += n
            continue
        j = i
        while j < len(row) and j - i < 128 and not (j + 2 < len(row) and row[j] == row[j + 1] == row[j + 2]):
            j += 1
        out += bytes([j - i - 1]) + row[i:j]
        i = j
    return bytes(out)


def delta(row, seed):
    """Delta row: each changed run as a command byte, an offset and the bytes."""
    out, i, last = bytearray(), 0, 0
    while i < len(row):
        if i < len(seed) and row[i] == seed[i]:
            i += 1
            continue
        j = i
        while j < len(row) and j - i < 8 and not (j < len(seed) and row[j] == seed[j]):
            j += 1
        offset = i - last
        first = min(offset, 31)
        out.append(((j - i - 1) << 5) | first)
        if first == 31:
            rest = offset - 31
            while rest >= 255:
                out.append(255)
                rest -= 255
            out.append(rest)
        out += row[i:j]
        last = j
        i = j
    return bytes(out)


def raster(rows, mode, at_cursor=True):
    out = cmd(f"*b{mode}M") + cmd(f"*r{1 if at_cursor else 0}A")
    seed = b""
    for row in rows:
        data = {0: row, 1: rle(row), 2: tiff(row), 3: delta(row, seed)}[mode]
        out += cmd(f"*b{len(data)}W") + data
        seed = row
    return out + cmd("*rB")


def mark(size=2):
    """A small square at the cursor, 300 dpi, uncompressed."""
    rows = [bytes([0xFF] * size)] * (8 * size)
    return cmd("*t300R") + raster(rows, 0)


def raster_stream():
    out = cmd("E")
    for k, res in enumerate((75, 100, 150, 300)):
        for mode in range(4):
            x, y = 100 + mode * 560, 100 + k * 600
            out += cmd(f"*p{x}x{y}Y") + cmd(f"*t{res}R")
            out += raster(pattern(96, 48, k * 4 + mode), mode)
    # A row of no data, rows skipped, then rows again.
    out += cmd("*p100x2500Y") + cmd("*t150R") + cmd("*b0M") + cmd("*r1A")
    for r in range(30):
        if r == 10:
            out += cmd("*b0W")
        if r == 20:
            out += cmd("*b5Y")
        out += cmd("*b4W") + b"\xF0\x0F\xAA\x55"
    out += cmd("*rB")
    # A row wider than the page, at the left edge (ESC *r0A).
    out += cmd("*p0x2800Y") + cmd("*t300R") + raster([bytes([0xCC] * 330)] * 6, 0, at_cursor=False)
    # Rows past the page's foot.
    out += cmd("*p500x3450Y") + cmd("*t75R") + raster(pattern(64, 40, 1), 0)
    return out + b"\f"


def moves_stream():
    out = cmd("E")
    steps = [
        cmd("&a720h360V"), cmd("&a+360H"), cmd("&a-180h+90V"),
        cmd("*p300x300Y"), cmd("*p+150X"), cmd("*p-60y+30X"),
        cmd("&a10C"), cmd("&a+5C"), cmd("&a2R"), cmd("&a+1R"), cmd("&a10c5R"),
        b"\r", b"\n", b"abc", b"\b", b"\t", b"   ", cmd("="),
        cmd("&k20H") + b"xx", cmd("&l12D") + b"\n", cmd("&l4C") + b"\n",
        cmd("&l3E") + cmd("&a0R"), cmd("&a8L") + b"\r", cmd("9") + b"\r",
        cmd("&a2.5R"), cmd("&a100.5H"),
    ]
    for step in steps:
        out += step + mark()
    return out + b"\f"


def pages_stream():
    out = cmd("E") + mark() + b"\f"            # a form feed
    out += mark() + cmd("E")                    # a reset with ink on the page
    out += mark() + b"\n" * 70 + mark()         # line feeds past the text's foot
    out += cmd("&l0L") + b"\n" * 70 + mark()    # perforation skip off
    out += b"\f" + b"\f"                        # a form feed on an empty page
    out += mark()
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, data in (("RASTER.PCL", raster_stream()), ("MOVES.PCL", moves_stream()),
                       ("PAGES.PCL", pages_stream())):
        (out / name).write_bytes(data)


if __name__ == "__main__":
    main()
