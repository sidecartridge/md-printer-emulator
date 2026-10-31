#!/usr/bin/env bash
#
# make_bench_disks.sh: the disks the bench's own tests boot, built into
# tools/dev/builds/disks/ from sources in this repository:
#   autotest.st   a floppy with \AUTO\AUTOTEST.PRG (tools/dev/st/autotest.s)
#   prpaths.st    a floppy with \AUTO\PRPATHS.PRG (tools/dev/st/prpaths.s)
#   escp.st       a floppy with \AUTO\PRFILES.PRG (tools/dev/st/prfiles.s) and the
#                 ESC/P test pages TEXT.PRN, GRAPH.PRN (tools/dev/st/make_escp_pages.py)
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
"$DEV/make_floppy.sh" "$WORK/escp" "$OUT/escp.st" ESCP
