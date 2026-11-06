#!/usr/bin/env bash
#
# make_bench_disks.sh: the disks the bench's own tests boot, built into
# tools/dev/builds/disks/ from sources in this repository:
#   autotest.st   a floppy with \AUTO\AUTOTEST.PRG (tools/dev/st/autotest.s)
#   prpaths.st    a floppy with \AUTO\PRPATHS.PRG (tools/dev/st/prpaths.s)
#   escp.st       a floppy with \AUTO\PRFILES.PRG (tools/dev/st/prfiles.s) and the
#                 ESC/P test pages TEXT.PRN, GRAPH.PRN (tools/dev/st/make_escp_pages.py);
#                 escp-expected.prn, the two as PRFILES sends them, and
#                 escp-expected.pdf and escp-expected-letter.pdf, its PDF as
#                 the firmware writes it on A4 and on Letter
#                 (tools/printer/render, the firmware's own renderer)
#   pcl.st        the same with our PCL streams as TEXT.PRN and GRAPH.PRN
#                 (tools/printer/pclstreams.py: RASTER.PCL, PAGES.PCL), one
#                 LaserJet job; pcl-expected.prn, and pcl-expected.pdf and
#                 pcl-expected.pages, as the firmware renders it on A4
#   plain.st      the same with plain text, no escape code, as TEXT.PRN and
#                 GRAPH.PRN (tools/dev/st/documents/desktop/PRINT.TXT);
#                 plain-expected.prn, and plain-laserjet.pdf and
#                 plain-laserjet.pages, as the firmware's LaserJet prints it
#
# Usage:  tools/dev/make_bench_disks.sh

set -Eeuo pipefail
trap 'echo "ERROR: make_bench_disks.sh: failed at line $LINENO" >&2' ERR

DEV="$(cd "$(dirname "$0")" && pwd)"
OUT="$DEV/builds/disks"
mkdir -p "$OUT"
"$DEV/st/build.sh" > /dev/null
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/autotest/AUTO"
cp "$DEV/builds/st/AUTOTEST.PRG" "$WORK/autotest/AUTO/"
"$DEV/make_floppy.sh" "$WORK/autotest" "$OUT/autotest.st" AUTOTEST
mkdir -p "$WORK/prpaths/AUTO"
cp "$DEV/builds/st/PRPATHS.PRG" "$WORK/prpaths/AUTO/"
"$DEV/make_floppy.sh" "$WORK/prpaths" "$OUT/prpaths.st" PRPATHS
mkdir -p "$WORK/escp/AUTO"
cp "$DEV/builds/st/PRFILES.PRG" "$WORK/escp/AUTO/"
python3 "$DEV/st/make_escp_pages.py" "$WORK/escp" > /dev/null
cat "$WORK/escp/TEXT.PRN" "$WORK/escp/GRAPH.PRN" > "$OUT/escp-expected.prn"   # what PRFILES prints
make -s -C "$DEV/../printer"
mkdir -p "$WORK/pages"
"$DEV/builds/printer/render" --pdf "$OUT/escp-expected.pdf" "$OUT/escp-expected.prn" "$WORK/pages" > /dev/null
"$DEV/builds/printer/render" --letter --pdf "$OUT/escp-expected-letter.pdf" "$OUT/escp-expected.prn" \
  "$WORK/pages" > /dev/null
"$DEV/make_floppy.sh" "$WORK/escp" "$OUT/escp.st" ESCP
mkdir -p "$WORK/pcl/AUTO" "$WORK/pclstreams"
cp "$DEV/builds/st/PRFILES.PRG" "$WORK/pcl/AUTO/"
python3 "$DEV/../printer/pclstreams.py" "$WORK/pclstreams"
cp "$WORK/pclstreams/RASTER.PCL" "$WORK/pcl/TEXT.PRN"
cp "$WORK/pclstreams/PAGES.PCL" "$WORK/pcl/GRAPH.PRN"
cat "$WORK/pcl/TEXT.PRN" "$WORK/pcl/GRAPH.PRN" > "$OUT/pcl-expected.prn"
"$DEV/builds/printer/render" --pdf "$OUT/pcl-expected.pdf" "$OUT/pcl-expected.prn" "$WORK/pages" \
  > "$OUT/pcl-expected.pages" 2> /dev/null
"$DEV/make_floppy.sh" "$WORK/pcl" "$OUT/pcl.st" PCL
mkdir -p "$WORK/plain/AUTO"
cp "$DEV/builds/st/PRFILES.PRG" "$WORK/plain/AUTO/"
cp "$DEV/st/documents/desktop/PRINT.TXT" "$WORK/plain/TEXT.PRN"
cp "$DEV/st/documents/desktop/PRINT.TXT" "$WORK/plain/GRAPH.PRN"
cat "$WORK/plain/TEXT.PRN" "$WORK/plain/GRAPH.PRN" > "$OUT/plain-expected.prn"
"$DEV/builds/printer/render" --laserjet --pdf "$OUT/plain-laserjet.pdf" "$OUT/plain-expected.prn" \
  "$WORK/pages" > "$OUT/plain-laserjet.pages" 2> /dev/null
"$DEV/make_floppy.sh" "$WORK/plain" "$OUT/plain.st" PLAIN
