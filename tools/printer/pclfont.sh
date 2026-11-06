#!/usr/bin/env bash
#
# pclfont.sh: regenerates rp/src/pcl/pclfont.c, the LaserJet's built-in fonts
# as bitmaps, with tools/printer/pclfont.c (FreeType: brew install freetype).
# Fetches the fonts it rasterises into tools/dev/builds/fonts/ the first time,
# each checked against its SHA-256: Courier Prime from Google Fonts' repository
# at a fixed commit (SIL Open Font License 1.1), and DejaVu Sans Mono 2.37,
# for the characters Courier Prime lacks (Bitstream Vera's licence). The
# licences go beside the bitmaps, as rp/src/pcl/OFL.txt and
# rp/src/pcl/DEJAVU-LICENSE.txt.
#
# Usage:  tools/printer/pclfont.sh
set -Eeuo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
FONTS="$ROOT/tools/dev/builds/fonts"
COURIER="https://raw.githubusercontent.com/google/fonts/7204f5a9b1e530633e00c67bd4954ceb7d8c2483/ofl/courierprime"
DEJAVU="https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.zip"

fetch() {  # fetch URL FILE SHA256
  if [ ! -f "$2" ] || ! echo "$3  $2" | shasum -a 256 -c - > /dev/null 2>&1; then
    mkdir -p "$(dirname "$2")"
    curl -fsSL -o "$2" "$1"
    echo "$3  $2" | shasum -a 256 -c - > /dev/null
  fi
}

fetch "$COURIER/CourierPrime-Regular.ttf" "$FONTS/courierprime/CourierPrime-Regular.ttf" \
  72f793376f8e2841656bf21d77a5de010f2929bd6956a22ee848ad0c7eb978af
fetch "$COURIER/CourierPrime-Bold.ttf" "$FONTS/courierprime/CourierPrime-Bold.ttf" \
  ff1f38786c849d1c41fa8e447960abdb2bd75fdfb0cfcdeb524fad65a5af3638
fetch "$COURIER/OFL.txt" "$FONTS/courierprime/OFL.txt" \
  9a755af092b494944c99f471be6fddd19b006a448fefdc4717e4ee0aa09a97b0
fetch "$DEJAVU" "$FONTS/dejavu/download/dejavu-fonts-ttf-2.37.zip" \
  7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a
unzip -o -q "$FONTS/dejavu/download/dejavu-fonts-ttf-2.37.zip" -d "$FONTS/dejavu/unpacked"
D="$FONTS/dejavu/unpacked/dejavu-fonts-ttf-2.37"

mkdir -p "$ROOT/tools/dev/builds/printer"
cc -std=c11 -O2 -Wall -Wextra -Werror $(freetype-config --cflags) \
  -o "$ROOT/tools/dev/builds/printer/pclfont" "$HERE/pclfont.c" $(freetype-config --libs) -liconv
"$ROOT/tools/dev/builds/printer/pclfont" "$FONTS/courierprime/CourierPrime-Regular.ttf" \
  "$FONTS/courierprime/CourierPrime-Bold.ttf" "$D/ttf/DejaVuSansMono.ttf" \
  "$D/ttf/DejaVuSansMono-Bold.ttf" "$ROOT/rp/src/pcl/pclfont.c"
cp "$FONTS/courierprime/OFL.txt" "$ROOT/rp/src/pcl/OFL.txt"
cp "$D/LICENSE" "$ROOT/rp/src/pcl/DEJAVU-LICENSE.txt"
