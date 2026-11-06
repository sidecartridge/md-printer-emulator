#!/usr/bin/env python3
"""pdfcheck.py: checks a PDF the renderer wrote against the pages it drew.

Usage:  pdfcheck.py [--dpi X Y] FILE.pdf PAGES
        PAGES is render's output for the same stream ("page N WxH MD5" per
        line). Its structure must be exact, since a reader that repairs a
        file without a word (Acrobat) would hide a fault: startxref finds the
        cross-reference table, each object is where the table says, the
        trailer's /Size is the table's, and each stream holds as many bytes
        as its /Length says. pdfinfo (poppler) must read the PDF without a
        word on stderr, with as many pages, each the size of its pixels at
        240 x 216 dpi (the FX-80's; --dpi 300 300 for the LaserJet's).
        Each image must inflate (zlib, its Adler-32 checked) to the page's
        pixels, and pdfimages must decode it to them too, its PBM's unused
        bits at the end of each row aside. Prints ok or FAIL and why.
"""
import hashlib
import re
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

DPI_X, DPI_Y = 240, 216


def fail(why):
    print(f"FAIL {why}")
    sys.exit(1)


def check_structure(data):
    start = int(re.search(rb"startxref\n(\d+)\n%%EOF\n$", data).group(1))
    m = re.compile(rb"xref\n0 (\d+)\n").match(data, start)
    if not m:
        fail(f"startxref {start} does not find the cross-reference table")
    size = int(m.group(1))
    table = data[m.end():m.end() + 20 * size]
    offsets = {}
    for n in range(size):
        entry = table[20 * n:20 * n + 20]
        if not re.fullmatch(rb"\d{10} \d{5} [nf] \n", entry):
            fail(f"cross-reference entry {n} is {entry!r}")
        if entry[17:18] == b"n":
            offset = int(entry[:10])
            if not data.startswith(b"%d 0 obj\n" % n, offset):
                fail(f"object {n} is not at {offset}")
            offsets[n] = offset
    trailer = re.compile(rb"trailer\n<< /Size (\d+) ").match(data, m.end() + 20 * size)
    if not trailer or int(trailer.group(1)) != size:
        fail(f"the trailer's /Size is not the table's {size}")
    for m in re.finditer(rb"/Length (\d+)( 0 R)? >>\nstream\n", data):
        length = int(m.group(1))
        if m.group(2):
            length = int(re.compile(rb"\d+ 0 obj\n(\d+)\nendobj").match(data, offsets[length]).group(1))
        if not data.startswith(b"\nendstream", m.end() + length):
            fail(f"the stream at {m.end()} is not {length} bytes")


def main():
    global DPI_X, DPI_Y
    args = sys.argv[1:]
    if args[:1] == ["--dpi"]:
        DPI_X, DPI_Y = int(args[1]), int(args[2])
        args = args[3:]
    if len(args) != 2:
        sys.exit(__doc__)
    pdf, pages = args
    want = []
    for line in open(pages):
        _, _, size, md5 = line.split()
        w, h = map(int, size.split("x"))
        want.append((w, h, md5))

    check_structure(Path(pdf).read_bytes())
    info = subprocess.run(["pdfinfo", "-f", "1", "-l", str(len(want)), pdf],
                          capture_output=True, text=True)
    if info.returncode or info.stderr.strip():
        fail(f"pdfinfo: {info.stderr.strip() or info.returncode}")
    count = int(re.search(r"^Pages:\s+(\d+)", info.stdout, re.M).group(1))
    if count != len(want):
        fail(f"{count} pages, not {len(want)}")
    sizes = re.findall(r"^Page\s+\d+ size:\s+([\d.]+) x ([\d.]+) pts", info.stdout, re.M)
    for n, ((w, h, _), (pw, ph)) in enumerate(zip(want, sizes), 1):
        if abs(float(pw) - w * 72 / DPI_X) > 0.01 or abs(float(ph) - h * 72 / DPI_Y) > 0.01:
            fail(f"page {n} is {pw} x {ph} pts, not {w}x{h} pixels at {DPI_X} x {DPI_Y} dpi")

    data = Path(pdf).read_bytes()
    streams = re.finditer(rb"/Subtype /Image .*?>>\nstream\n", data)
    inflated = []
    for m in streams:
        end = data.index(b"\nendstream", m.end())
        try:
            inflated.append(hashlib.md5(zlib.decompress(data[m.end():end])).hexdigest())
        except zlib.error as e:
            fail(f"image {len(inflated) + 1}: {e}")
    if inflated != [md5 for _, _, md5 in want]:
        fail(f"the images inflate to other pixels: {inflated}")

    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["pdfimages", pdf, f"{tmp}/img"], check=True)
        decoded = []
        for pbm in sorted(Path(tmp).glob("img-*.pbm")):
            head, size, pixels = pbm.read_bytes().split(b"\n", 2)
            w, h = map(int, size.split())
            stride = (w + 7) // 8
            pixels = bytearray(pixels)
            unused = (0xff << (stride * 8 - w)) & 0xff
            for row in range(h):
                pixels[row * stride + stride - 1] &= unused
            decoded.append(hashlib.md5(pixels).hexdigest())
    if decoded != [md5 for _, _, md5 in want]:
        fail(f"pdfimages decodes other pixels: {decoded}")
    print(f"ok   {count} pages, {len(data)} bytes")


if __name__ == "__main__":
    main()
