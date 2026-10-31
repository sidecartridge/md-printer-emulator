#!/usr/bin/env python3
"""A work copy of a program's floppy image with some files replaced, for a
test that needs the program configured otherwise (1st Word Plus with its Epson
FX table as PRINTER.CFG, say). The original image is only read; the copy is a
plain .ST (an .MSA is converted with Hatari's hmsa first). Pasti (.STX) images
cannot be copied this way.

Usage:  disk_copy.py IN OUT [--copy FROM=TO ...] [--put FILE=TO ...]
        --copy PRINTERS\\EPSON_FX.CFG=PRINTER.CFG copies a file of the disk
        over another (paths on the disk, \\ or / between folders).
        --put FILE=TO writes a file from this computer onto the copy.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEV = Path(__file__).resolve().parent
HMSA = DEV / "builds/emumd-cache/hatari-fork/build/tools/hmsa/hmsa"


def mtools(*args):
    subprocess.run(args, check=True, env=dict(os.environ, MTOOLS_SKIP_CHECK="1"),
                   stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--copy", action="append", default=[], metavar="FROM=TO")
    ap.add_argument("--put", action="append", default=[], metavar="FILE=TO")
    args = ap.parse_args()
    src = Path(args.image)
    if src.suffix.lower() == ".stx":
        ap.error("a Pasti image cannot be copied file by file")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp) / ("disk" + src.suffix.lower())
        shutil.copy(src, work)
        if work.suffix == ".msa":
            subprocess.run([str(HMSA), str(work)], check=True, stdout=subprocess.DEVNULL)
            work = work.with_suffix(".st")
        for pair in args.copy:
            frm, to = (p.replace("\\", "/") for p in pair.split("=", 1))
            mtools("mcopy", "-n", "-i", str(work), f"::/{frm}", f"{tmp}/file")
            mtools("mcopy", "-o", "-i", str(work), f"{tmp}/file", f"::/{to}")
        for pair in args.put:
            frm, to = pair.split("=", 1)
            mtools("mcopy", "-o", "-i", str(work), frm, f"::/{to.replace(chr(92), '/')}")
        shutil.copy(work, args.out)
    print(f"disk_copy.py: {args.out} (from {src.name}"
          + "".join(", " + c for c in args.copy + args.put) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
