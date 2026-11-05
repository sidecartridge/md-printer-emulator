#!/usr/bin/env python3
"""The FX-80 renderer's own test streams, one page or two each, for the host
test (test.sh): the commands the ST's test pages (tools/dev/st/
make_escp_pages.py) leave out.

  MOVES.PRN     the head and the paper: CR alone, BS overstrike, tabs set
                with ESC D, vertical tabs (ESC B, VT), margins (ESC l,
                ESC Q) and the line that wraps at the right one, ESC J and
                the reverse feed ESC j, CAN and DEL, DC3/DC1, the form
                length (ESC C) and skipping the perforation (ESC N)
  NATIONAL.PRN  the twelve codes of every international set, ESC R 0-9
  MODES.PRN     proportional, master select's combinations and their
                priorities, RAM characters (ESC &, ESC %, ESC :), the 8th
                bit (ESC =, ESC >, ESC #), ESC 7, expanded with SO and DC4

Usage:  streams.py OUTDIR
"""
import sys
from pathlib import Path

ESC = b"\x1b"
CR, LF, FF = b"\r", b"\n", b"\x0c"
CRLF = CR + LF


def line(text):
    return text.encode("latin-1") + CRLF


def moves():
    out = ESC + b"@" + line("MOVES.PRN: the head and the paper.") + CRLF
    out += b"CR alone: xxxxxxxxxx" + CR + line("          overprinted")
    out += b"BS overstrike: O" + b"\x08" + b"/ a" + b"\x08" + b"` e" + b"\x08" + b"^" + CRLF
    out += ESC + b"D" + bytes([25, 35, 50, 0]) + b"Tabs at 25, 35, 50:" + b"\x09A\x09B\x09C" + CRLF
    out += ESC + b"D" + bytes(range(8, 81, 8)) + b"\x00"  # back to every eight
    out += b"Default-like tabs:\x09one\x09two\x09three" + CRLF
    out += ESC + b"l" + bytes([10]) + line("Left margin at column 10.")
    out += ESC + b"Q" + bytes([40]) + line("Right margin at column 40: this sentence is long enough to wrap twice at it.")
    out += ESC + b"l" + bytes([0]) + ESC + b"Q" + bytes([80])
    out += b"ESC J 36 (1/6 inch, no CR):" + ESC + b"J" + bytes([36]) + line("here")
    out += b"ESC j 18 back up:" + ESC + b"J" + bytes([36]) + ESC + b"j" + bytes([18]) + line(" raised half a line")
    out += b"CAN:" + b"\x09" + b"cancelled text" + b"\x18" + line("kept, at the tab")
    out += b"DEL: wrongX" + b"\x7f" + line(" right")
    out += b"DC3: " + b"\x13" + b"not printed" + b"\x11" + line("printed")
    out += ESC + b"B" + bytes([24, 30, 0]) + line("Vertical tabs at lines 24 and 30:")
    out += b"\x0b" + line("at line 24") + b"\x0b" + line("at line 30")
    out += FF
    # Page 2 is a short form: 2 inches, and a perforation skip of 2 lines.
    out += ESC + b"C" + b"\x00" + bytes([2]) + ESC + b"N" + bytes([2])
    for n in range(1, 13):
        out += line(f"Short form, 2 inches, skip 2: line {n}")
    out += ESC + b"@" + FF
    return out


def national():
    out = ESC + b"@" + line("NATIONAL.PRN: the international sets (ESC R n), on # $ @ [ \\ ] ^ ` { | } ~") + CRLF
    names = ["USA", "France", "Germany", "United Kingdom", "Denmark", "Sweden",
             "Italy", "Spain", "Japan", "Norway"]
    for n, name in enumerate(names):
        out += ESC + b"R" + bytes([n]) + f"{n} {name:15}".encode() + b" ".join(bytes([c]) for c in b"#$@[\\]^`{|}~") + CRLF
    out += ESC + b"R" + bytes([0]) + FF
    return out


def modes():
    out = ESC + b"@" + line("MODES.PRN: the remaining modes.") + CRLF
    out += ESC + b"p1" + line("Proportional: Illinois minimum, wide WM.") + ESC + b"p0"
    for n, name in ((0x01, "elite"), (0x04, "compressed"), (0x05, "elite+compressed = elite"),
                    (0x08, "emphasized"), (0x09, "elite+emphasized = elite"),
                    (0x0C, "compressed+emphasized = both"), (0x10, "double-strike"),
                    (0x20, "expanded"), (0x24, "compressed expanded")):
        out += ESC + b"!" + bytes([n]) + line(f"ESC ! {n}: {name}")
    out += ESC + b"!" + b"\x00"
    # Two RAM characters: a box at 'A' (top pins) and a hook at 'g' (descender).
    box = bytes([0x8B, 0xFE, 0, 0x82, 0, 0x82, 0, 0x82, 0, 0xFE, 0, 0])
    hook = bytes([0x0B, 0x00, 0, 0x01, 0, 0x01, 0, 0x01, 0, 0xFE, 0, 0])
    out += ESC + b"&" + bytes([0, ord("A"), ord("A")]) + box
    out += ESC + b"&" + bytes([0, ord("g"), ord("g")]) + hook
    out += ESC + b"%" + bytes([1, 0]) + line("RAM: A g (a box, a hook), others blank")
    out += ESC + b":" + bytes([0, 0, 0]) + line("ROM copied to RAM: A g as the font")
    out += ESC + b"%" + bytes([0, 0]) + line("ROM again: A g")
    out += ESC + b">" + line("ESC >, 8th bit set: abc") + ESC + b"="
    out += line("ESC =, 8th bit cleared: " + bytes([0xC1, 0xC2, 0xC3]).decode("latin-1")) + ESC + b"#"
    out += ESC + b"7" + b"ESC 7, 0x8D a CR: before" + b"\x8d" + b"after" + ESC + b"6" + CRLF
    out += b"\x0e" + b"SO expanded, DC4 back" + b"\x14" + line(" to normal")
    out += ESC + b"W1" + b"ESC W1 survives CR" + CR + LF + line("still expanded") + ESC + b"W0"
    out += FF
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, data in (("MOVES.PRN", moves()), ("NATIONAL.PRN", national()), ("MODES.PRN", modes())):
        (out / name).write_bytes(data)
        print(f"{out / name}: {len(data)} bytes")


if __name__ == "__main__":
    main()
