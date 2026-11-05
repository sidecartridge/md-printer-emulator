#!/usr/bin/env python3
"""The FX-80 renderer's font (rp/src/fx80/font.txt): checked, and drawn as a
specimen.

  font.py check [FONT]              the glyphs keep the printer's rules
  font.py specimen OUT.png [TEXT]   TEXT in draft, emphasized, double-strike
                                    and both, at 1,200 dpi, a dot 1/72 inch

The rules: nine rows of eleven positions, '#' or '.'; no pin fired at two
neighbouring positions; the last two positions blank; the top or the bottom
row blank (a glyph uses eight pins). Emphasized prints every dot again 1/240
inch to the right, double-strike every dot again 1/216 inch lower.
"""
import struct
import sys
import zlib
from pathlib import Path

FONT = Path(__file__).resolve().parents[2] / "rp/src/fx80/font.txt"
ROWS, COLS = 9, 11
DPI = 1200
DOT = DPI / 72          # pin diameter: about the 1/72 inch between pins
SAMPLE = "Hamburgefonstiv 0123456789"


def load(path):
    """{code: [row, ...]} and the errors found."""
    glyphs, errors, code, rows = {}, [], None, []

    def close():
        if code is None:
            return
        where = f"0x{code:02x}"
        if len(rows) != ROWS:
            errors.append(f"{where}: {len(rows)} rows, not {ROWS}")
            return
        for n, row in enumerate(rows, 1):
            if len(row) != COLS or set(row) - set("#."):
                errors.append(f"{where} row {n}: {row!r} is not {COLS} of '#' and '.'")
            elif "##" in row:
                errors.append(f"{where} row {n}: a pin fired at two neighbouring positions")
            elif "#" in row[COLS - 2:]:
                errors.append(f"{where} row {n}: the last two positions are not blank")
        if "#" in rows[0] and "#" in rows[-1]:
            errors.append(f"{where}: uses all nine pins (top and bottom rows)")
        glyphs[code] = rows

    for n, line in enumerate(path.read_text().splitlines(), 1):
        if not line or line.startswith(";"):
            continue
        if line.startswith("0x"):
            close()
            head = line.split(" ", 1)
            code, rows = int(head[0], 16), []
            if code in glyphs:
                errors.append(f"line {n}: 0x{code:02x} twice")
            if 0x21 <= code <= 0x7e and (len(head) < 2 or head[1] != chr(code)):
                errors.append(f"line {n}: 0x{code:02x} is {chr(code)!r}")
        elif code is None:
            errors.append(f"line {n}: a row before any glyph")
        else:
            rows.append(line)
    close()
    return glyphs, errors


def write_png(path, width, height, pixels):
    """An 8-bit greyscale PNG; pixels is a bytearray, 0 black."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + bytes(pixels[y * width:(y + 1) * width]) for y in range(height))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n"
                           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def specimen(glyphs, out, text):
    modes = [("draft", 0, 0), ("emphasized", 1 / 240, 0), ("double-strike", 0, 1 / 216),
             ("both", 1 / 240, 1 / 216)]
    margin, line = 0.25, 0.25
    width = round((2 * margin + len(text) / 10) * DPI)
    height = round((2 * margin + len(modes) * line) * DPI)
    pixels = bytearray(b"\xff") * (width * height)
    r = DOT / 2

    def dot(x, y):
        for py in range(int(y - r), int(y + r) + 2):
            for px in range(int(x - r), int(x + r) + 2):
                if (px - x) ** 2 + (py - y) ** 2 <= r * r and 0 <= px < width and 0 <= py < height:
                    pixels[py * width + px] = 0

    for m, (_, dx, dy) in enumerate(modes):
        top = margin + m * line
        for i, ch in enumerate(text):
            rows = glyphs.get(ord(ch))
            if rows is None:
                continue
            for row, bits in enumerate(rows):
                for col, bit in enumerate(bits):
                    if bit != "#":
                        continue
                    x = (margin + i / 10 + col / 120) * DPI + r
                    y = (top + row / 72) * DPI + r
                    for ox, oy in {(0, 0), (dx, 0), (0, dy), (dx, dy)}:
                        dot(x + ox * DPI, y + oy * DPI)
    write_png(out, width, height, pixels)
    missing = sorted({c for c in text if ord(c) not in glyphs and c != " "})
    print(f"{out}: {width} x {height}, {', '.join(n for n, _, _ in modes)}"
          + (f"; no glyph for {''.join(missing)!r}" if missing else ""))


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ("check", "specimen"):
        sys.exit(__doc__)
    if args[0] == "check":
        path = Path(args[1]) if len(args) > 1 else FONT
        glyphs, errors = load(path)
        for e in errors:
            print(f"{path}: {e}")
        print(f"{path}: {len(glyphs)} glyphs, " + ("FAIL" if errors else "ok"))
        sys.exit(1 if errors else 0)
    if len(args) < 2:
        sys.exit(__doc__)
    glyphs, errors = load(FONT)
    if errors:
        sys.exit("\n".join(errors))
    specimen(glyphs, args[1], args[2] if len(args) > 2 else SAMPLE)


if __name__ == "__main__":
    main()
