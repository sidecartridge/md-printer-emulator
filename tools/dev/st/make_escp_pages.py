#!/usr/bin/env python3
"""The bench's ESC/P test pages: raw printer files that exercise an
Epson FX-80's text modes and bit-image densities, written on this computer and
sent to the printer byte by byte by PRFILES.PRG (prfiles.s) from a floppy.

  TEXT.PRN    every text mode: pica, elite, condensed, expanded, emphasized,
              double-strike, underline, italic, super/subscript, the line
              spacings, a tab, a form feed between two pages
  GRAPH.PRN   every bit-image density (ESC K, L, Y, Z, * 0-6, ^ 0/1), each
              printing the same 64-column ramp, and a pattern mixed with text

Usage:  make_escp_pages.py OUTDIR
"""
import sys
from pathlib import Path

ESC = b"\x1b"
CRLF = b"\r\n"


def line(text):
    return text.encode("ascii") + CRLF


def text_page():
    out = ESC + b"@"                            # reset
    out += line("TEXT.PRN: the FX-80's text modes (the printer emulator's test pages).") + CRLF
    out += line("Pica (10 cpi): The quick brown fox jumps over the lazy dog.")
    out += ESC + b"M" + line("Elite (12 cpi): The quick brown fox jumps over the lazy dog.") + ESC + b"P"
    out += b"\x0f" + line("Condensed (17 cpi): The quick brown fox jumps over the lazy dog.") + b"\x12"
    out += ESC + b"W1" + line("Expanded") + ESC + b"W0"
    out += b"\x0e" + line("Expanded for one line (SO)")
    out += ESC + b"E" + line("Emphasized: ABCDEFGHIJ abcdefghij") + ESC + b"F"
    out += ESC + b"G" + line("Double-strike: ABCDEFGHIJ abcdefghij") + ESC + b"H"
    out += ESC + b"-1" + line("Underlined text") + ESC + b"-0"
    out += ESC + b"4" + line("Italic text") + ESC + b"5"
    out += b"Normal" + ESC + b"S0" + b"super" + ESC + b"T" + b" normal" + ESC + b"S1" + b"sub" + ESC + b"T" + CRLF
    out += ESC + b"!" + bytes([0x38]) + line("Master select: emphasized, double-strike, expanded") + ESC + b"!\x00"
    out += CRLF + line("Line spacing 1/8 inch (ESC 0):")
    out += ESC + b"0" + line("line one") + line("line two") + ESC + b"2"
    out += line("Line spacing 7/72 inch (ESC 1):")
    out += ESC + b"1" + line("line one") + line("line two") + ESC + b"2"
    out += line("Line spacing 36/216 inch (ESC 3 36):")
    out += ESC + b"3" + bytes([36]) + line("line one") + line("line two") + ESC + b"2"
    out += line("Line spacing 12/72 inch (ESC A 12):")
    out += ESC + b"A" + bytes([12]) + line("line one") + line("line two") + ESC + b"2"
    out += b"A tab:\there." + CRLF
    out += line("Printable ASCII, 32 to 126:")
    out += bytes(range(32, 80)) + CRLF + bytes(range(80, 127)) + CRLF
    out += line("End of page 1; a form feed follows.") + b"\x0c"
    out += line("Page 2: after the form feed.") + b"\x0c"
    return out


def ramp(columns=64):
    """A ramp of the 8 pins: each column's byte sets one more pin."""
    return bytes(((1 << (1 + (c * 8 // columns))) - 1) & 0xFF for c in range(columns))


def bit_image(command, data):
    n = len(data)
    return command + bytes([n & 0xFF, n >> 8]) + data


def graph_page():
    out = ESC + b"@"
    out += line("GRAPH.PRN: the FX-80's bit-image densities (the printer emulator's test pages).") + CRLF
    data = ramp()
    for name, command in (("ESC K 60 dpi", ESC + b"K"), ("ESC L 120 dpi", ESC + b"L"),
                          ("ESC Y 120 dpi, fast", ESC + b"Y"), ("ESC Z 240 dpi", ESC + b"Z")):
        out += name.encode("ascii") + b": " + bit_image(command, data) + CRLF
    for m in range(7):
        out += f"ESC * {m}: ".encode("ascii") + bit_image(ESC + b"*" + bytes([m]), data) + CRLF
    for m in (0, 1):
        # 9-pin graphics: two bytes a column, the 9th pin in the second's top bit
        cols = 32
        nine = b"".join(bytes([0xFF, 0x80 if c % 2 else 0x00]) for c in range(cols))
        out += f"ESC ^ {m}: ".encode("ascii") + ESC + b"^" + bytes([m, cols & 0xFF, cols >> 8]) + nine + CRLF
    out += CRLF + line("A 3-pass pattern, 24/216 inch steps, as screen dumps print:")
    out += ESC + b"3" + bytes([24])
    for row in range(3):
        stripes = bytes((0xAA if (c // 8 + row) % 2 else 0x55) for c in range(240))
        out += bit_image(ESC + b"L", stripes) + CRLF
    out += ESC + b"2" + line("End of the graphics page.") + b"\x0c"
    return out


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    (out / "TEXT.PRN").write_bytes(text_page())
    (out / "GRAPH.PRN").write_bytes(graph_page())
    for name in ("TEXT.PRN", "GRAPH.PRN"):
        print(f"make_escp_pages.py: {out / name} ({(out / name).stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
