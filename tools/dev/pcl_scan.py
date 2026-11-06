#!/usr/bin/env python3
"""pcl_scan.py: which PCL commands a printer stream uses.

Walks a stream as an HP LaserJet reads it and counts each control code and
escape sequence, skipping the data of the commands that carry some (raster
rows, downloaded fonts and characters, transparent print), so their bytes are
never taken for commands. A parameterized sequence (ESC, a parameter
character, a group character, then values each ended by a letter: lowercase
when another follows, as in ESC &l1o66P) is counted per command, ESC &l#O,
with the values it was given. Printable ASCII is counted as "(text)", each
byte above 127 on its own.

    pcl_scan.py FILE...            # a table per file
    pcl_scan.py --sum FILE...      # one table for all of them
"""

import argparse
import collections
import sys

CONTROL = {0x08: "BS", 0x09: "HT", 0x0A: "LF", 0x0C: "FF", 0x0D: "CR", 0x0E: "SO",
           0x0F: "SI"}

# Parameterized commands whose value is a count of data bytes that follow.
DATA = {"*bW", "*bV", "(sW", ")sW", "&pX", "*vW", "*mW", "*cW", "(fW", "*lW",
        "&bW", "*oW", "*gW"}

VALUES_SHOWN = 8  # distinct values listed per command


def scan(data, counts, values):
    """Counts the commands of data; returns the offset of an unknown one, or None."""
    i = 0
    n = len(data)
    while i < n:
        b = data[i]
        if b != 0x1B:
            if 0x20 <= b < 0x7F:
                counts["(text)"] += 1
            elif b in CONTROL:
                counts[CONTROL[b]] += 1
            else:
                counts[f"0x{b:02X}"] += 1
            i += 1
            continue
        if i + 1 >= n:
            return i
        c = chr(data[i + 1])
        if "\x30" <= c <= "\x7e":  # two-character: ESC E, ESC 9, ESC =, ...
            counts[f"ESC {c}"] += 1
            i += 2
            continue
        if not "\x21" <= c <= "\x2f" or i + 2 >= n:
            return i
        group = chr(data[i + 2])
        j = i + 3
        if not "\x60" <= group <= "\x7e":  # no group character: ESC (8U, ESC (s...
            group = ""
            j = i + 2
        while True:  # values, each ended by a letter
            start = j
            while j < n and chr(data[j]) in "+-.0123456789":
                j += 1
            if j >= n:
                return i
            value = data[start:j].decode() or "0"
            end = chr(data[j])
            j += 1
            if not ("\x40" <= end <= "\x5e" or "\x60" <= end <= "\x7e"):
                return i
            key = f"{c}{group}{end.upper()}"
            name = f"ESC {c}{group}#{end.upper()}"
            counts[name] += 1
            values[name][value] += 1
            if key in DATA:
                j += int(float(value))
            if "\x40" <= end <= "\x5e":  # uppercase ends the sequence
                break
        i = j
    return None


def table(counts, values):
    for name, count in sorted(counts.items(), key=lambda kv: (kv[0].startswith("("), kv[0])):
        shown = ""
        if name in values:
            vals = values[name]
            top = sorted(vals.items(), key=lambda kv: -kv[1])[:VALUES_SHOWN]
            shown = "  " + ", ".join(f"{v}" + (f" x{k}" if k > 1 else "") for v, k in top)
            if len(vals) > VALUES_SHOWN:
                shown += f", ... ({len(vals)} values)"
        print(f"  {count:8d}  {name}{shown}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sum", action="store_true", help="one table for all files")
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    total = collections.Counter()
    total_values = collections.defaultdict(collections.Counter)
    bad = 0
    for path in args.files:
        counts = total if args.sum else collections.Counter()
        values = total_values if args.sum else collections.defaultdict(collections.Counter)
        data = open(path, "rb").read()
        stop = scan(data, counts, values)
        if stop is not None:
            print(f"{path}: unknown sequence at offset {stop}: {data[stop:stop + 12]!r}")
            bad = 1
        if not args.sum:
            print(f"{path} ({len(data)} bytes)")
            table(counts, values)
    if args.sum:
        table(total, total_values)
    return bad


if __name__ == "__main__":
    sys.exit(main())
