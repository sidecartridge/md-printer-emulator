#!/usr/bin/env bash
#
# printertopdf.sh: a printer stream rendered by PrinterToPDF, the reference
# ESC/P renderer (tools/dev/printertopdf/Dockerfile), into OUT: what it
# writes for each page. As a 9-pin printer (-9) with its Atari character set
# (font2/Epson-Atari.C16), A4, unless OPTIONS say otherwise. The Docker image
# is built the first time.
#
# Usage:  tools/dev/printertopdf.sh FILE OUT [OPTIONS]
#         OPTIONS are PrinterToPDF's (-p, -m, -l, -8, -i; see its README).
set -Eeuo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

DEV="$(cd "$(dirname "$0")" && pwd)"
IMAGE="md-printer-emulator/printertopdf:feaba09"
[ $# -ge 2 ] || { sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
FILE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="$2"
shift 2
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

if ! docker image inspect "$IMAGE" > /dev/null 2>&1; then
  docker build -q -t "$IMAGE" "$DEV/printertopdf" > /dev/null
fi
docker run --rm -v "$(dirname "$FILE"):/in:ro" -v "$OUT:/out" "$IMAGE" \
  ./printerToPDF -o /out/ -f font2/Epson-Atari.C16 -9 "$@" "/in/$(basename "$FILE")"
