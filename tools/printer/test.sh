#!/usr/bin/env bash
#
# test.sh: the FX-80 renderer's host test. Builds render (the firmware's own C
# in rp/src/fx80), writes our own test streams (tools/dev/st/make_escp_pages.py
# and make_set128.py, the ST's; streams.py, the renderer's), renders each and
# compares every page's MD5 with
# golden/<stream>.pages, on A4, and golden/<stream>.letter.pages, on Letter
# (the LaserJet's too: pclstreams.py, and captures in local/pcl/),
# then again through the firmware's band of rows,
# which must draw the same pages. Each stream's PDF (rp/src/pdf) must read
# without errors and hold exactly those pages (pdfcheck.py; poppler's pdfinfo
# and pdfimages). The pages are left as PNGs and job.pdf in
# tools/dev/builds/printer/<stream>/ to look at. Also checks that font.c is
# font.txt's. Ends in PASS or FAIL.
#
# Captures of commercial programs are never committed: put each in
# tools/printer/local/ (gitignored) as NAME.prn, and the test checks it too,
# against local/NAME.pages.
#
# Usage:  tools/printer/test.sh            # compare
#         tools/printer/test.sh --update   # write the goldens from this run,
#                                          # once the pages have been checked
set -Eeuo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/tools/dev/builds/printer"
UPDATE=0
[ "${1:-}" = "--update" ] && UPDATE=1
BAND=128  # the firmware's band

make -s -C "$HERE"
mkdir -p "$BUILD/streams"
fail=0

"$HERE/font.py" c "$BUILD/font.c" > /dev/null
if ! cmp -s "$BUILD/font.c" "$ROOT/rp/src/fx80/font.c"; then
  echo "FAIL font.c is not font.txt's: run tools/printer/font.py c"
  fail=1
fi

python3 "$ROOT/tools/dev/st/make_escp_pages.py" "$BUILD/streams" > /dev/null
python3 "$ROOT/tools/dev/st/make_set128.py" "$BUILD/streams" > /dev/null
python3 "$HERE/streams.py" "$BUILD/streams" > /dev/null
# check NAME FILE GOLDEN [OPTION]: render FILE (with render's OPTION, e.g.
# --letter), compare its pages with GOLDEN
check() {
  local name="$1" file="$2" golden="$3" out="$BUILD/$1"
  local options=("${@:4}") dpi=()
  [[ " ${options[*]-} " == *" --laserjet "* ]] && dpi=(--dpi 300 300)
  mkdir -p "$out/band"
  find "$out" -name 'page-*.png' -delete
  "$BUILD/render" ${options[@]+"${options[@]}"} --pdf "$out/job.pdf" "$file" "$out" > "$out/pages"
  if [ "$UPDATE" = 1 ]; then
    mkdir -p "$(dirname "$golden")"
    cp "$out/pages" "$golden"
    echo "golden $name: $(wc -l < "$golden" | tr -d ' ') pages"
  elif [ ! -f "$golden" ]; then
    echo "FAIL $name: no golden"
    fail=1
  elif ! diff -u "$golden" "$out/pages" > "$out/diff"; then
    echo "FAIL $name: pages differ from the golden ($out/diff; PNGs in $out)"
    fail=1
  elif ! "$BUILD/render" ${options[@]+"${options[@]}"} --band "$BAND" "$file" "$out/band" \
       | diff -q - "$out/pages" > /dev/null; then
    echo "FAIL $name: a band of $BAND rows draws other pages than the whole page"
    fail=1
  elif ! pdf="$("$HERE/pdfcheck.py" ${dpi[@]+"${dpi[@]}"} "$out/job.pdf" "$out/pages")"; then
    echo "FAIL $name: its PDF: ${pdf#FAIL }"
    fail=1
  else
    echo "ok   $name ($(wc -l < "$golden" | tr -d ' ') pages, the same through a band; PDF ${pdf#ok   })"
  fi
}

for stream in TEXT.PRN GRAPH.PRN SET128.TXT MOVES.PRN NATIONAL.PRN MODES.PRN LONG.PRN; do
  check "$stream" "$BUILD/streams/$stream" "$HERE/golden/$stream.pages"
  check "$stream.letter" "$BUILD/streams/$stream" "$HERE/golden/$stream.letter.pages" --letter
done
for capture in "$HERE"/local/*.prn; do
  [ -f "$capture" ] || continue
  name="$(basename "$capture" .prn)"
  check "local/$name" "$capture" "$HERE/local/$name.pages"
done

# The LaserJet (rp/src/pcl): our PCL streams, and local captures in
# local/pcl/. Where GhostPCL is built (the reference, pclref.py), their pages
# must be its pages, pixel for pixel; MOVES.PCL's only once text is drawn.
python3 "$HERE/pclstreams.py" "$BUILD/streams" > /dev/null
reference() {  # reference NAME FILE [--letter]
  local name="$1" file="$2"
  local ours
  mkdir -p "$BUILD/$name.ref"
  ours="$("$BUILD/render" --laserjet "${@:3}" "$file" "$BUILD/$name.ref" | grep '^page')" || true
  if [ "$ours" != "$("$HERE/pclref.py" "${@:3}" "$file" | sed 's/ (blank)$//')" ]; then
    echo "FAIL $name: other pages than GhostPCL's"
    fail=1
  else
    echo "ok   $name: GhostPCL's pages"
  fi
}
for stream in RASTER.PCL MOVES.PCL PAGES.PCL; do
  check "$stream" "$BUILD/streams/$stream" "$HERE/golden/$stream.pages" --laserjet
  check "$stream.letter" "$BUILD/streams/$stream" "$HERE/golden/$stream.letter.pages" --laserjet --letter
done
for capture in "$HERE"/local/pcl/*.prn; do
  [ -f "$capture" ] || continue
  name="$(basename "$capture" .prn)"
  check "local/pcl/$name" "$capture" "$HERE/local/pcl/$name.pages" --laserjet
done
if [ "$UPDATE" = 0 ] && compgen -G "$ROOT/tools/dev/builds/ghostpcl/src/ghostpdl-*/bin/gpcl6" > /dev/null; then
  mkdir -p "$BUILD/ref"
  for stream in RASTER.PCL PAGES.PCL; do
    reference "ref/$stream" "$BUILD/streams/$stream"
    reference "ref/$stream.letter" "$BUILD/streams/$stream" --letter
  done
  for capture in "$HERE"/local/pcl/*.prn; do
    [ -f "$capture" ] || continue
    reference "ref/$(basename "$capture" .prn)" "$capture"
  done
fi

[ "$UPDATE" = 1 ] && exit 0
if [ "$fail" = 0 ]; then echo "PASS"; else echo "FAIL"; exit 1; fi
