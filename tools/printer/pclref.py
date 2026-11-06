#!/usr/bin/env python3
"""pclref.py: a PCL stream's pages as GhostPCL prints them, beside ours.

Usage:  pclref.py [--letter] FILE
        Renders FILE with GhostPCL (gpcl6, built under tools/dev/builds/ghostpcl,
        the reference) at 300 dpi on A4 (or Letter), and prints "page N WxH MD5"
        per page, the MD5 of its rows of bits as tools/printer/render gives them,
        so the two can be compared line for line. A page of GhostPCL's that is
        blank is printed as such.
"""
import hashlib
import subprocess
import sys
import tempfile
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


def main():
    args = sys.argv[1:]
    letter = "--letter" in args
    files = [a for a in args if not a.startswith("--")]
    if len(files) != 1 or GPCL6 is None:
        sys.exit(__doc__ if GPCL6 else "pclref.py: GhostPCL is not built (tools/dev/builds/ghostpcl)")
    with tempfile.TemporaryDirectory() as tmp:
        paper = "LETTER" if letter else "A4"
        subprocess.run([str(GPCL6), "-q", "-dNOPAUSE", f"-J@PJL SET PAPER={paper}", "-sDEVICE=pbmraw",
                        "-r300", "-o", f"{tmp}/page-%03d.pbm", files[0]], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for n, page in enumerate(sorted(Path(tmp).glob("page-*.pbm")), 1):
            w, h, bits = pbm(page)
            blank = "" if any(bits) else " (blank)"
            print(f"page {n} {w}x{h} {hashlib.md5(bits).hexdigest()}{blank}")


if __name__ == "__main__":
    main()
