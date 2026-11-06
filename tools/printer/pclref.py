#!/usr/bin/env python3
"""pclref.py: a PCL stream's pages as GhostPCL prints them, beside ours.

Usage:  pclref.py [--letter] FILE
        Renders FILE with GhostPCL (gpcl6, built under tools/dev/builds/ghostpcl,
        the reference) at 300 dpi on A4 (or Letter), and prints "page N WxH MD5"
        per page, the MD5 of its rows of bits as tools/printer/render gives them,
        so the two can be compared line for line. A page of GhostPCL's that is
        blank is printed as such.
        pclref.py [--letter] --layout DIR FILE
        For text, which GhostPCL draws in its own Courier: compares the pages
        render left in DIR (page-NNN.png) with GhostPCL's on a grid of CELL
        dots: a cell with ink on one must have ink in it or next to it on the
        other, so the fonts' shapes may differ but a character drawn a line or
        more away, or on one page only, is found. Prints ok or FAIL and where.
"""
import hashlib
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GPCL6 = next(ROOT.glob("tools/dev/builds/ghostpcl/src/ghostpdl-*/bin/gpcl6"), None)


def pbm(path):
    data = path.read_bytes()
    fields, i = [], 0
    while len(fields) < 3:
        j = data.index(b"\n", i)
        line, i = data[i:j], j + 1
        if not line.startswith(b"#"):
            fields += line.split()
    return int(fields[1]), int(fields[2]), data[i:]


CELL = 24  # dots: a grid cell, half a line at 6 lines an inch


def png_rows(path):
    """render's page PNG (1 bit, 0 is ink) as rows of bits, 1 for ink."""
    b = path.read_bytes()
    w, h = struct.unpack(">II", b[16:24])
    data, i = b"", 8
    while i < len(b):
        n, kind = struct.unpack(">I4s", b[i:i + 8])
        if kind == b"IDAT":
            data += b[i + 8:i + 8 + n]
        i += 12 + n
    raw = zlib.decompress(data)
    s = (w + 7) // 8 + 1
    return w, h, [bytes(~x & 0xFF for x in raw[r * s + 1:(r + 1) * s]) for r in range(h)]


def grid(w, h, rows):
    """The cells of CELL x CELL dots that hold ink."""
    cells = set()
    for y in range(h):
        row = rows[y]
        for k, v in enumerate(row):
            if v:
                for b in range(8):
                    if v >> (7 - b) & 1:
                        cells.add(((k * 8 + b) // CELL, y // CELL))
    return cells


def near(a, cells):
    """The cells of a with no inked cell of cells in or next to them."""
    return [(x, y) for x, y in a
            if not any((x + i, y + j) in cells for i in (-1, 0, 1) for j in (-1, 0, 1))]


def pbm_rows(path):
    w, h, bits = pbm(path)
    s = (w + 7) // 8
    return w, h, [bits[r * s:(r + 1) * s] for r in range(h)]


def layout(ours_dir, ref_dir):
    ours = sorted(Path(ours_dir).glob("page-*.png"))
    ref = sorted(Path(ref_dir).glob("page-*.pbm"))
    if len(ours) != len(ref):
        return f"{len(ours)} pages, GhostPCL {len(ref)}"
    for n, (a, b) in enumerate(zip(ours, ref), 1):
        ga, gb = grid(*png_rows(a)), grid(*pbm_rows(b))
        only_ours, only_ref = near(ga, gb), near(gb, ga)
        if only_ours or only_ref:
            def where(cells):
                return ", ".join(f"({x * CELL}, {y * CELL})" for x, y in sorted(cells)[:4])
            return (f"page {n}: ink where GhostPCL has none at {where(only_ours) or '-'}; "
                    f"none where it has some at {where(only_ref) or '-'} (dots)")
    return None


def main():
    args = sys.argv[1:]
    letter = "--letter" in args
    ours_dir = None
    if "--layout" in args:
        i = args.index("--layout")
        ours_dir = args[i + 1]
        del args[i:i + 2]
    files = [a for a in args if not a.startswith("--")]
    if len(files) != 1 or GPCL6 is None:
        sys.exit(__doc__ if GPCL6 else "pclref.py: GhostPCL is not built (tools/dev/builds/ghostpcl)")
    with tempfile.TemporaryDirectory() as tmp:
        paper = "LETTER" if letter else "A4"
        subprocess.run([str(GPCL6), "-q", "-dNOPAUSE", f"-J@PJL SET PAPER={paper}", "-sDEVICE=pbmraw",
                        "-r300", "-o", f"{tmp}/page-%03d.pbm", files[0]], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if ours_dir:
            why = layout(ours_dir, tmp)
            print(f"FAIL {why}" if why else "ok")
            sys.exit(1 if why else 0)
        for n, page in enumerate(sorted(Path(tmp).glob("page-*.pbm")), 1):
            w, h, bits = pbm(page)
            blank = "" if any(bits) else " (blank)"
            print(f"page {n} {w}x{h} {hashlib.md5(bits).hexdigest()}{blank}")


if __name__ == "__main__":
    main()
