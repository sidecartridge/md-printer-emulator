#!/usr/bin/env bash
#
# build.sh: assemble the bench's ST test programs (tools/dev/st/*.s) into
# tools/dev/builds/st/<NAME>.PRG with vasm, through stcmd (the Atari
# toolchain's Docker image, as the firmware's own m68k build uses). Each source
# is assembled from a fresh copy: Docker Desktop can show a container a file
# just rewritten on the host at its old length.
#
# Usage:  tools/dev/st/build.sh

set -Eeuo pipefail
trap 'echo "ERROR: tools/dev/st/build.sh: failed at line $LINENO" >&2' ERR

ST="$(cd "$(dirname "$0")" && pwd)"
OUT="$ST/../builds/st"
mkdir -p "$OUT"
WORK="$(mktemp -d "$ST/../builds/st-work.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
cp "$ST"/*.s "$WORK/"
for src in "$WORK"/*.s; do
  name="$(basename "$src" .s | tr 'a-z' 'A-Z')"
  (cd "$WORK" && STCMD_NO_TTY=1 ST_WORKING_FOLDER="$WORK" stcmd \
     vasm -quiet -Ftos -devpac -o "$name.PRG" "$(basename "$src")")
  cp "$WORK/$name.PRG" "$OUT/$name.PRG"
  echo "$OUT/$name.PRG ($(stat -f %z "$OUT/$name.PRG") bytes)"
done
