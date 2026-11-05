#!/usr/bin/env python3
"""The FX-80 renderer's font (rp/src/fx80/font.txt): checked, and drawn as a
specimen.

  font.py check [FONT]              the glyphs keep the printer's rules
  font.py specimen OUT.png [TEXT]   TEXT in draft, emphasized, double-strike
                                    and both, at 1,200 dpi, a dot 1/72 inch
  font.py chart OUT.png             every glyph from 0x20 to 0xff, sixteen to
                                    a line, in draft
  font.py c [OUT.c]                 the font as C (rp/src/fx80/font.c)

The rules: nine rows of eleven positions, '#' or '.'; no pin fired at two
neighbouring positions; the last two positions blank, unless the row fires
every main column to join the next character (the underline: a character is
twelve positions wide, so the next one starts two positions on); the top or
the bottom row blank (a glyph uses eight pins). Emphasized prints every dot again 1/240
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
JOIN = "#." * 5 + "#"   # a row that joins the next character's: every main column
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
            elif "#" in row[COLS - 2:] and row != JOIN:
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


def draw(glyphs, out, lines, line=0.25, label=None):
    """lines: [(codes, dx, dy)], each in its own line, every dot printed again
    dx inch to the right and dy inch lower (emphasized, double-strike)."""
    margin = 0.25
    width = round((2 * margin + max(len(c) for c, _, _ in lines) / 10) * DPI)
    height = round((2 * margin + len(lines) * line) * DPI)
    pixels = bytearray(b"\xff") * (width * height)
    r = DOT / 2

    def dot(x, y):
        for py in range(int(y - r), int(y + r) + 2):
            for px in range(int(x - r), int(x + r) + 2):
                if (px - x) ** 2 + (py - y) ** 2 <= r * r and 0 <= px < width and 0 <= py < height:
                    pixels[py * width + px] = 0

    for n, (codes, dx, dy) in enumerate(lines):
        top = margin + n * line
        for i, code in enumerate(codes):
            for row, bits in enumerate(glyphs.get(code, ())):
                for col, bit in enumerate(bits):
                    if bit != "#":
                        continue
                    x = (margin + i / 10 + col / 120) * DPI + r
                    y = (top + row / 72) * DPI + r
                    for ox, oy in {(0, 0), (dx, 0), (0, dy), (dx, dy)}:
                        dot(x + ox * DPI, y + oy * DPI)
    write_png(out, width, height, pixels)
    print(f"{out}: {width} x {height}" + (f", {label}" if label else ""))


def specimen(glyphs, out, text):
    modes = [("draft", 0, 0), ("emphasized", 1 / 240, 0), ("double-strike", 0, 1 / 216),
             ("both", 1 / 240, 1 / 216)]
    codes = [ord(c) for c in text]
    draw(glyphs, out, [(codes, dx, dy) for _, dx, dy in modes],
         label=", ".join(n for n, _, _ in modes))
    missing = sorted({c for c in text if ord(c) not in glyphs})
    if missing:
        print(f"no glyph for {''.join(missing)!r}")


def chart(glyphs, out):
    lines = []
    for row in range(0x20, 0x100, 16):
        codes = []
        for c in range(row, row + 16):
            codes += [c, 0x20]
        lines.append((codes, 0, 0))
    draw(glyphs, out, lines, line=1 / 6, label="0x20-0xff, sixteen to a line")


def to_c(glyphs):
    """Eleven columns per code, bit n the pin n + 1 (bit 0 the top pin); codes
    with no glyph are blank."""
    last = max(glyphs)
    out = ["// The FX-80 renderer's font, from font.txt by tools/printer/font.py c:",
           "// do not edit. Eleven columns per code, 1/120 inch apart at 10 cpi;",
           "// bit n of a column fires pin n + 1 (bit 0 the top pin).",
           "#include \"fx80.h\"", "",
           f"const uint16_t fx80_font[FX80_FONT_CODES][FX80_GLYPH_COLUMNS] = {{"]
    for code in range(last + 1):
        rows = glyphs.get(code)
        if rows is None:
            continue
        cols = [sum(1 << pin for pin in range(ROWS) if rows[pin][col] == "#") for col in range(COLS)]
        out.append(f"    [0x{code:02x}] = {{" + ", ".join(f"0x{c:03x}" for c in cols) + "},")
    out.append("};")
    return "\n".join(out) + "\n", last + 1


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ("check", "specimen", "chart", "c"):
        sys.exit(__doc__)
    if args[0] == "check":
        path = Path(args[1]) if len(args) > 1 else FONT
        glyphs, errors = load(path)
        for e in errors:
            print(f"{path}: {e}")
        print(f"{path}: {len(glyphs)} glyphs, " + ("FAIL" if errors else "ok"))
        sys.exit(1 if errors else 0)
    glyphs, errors = load(FONT)
    if errors:
        sys.exit("\n".join(errors))
    if args[0] == "c":
        text, codes = to_c(glyphs)
        out = Path(args[1]) if len(args) > 1 else FONT.with_name("font.c")
        out.write_text(text)
        print(f"{out}: {codes} codes")
        return
    if len(args) < 2:
        sys.exit(__doc__)
    if args[0] == "chart":
        chart(glyphs, args[1])
    else:
        specimen(glyphs, args[1], args[2] if len(args) > 2 else SAMPLE)


if __name__ == "__main__":
    main()
