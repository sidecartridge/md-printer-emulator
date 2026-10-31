#!/usr/bin/env bash
#
# make_bench_disks.sh: the disks the bench's own tests boot, built into
# tools/dev/builds/disks/ from sources in this repository:
#   autotest.st   a floppy with \AUTO\AUTOTEST.PRG (tools/dev/st/autotest.s)
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
