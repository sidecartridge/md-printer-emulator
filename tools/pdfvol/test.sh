#!/usr/bin/env bash
#
# test.sh: the PDF volume's host test (macOS). Builds volume (the firmware's
# own C in rp/src/pdfvol), makes a folder for each case, writes its volume as
# an image, attaches the image read-only, and checks it: fsck_msdos finds
# nothing to fix, the volume lists exactly the folder's nnnnnnnn.pdf files
# (lowercase, as the card's), and each holds the same bytes and the same
# date. Ends in PASS or FAIL.
#
# Cases: no PDF; one; 1,500 (the root folder takes two clusters); one of
# 40 MB; files that are not finished PDFs beside them, which must not show;
# and more than the volume holds, where the lowest numbers are left out.
#
# Usage:  tools/pdfvol/test.sh
set -Eeuo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/tools/dev/builds/pdfvol"
VOLUME="$BUILD/volume"
make -s -C "$HERE"
rm -rf "$BUILD/cases"
mkdir -p "$BUILD/cases"
fail=0

pdf() {  # pdf FOLDER NUMBER SIZE: a file of SIZE random bytes
  head -c "$3" /dev/urandom > "$(printf '%s/%08d.pdf' "$1" "$2")"
}

# check NAME [SKIP_DATA]: the volume of cases/NAME against the folder
check() {
  local name="$1" folder="$BUILD/cases/$1" image="$BUILD/cases/$1.img"
  local mnt="$BUILD/cases/$1.mnt" why="" dev
  "$VOLUME" "$folder" "$image" > "$BUILD/cases/$name.out"
  dev="$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -readonly -nomount "$image" |
    awk 'NR == 1 { print $1 }')"
  if ! fsck_msdos -n "$dev" > "$BUILD/cases/$name.fsck" 2>&1; then
    why="fsck_msdos: $(tail -1 "$BUILD/cases/$name.fsck")"
  fi
  mkdir -p "$mnt"
  if [ -z "$why" ] && ! mount -t msdos -o rdonly "$dev" "$mnt" 2> /dev/null; then
    why="it does not mount"
  fi
  if [ -z "$why" ]; then
    why="$(python3 - "$folder" "$mnt" "${2:-}" <<'PY'
import hashlib, os, re, sys, time
folder, mnt, skip_data = sys.argv[1:]
want = sorted(n.lower() for n in os.listdir(folder)
              if re.fullmatch(r"\d{8}\.pdf", n, re.I) and os.path.isfile(os.path.join(folder, n)))
if skip_data:  # only the highest numbers fit
    fitted, used = [], 0
    for n in reversed(want):
        size = os.path.getsize(os.path.join(folder, n))
        used += -(-size // 32768)
        if used > 65600 - 1:
            break
        fitted.append(n)
    want = sorted(fitted)
got = sorted(os.listdir(mnt))
if got != want:
    sys.exit(print(f"it lists {got[:4]}... ({len(got)}), not {want[:4]}... ({len(want)})"))
for n in want if not skip_data else []:
    src = next(m for m in os.listdir(folder) if m.lower() == n)
    a, b = os.path.join(folder, src), os.path.join(mnt, n)
    if hashlib.sha256(open(a, "rb").read()).digest() != hashlib.sha256(open(b, "rb").read()).digest():
        sys.exit(print(f"{n} holds other bytes"))
    ta, tb = os.path.getmtime(a), os.path.getmtime(b)
    if time.localtime(ta).tm_year >= 1980 and abs(ta - tb) > 2:
        sys.exit(print(f"{n} is dated {time.ctime(tb)}, not {time.ctime(ta)}"))
PY
)"
    umount "$mnt"
  fi
  hdiutil detach -quiet "$dev"
  rm -f "$image"
  if [ -n "$why" ]; then
    echo "FAIL $name: $why"
    fail=1
  else
    echo "ok   $name: $(cat "$BUILD/cases/$name.out")"
  fi
}

c="$BUILD/cases"
mkdir -p "$c/none" "$c/one" "$c/many" "$c/large" "$c/others" "$c/full"
check none
pdf "$c/one" 1 12957
check one
for n in $(seq 1 1500); do pdf "$c/many" "$n" $((RANDOM * 3 + 1)); done
check many
pdf "$c/large" 7 41943040
check large
pdf "$c/others" 3 5000
pdf "$c/others" 4 32768  # exactly a cluster
pdf "$c/others" 5 32769
head -c 100 /dev/urandom > "$c/others/00000009.PDF"
for other in 00000006.PRN 00000007.TMP 00000008.PDT 1.pdf 123456789.pdf abcdefgh.pdf SZ2_S6.ST8; do
  head -c 100 /dev/urandom > "$c/others/$other"
done
mkdir "$c/others/00000010.pdf"  # a folder, not a file
check others
for n in 1 2 3; do mkfile -n 900m "$(printf '%s/%08d.pdf' "$c/full" "$n")"; done
check full skip-data

if [ "$fail" = 0 ]; then echo PASS; else echo FAIL; exit 1; fi
