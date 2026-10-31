#!/usr/bin/env bash
#
# make_acsi.sh: an ACSI hard-disk image for Hatari, booted by Peter Putnik's
# PPDRIVER, holding a copy of a folder from this computer. Hatari's GEMDOS
# drive is never used, since it takes the first 1 KB of the cartridge; an
# ACSI disk leaves the cartridge to the firmware.
#
# The image is made by atari-hd (https://github.com/sidecartridge/atari-hd,
# --ppdriver-bootable: one partition, the driver's boot data from a real
# PPDRIVER disk), then filled with mtools. Every file is copied back out and
# compared by SHA-256. macOS files (.DS_Store) are left out.
#
# Usage:  tools/dev/make_acsi.sh <folder> <image> [size MB, default 32]
# Environment: ATARI_HD, atari-hd's checkout (default ../atari-hd beside this
# repository).

set -Eeuo pipefail
trap 'echo "ERROR: make_acsi.sh: failed at line $LINENO" >&2' ERR

[[ $# -ge 2 && $# -le 3 ]] || { echo "usage: $0 <folder> <image> [size MB]" >&2; exit 2; }
DEV="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DEV/../.." && pwd)"
SRC="$(cd "$1" && pwd)"
OUT="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
SIZE="${3:-32}"
ATARI_HD="${ATARI_HD:-$(cd "$ROOT/.." && pwd)/atari-hd}"
[[ -f "$ATARI_HD/atari_hd.py" ]] || { echo "make_acsi.sh: no atari_hd.py in $ATARI_HD (set ATARI_HD)" >&2; exit 1; }
for tool in mcopy rsync; do
  command -v "$tool" >/dev/null || { echo "make_acsi.sh: $tool not found (mtools, rsync)" >&2; exit 1; }
done
export MTOOLS_SKIP_CHECK=1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
rsync -a --exclude .DS_Store "$SRC/" "$WORK/files/"

# atari-hd asks for the file name and a confirmation even without its TUI.
(cd "$WORK" && printf 'disk.img\ny\n' | python3 -I "$ATARI_HD/atari_hd.py" --no-tui \
  --ppdriver-bootable --size "$SIZE" --auto default --auto-n 1 > atari-hd.log)

# The partition's first sector, from the MBR's first entry (little-endian).
lba=$(python3 -I -c 'import sys; d = open(sys.argv[1], "rb").read(512)
assert d[510:512] == b"\x55\xaa", "no MBR"
print(int.from_bytes(d[0x1C6:0x1CA], "little"))' "$WORK/disk.img")
img="$WORK/disk.img@@$((lba * 512))"
if [[ -n "$(ls -A "$WORK/files")" ]]; then
  mcopy -s -m -i "$img" "$WORK/files/"* ::/
fi

# Check: everything back out, compared file by file.
mkdir "$WORK/check"
mcopy -s -n -m -i "$img" "::/*" "$WORK/check/" 2>/dev/null || true
sums() { (cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256 2>/dev/null) | tr 'a-z' 'A-Z'; }
if ! diff <(sums "$WORK/files") <(sums "$WORK/check") >/dev/null; then
  echo "make_acsi.sh: the image does not hold the same files as $SRC" >&2
  exit 1
fi
mv "$WORK/disk.img" "$OUT"
echo "make_acsi.sh: $OUT (${SIZE} MB, PPDRIVER, partition at LBA $lba, $(find "$WORK/files" -type f | wc -l | tr -d ' ') files, checked)"
