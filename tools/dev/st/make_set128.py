#!/usr/bin/env python3
"""SET128.TXT: a text file with every Atari character from 128 to 255, 16 to a
line, each line after its first code in hex. Printed by a program, it shows
what the program sends to the printer for the characters above ASCII: the
desktop's Print sends the file's bytes, a word processor's printer table may
switch the printer's international set or substitute characters.

Usage:  make_set128.py OUTDIR
"""
import sys
from pathlib import Path

CRLF = b"\r\n"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    text = b"SET128.TXT: the Atari characters 128 to 255 (a printer emulator test)." + CRLF + CRLF
    for row in range(0x80, 0x100, 16):
        text += f"{row:02X}:".encode() + b"".join(b" " + bytes([c]) for c in range(row, row + 16)) + CRLF
    text += CRLF + b"End of SET128.TXT." + CRLF
    (out / "SET128.TXT").write_bytes(text)
    print(f"{out / 'SET128.TXT'}: {len(text)} bytes")


if __name__ == "__main__":
    main()
