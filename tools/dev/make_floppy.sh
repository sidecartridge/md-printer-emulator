#!/usr/bin/env bash
#
# make_floppy.sh: a 720 KB double-sided floppy image (.ST, FAT12) holding a
# copy of a folder, for Hatari's drive A or B. A program in the folder's AUTO\
# runs at boot. macOS files (.DS_Store) are left out; every file is copied
# back out and compared by SHA-256.
#
# Usage:  tools/dev/make_floppy.sh <folder> <image.st> [volume label]

set -Eeuo pipefail
trap 'echo "ERROR: make_floppy.sh: failed at line $LINENO" >&2' ERR

[[ $# -ge 2 && $# -le 3 ]] || { echo "usage: $0 <folder> <image.st> [label]" >&2; exit 2; }
SRC="$(cd "$1" && pwd)"
OUT="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
LABEL="${3:-BENCH}"
command -v mformat >/dev/null || { echo "make_floppy.sh: mtools not found" >&2; exit 1; }
export MTOOLS_SKIP_CHECK=1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
rsync -a --exclude .DS_Store "$SRC/" "$WORK/files/"
# 80 tracks, 2 sides, 9 sectors: an ST's double-sided 720 KB disk.
mformat -i "$WORK/disk.st" -C -t 80 -h 2 -s 9 -v "$LABEL" ::
if [[ -n "$(ls -A "$WORK/files")" ]]; then
  mcopy -s -m -i "$WORK/disk.st" "$WORK/files/"* ::/
fi
mkdir "$WORK/check"
mcopy -s -n -m -i "$WORK/disk.st" "::/*" "$WORK/check/" 2>/dev/null || true
sums() { (cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 shasum -a 256 2>/dev/null) | tr 'a-z' 'A-Z'; }
if ! diff <(sums "$WORK/files") <(sums "$WORK/check") >/dev/null; then
  echo "make_floppy.sh: the image does not hold the same files as $SRC" >&2
  exit 1
fi
mv "$WORK/disk.st" "$OUT"
echo "make_floppy.sh: $OUT ($(find "$WORK/files" -type f | wc -l | tr -d ' ') files, checked)"
