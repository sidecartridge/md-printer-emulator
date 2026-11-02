#!/usr/bin/env python3
"""A plain .ST image from a Pasti (.STX) one, so its files can be copied with
mtools. Each track's sectors are written in order, 512 bytes each; a sector
the FDC read with an error, a missing one or one of another size is reported
(protection usually lives there) and its place left zero: the copy is for
the files, which a protection's tracks past the file system leave whole. The
.STX is only read.

Usage:  stx_to_st.py IN.STX OUT.ST [--sides 1|2] [--sectors N]
        --sides and --sectors default to what the image's tracks hold.

Pasti's layout (Jean Louis-Guerin's description of the format): a 16-byte
file header ("RSY\\0", version, tool, track count), then per track a 16-byte
record (record size, fuzzy byte count, sector count, flags, track length,
track number | side << 7, track type), the sector descriptors when flags bit 0
is set (16 bytes each: data offset, bit position, read time, ID track, side,
number and size, ID CRC, FDC status, reserved), the fuzzy mask, then the
track data; a sector's data starts at its offset from the track data's start.
"""

import argparse
import struct
import sys


def tracks(data):
    if data[:4] != b"RSY\0":
        sys.exit("not a Pasti image")
    count = data[10]
    pos = 16
    for _ in range(count):
        size, fuzzy, nsec, flags, tlen, tnum, ttype = struct.unpack_from("<IIHHHBB", data, pos)
        body = pos + 16
        sectors = {}
        if flags & 1:
            descs = body
            data_start = descs + 16 * nsec + fuzzy
            for s in range(nsec):
                off, bitpos, rtime, idt, ids, idn, idsz, crc, status, _ = \
                    struct.unpack_from("<IHHBBBBHBB", data, descs + 16 * s)
                length = 128 << (idsz & 3)
                start = data_start + off
                sectors[idn] = (data[start:start + length], status, idsz)
        else:  # a standard track: the sectors one after the other
            for s in range(nsec):
                start = body + 512 * s
                sectors[s + 1] = (data[start:start + 512], 0, 2)
        yield tnum & 0x7F, tnum >> 7, sectors
        pos += size


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("stx")
    ap.add_argument("st")
    ap.add_argument("--sides", type=int)
    ap.add_argument("--sectors", type=int)
    args = ap.parse_args()
    data = open(args.stx, "rb").read()
    found = {(t, side): secs for t, side, secs in tracks(data)}
    sides = args.sides or (2 if any(side for _, side in found) else 1)
    nsec = args.sectors or max(len([n for n in secs if 1 <= n <= 11]) for secs in found.values())
    ntracks = max(t for t, _ in found) + 1
    out = bytearray(ntracks * sides * nsec * 512)
    problems = 0
    for t in range(ntracks):
        for side in range(sides):
            secs = found.get((t, side), {})
            for n in range(1, nsec + 1):
                where = ((t * sides + side) * nsec + n - 1) * 512
                if n not in secs:
                    print(f"track {t} side {side} sector {n}: missing")
                    problems += 1
                    continue
                body, status, size = secs[n]
                if status & 0x18 or size != 2 or len(body) != 512:
                    print(f"track {t} side {side} sector {n}: FDC status {status:#04x}, size code {size}")
                    problems += 1
                out[where:where + len(body[:512])] = body[:512]
    open(args.st, "wb").write(out)
    print(f"{args.st}: {ntracks} tracks, {sides} side(s), {nsec} sectors; {problems} sector(s) not plain")
    return 0


if __name__ == "__main__":
    sys.exit(main())
