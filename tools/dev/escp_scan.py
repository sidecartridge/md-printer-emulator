#!/usr/bin/env python3
"""escp_scan.py: which ESC/P commands a printer stream uses.

Walks a stream as a 9-pin Epson printer reads it and counts each control code
and ESC sequence, skipping the data of bit images and user-defined
characters, so their bytes are never taken for commands. A sequence it does
not know is reported with its offset: the stream cannot be followed past it.
Printable ASCII is counted as "(text)", each character above 127 on its own.

    escp_scan.py FILE...            # a table per file
    escp_scan.py --sum FILE...      # one table for all of them

Parameter counts are the FX-80's (its ESC % takes two, later printers' one),
and later ESC/P's for the commands the FX-80 does not have.
"""

import argparse
import collections
import sys

CONTROL = {0x07: "BEL", 0x08: "BS", 0x09: "HT", 0x0A: "LF", 0x0B: "VT", 0x0C: "FF",
           0x0D: "CR", 0x0E: "SO", 0x0F: "SI", 0x11: "DC1", 0x12: "DC2", 0x13: "DC3",
           0x14: "DC4", 0x18: "CAN", 0x7F: "DEL"}

# ESC + this byte, then this many parameter bytes.
FIXED = {"@": 0, "E": 0, "F": 0, "G": 0, "H": 0, "4": 0, "5": 0, "6": 0, "7": 0, "8": 0,
         "9": 0, "0": 0, "1": 0, "2": 0, "<": 0, "=": 0, ">": 0, "#": 0, "M": 0, "P": 0,
         "O": 0, "T": 0, "\x0e": 0, "\x0f": 0,
         "!": 1, "-": 1, "/": 1, "3": 1, "A": 1, "J": 1, "j": 1, "N": 1, "Q": 1, "l": 1,
         "R": 1, "S": 1, "U": 1, "W": 1, "i": 1, "m": 1, "p": 1, "s": 1, "t": 1, "x": 1,
         "k": 1, "a": 1, "r": 1, "I": 1, "\x19": 1, " ": 1, "+": 1,
         "%": 2, "?": 2, "e": 2, "f": 2, "$": 2, "\\": 2,
         ":": 3}
# ESC + this byte, n1 n2, then n1 + 256 n2 columns of data.
IMAGE = {"K": "ESC K", "L": "ESC L", "Y": "ESC Y", "Z": "ESC Z"}
TO_NUL = {"B": "ESC B", "D": "ESC D"}  # tab stops, ended by NUL


def name(byte):
    return chr(byte) if 0x20 < byte < 0x7F else f"0x{byte:02X}"


def scan(data):
    """(counts, problem): counts maps a command to its uses; problem is None, or
    where and why the walk stopped."""
    counts = collections.Counter()
    i, n = 0, len(data)

    def need(count, what):
        if i + count > n:
            raise ValueError(f"{what} runs past the end")

    try:
        while i < n:
            b = data[i]
            if b != 0x1B:
                if b in CONTROL:
                    counts[CONTROL[b]] += 1
                elif b < 0x20:
                    counts[f"0x{b:02X}"] += 1
                elif b < 0x80:
                    counts["(text)"] += 1
                else:  # each character above 127 on its own: the set decides it
                    counts[f"(char 0x{b:02X})"] += 1
                i += 1
                continue
            start = i
            need(2, "ESC")
            c = chr(data[i + 1])
            i += 2
            if c in FIXED:
                need(FIXED[c], f"ESC {name(ord(c))}")
                counts[f"ESC {name(ord(c))}"] += 1
                i += FIXED[c]
            elif c == "C":  # form length: n lines, or 0 then n inches
                need(1, "ESC C")
                if data[i] == 0:
                    need(2, "ESC C 0")
                    counts["ESC C 0"] += 1
                    i += 2
                else:
                    counts["ESC C"] += 1
                    i += 1
            elif c in IMAGE:
                need(2, IMAGE[c])
                cols = data[i] + 256 * data[i + 1]
                need(2 + cols, IMAGE[c])
                counts[IMAGE[c]] += 1
                i += 2 + cols
            elif c == "*":  # mode m, n1 n2 columns: 24-pin modes take 3 bytes each
                need(3, "ESC *")
                cols = data[i + 1] + 256 * data[i + 2]
                size = cols * (3 if data[i] >= 32 else 1)
                need(3 + size, "ESC *")
                counts[f"ESC * {data[i]}"] += 1
                i += 3 + size
            elif c == "^":  # 9-pin image: mode m, n1 n2 columns of 2 bytes
                need(3, "ESC ^")
                cols = data[i + 1] + 256 * data[i + 2]
                need(3 + 2 * cols, "ESC ^")
                counts[f"ESC ^ {data[i]}"] += 1
                i += 3 + 2 * cols
            elif c in TO_NUL:
                end = data.find(b"\x00", i)
                if end < 0:
                    raise ValueError(f"{TO_NUL[c]} has no NUL")
                counts[TO_NUL[c]] += 1
                i = end + 1
            elif c == "b":  # VT channel: n, then stops to NUL
                end = data.find(b"\x00", i + 1)
                if end < 0:
                    raise ValueError("ESC b has no NUL")
                counts["ESC b"] += 1
                i = end + 1
            elif c == "&":  # user-defined characters: 0 n m, then per character
                need(3, "ESC &")  # an attribute byte and 11 columns
                first, last = data[i + 1], data[i + 2]
                size = 3 + (last - first + 1) * 12
                need(size, "ESC &")
                counts["ESC &"] += 1
                i += size
            else:
                return counts, f"offset {start}: unknown ESC {name(ord(c))}"
    except ValueError as e:
        return counts, f"offset {start}: {e}"
    return counts, None


def table(counts):
    order = lambda k: (k.startswith("ESC"), k == "(text)", k)
    return "\n".join(f"  {k:<12} {counts[k]:>7}" for k in sorted(counts, key=order))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--sum", action="store_true", help="one table for all the files")
    args = ap.parse_args()
    total = collections.Counter()
    failed = False
    for path in args.files:
        data = open(path, "rb").read()
        counts, problem = scan(data)
        total.update(counts)
        if not args.sum:
            print(f"{path}: {len(data)} bytes")
            print(table(counts))
        if problem:
            failed = True
            print(f"{path}: stopped at {problem}", file=sys.stderr)
    if args.sum:
        print(table(total))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
