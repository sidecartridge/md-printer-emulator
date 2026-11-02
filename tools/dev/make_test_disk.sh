#!/usr/bin/env bash
#
# make_test_disk.sh: one ACSI hard-disk image with every program the printer's
# tests print with, to run them on a real ST through the Multi-device. On an
# ACSI2STM it is acsi2stm/hd0.img at the root of the SD card; in Hatari,
# --acsi. It boots with P. Putnik's driver (make_acsi.sh) to a desktop with a
# window on C:. C:\README.TXT says what to print in each folder and the MD5 a
# debug build's log should show for the job.
#
# The programs come from their downloaded disks in $PROGRAM_DISKS (default
# /Volumes/MacData/md-printer-emulator-disks; only read), set up as the
# emulated tests set them up, with paths on C: instead of A: or E:. The image
# holds commercial programs: it stays in tools/dev/builds/disks/, never
# committed.
#
#   C:\AUTO\GDOS.PRG      Easy Draw's GDOS, with C:\ASSIGN.SYS (its fonts and
#                         FX-80 driver in C:\EASYDRAW\GEMSYS\)
#   C:\XCONTROL.ACC       XControl, and C:\PRINTER.CPX where it looks for CPXs
#   C:\ESCP, PRPATHS      our test programs (tools/dev/st)
#   C:\DESKPRT\PRINT.TXT  the desktop's Print
#   C:\WORDPLUS, WORDPLFX 1st Word Plus, with its own and its Epson FX table
#   C:\STWRITER, GFA, DEGAS, EASYDRAW, CALAMUS, SIGNUM3
#
# Usage:  tools/dev/make_test_disk.sh [IMAGE [PROGRAM [OPTIONS]]]
#         IMAGE defaults to tools/dev/builds/disks/printer-tests.img. With a
#         PROGRAM ('C:\ESCP\PRFILES.PRG'), the desktop starts it at boot, as
#         an emulated check of the image needs (make_launcher.py's PROGRAM and
#         options: --tos-program, --res medium, --window).
# Needs tools/dev/st/build.sh's programs (tools/dev/builds/st) and mtools.
set -Eeuo pipefail
trap 'echo "ERROR: ${BASH_SOURCE[0]}: failed at line ${LINENO}" >&2' ERR

DEV="$(cd "$(dirname "$0")" && pwd)"
P="${PROGRAM_DISKS:-/Volumes/MacData/md-printer-emulator-disks}"
OUT="${1:-$DEV/builds/disks/printer-tests.img}"
shift $(( $# > 0 ? 1 : 0 ))
mkdir -p "$DEV/builds/disks"
WORK="$(mktemp -d "$DEV/builds/test-disk.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
export MTOOLS_SKIP_CHECK=1
C="$WORK/c"
mkdir -p "$C/AUTO"

# unpack IMAGE FOLDER: every file of a floppy image (.ST, .MSA, .STX) into FOLDER.
unpack() {
  local st="$WORK/unpack.st"
  case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
    *.stx) python3 "$DEV/stx_to_st.py" "$1" "$st" > /dev/null ;;
    *.msa) python3 "$DEV/disk_copy.py" "$1" "$st" > /dev/null ;;
    *) cp "$1" "$st" ;;
  esac
  mkdir -p "$2"
  mcopy -s -n -i "$st" "::/*" "$2/"
}

# replace FILE FROM TO: every FROM in FILE becomes TO, bytes as they are.
replace() {
  python3 -I - "$@" <<'EOF'
import sys
path, old, new = sys.argv[1], sys.argv[2].encode(), sys.argv[3].encode()
data = open(path, "rb").read()
if old not in data:
    sys.exit(f"{path}: no {sys.argv[2]}")
open(path, "wb").write(data.replace(old, new))
EOF
}

# Our test programs.
mkdir -p "$C/ESCP" "$C/PRPATHS" "$C/DESKPRT"
cp "$DEV/builds/st/PRFILES.PRG" "$C/ESCP/"
python3 "$DEV/st/make_escp_pages.py" "$C/ESCP" > /dev/null
cp "$DEV/builds/st/PRPATHS.PRG" "$C/PRPATHS/"
cp "$DEV/st/documents/desktop/PRINT.TXT" "$C/DESKPRT/"

# 1st Word Plus, with the table it ships with and with its Epson FX table.
unpack "$P/1st_word_plus_2_02/files/1stWordPlus1.MSA" "$C/WORDPLUS"
unpack "$P/1st_word_plus_2_02/files/1stWordPlus1.MSA" "$C/WORDPLFX"
cp "$C/WORDPLFX/PRINTERS/EPSON_FX.CFG" "$C/WORDPLFX/PRINTER.CFG"

unpack "$P/st_writer_elite_4_0/files/st_writer_elite_atari_(usa).st" "$C/STWRITER"
unpack "$P/gfa_basic_3_5e/files/gfa_basic_3_5e_st_format.msa" "$C/GFA"
unpack "$P/degas_elite_1_0/files/Degas_Elite_-_Batteries_included.STX" "$C/DEGAS"

# Easy Draw: GDOS from the boot drive's AUTO, ASSIGN.SYS at its root.
unpack "$P/easy_draw_2_35/files/easy_draw_2_35_st_review.msa" "$C/EASYDRAW"
mv "$C/EASYDRAW/AUTO/GDOS.PRG" "$C/AUTO/"
cp "$C/EASYDRAW/ASSIGN.SYS" "$C/ASSIGN.SYS"
replace "$C/ASSIGN.SYS" 'A:\GEMSYS\' 'C:\EASYDRAW\GEMSYS\'

# XControl: an accessory loads from the boot drive's root, and finds its CPXs
# there.
unpack "$P/xcontrol/files/xcontrol_-_extensible_control_panel_atari_corp_1_0..msa" "$C/XCONTROL"
cp "$C/XCONTROL/XCONTROL.ACC" "$C/"
cp "$C/XCONTROL/CPX/PRINTER.CPX" "$C/"

# Calamus: CALAMUS.SET's folders are on the uploader's E:, and its printer an
# Epson LQ driver the disk does not have.
unpack "$P/calamus_1_09n/files/calamus_1_09n_dmc_de.msa" "$C/CALAMUS"
replace "$C/CALAMUS/CALAMUS.SET" 'E:\CALAMUS1.09N\' 'C:\CALAMUS\'
replace "$C/CALAMUS/CALAMUS.SET" 'E:\CALAMUS_.SL\' 'C:\CALAMUS\'
replace "$C/CALAMUS/CALAMUS.SET" 'EPSON_LQ.CPD' 'HP_LJETP.CPD'

# Signum 3: its print driver's path is the uploader's E:.
cp -R "$P/signum_3_1_30/files/signum3" "$C/SIGNUM3"
replace "$C/SIGNUM3/prdrv.inf" 'E:\SIGNUM3\' 'C:\SIGNUM3\'

# The desktop: a window on C: at boot.
python3 "$DEV/make_launcher.py" "$C" --inf-only --window 'C:\*.*' "$@" > /dev/null
sed 's/$/\r/' "$DEV/test_disk_readme.txt" > "$C/README.TXT"

"$DEV/make_acsi.sh" "$C" "$OUT" 32
