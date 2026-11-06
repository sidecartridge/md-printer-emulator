#!/usr/bin/env bash
#
# make_program_disks.sh: the disks the printer's program tests boot
# (tools/dev/emu_tests/print-*.steps), built from the downloaded programs' disks in
# $PROGRAM_DISKS (default /Volumes/MacData/md-printer-emulator-disks; never
# committed, only read). Into tools/dev/builds/:
#
#   launchers/  floppies whose desktop starts a program from drive B by itself
#               (make_launcher.py): stwriter, gfabasic, wordplus (medium),
#               degas, desktop (no program: a window on A:, PRINT.TXT),
#               desktop-set128 (the same with SET128.TXT, st/make_set128.py), signum
#   disks/      work copies of programs' own disks (disk_copy.py):
#               wordplus-fx.st  1st Word Plus with PRINTER.CFG = its EPSON_FX.CFG
#               wordplus-set128.st  the same with DOCS\SET128.TXT
#               easydraw.st     Easy Draw booting itself (GDOS from its AUTO)
#               easydraw-laserjet.st  the same printing to an HP LaserJet: Timeworks
#                               DTP's GDOS driver HP_LJET.SYS as device 21, in
#                               the room of the Epson fonts
#               calamus.st      Calamus booting itself, CALAMUS.SET's folders on A:
#               xcontrol.st     XControl with its PRINTER.CPX at the root
#               signum.img      an ACSI image with Signum 3, its print driver's
#                               path on C: (make_acsi.sh)
#
# Usage:  tools/dev/make_program_disks.sh

set -Eeuo pipefail
trap 'echo "ERROR: make_program_disks.sh: failed at line $LINENO" >&2' ERR

DEV="$(cd "$(dirname "$0")" && pwd)"
P="${PROGRAM_DISKS:-/Volumes/MacData/md-printer-emulator-disks}"
L="$DEV/builds/launchers"
D="$DEV/builds/disks"
mkdir -p "$L" "$D"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
launcher() { python3 "$DEV/make_launcher.py" "$@" | tail -1; }
copy() { python3 "$DEV/disk_copy.py" "$@" | cut -c1-100; }

# Launchers: the program on its own disk in drive B.
launcher "$L/stwriter.st" 'B:\STWRITER.PRG'
launcher "$L/gfabasic.st" 'B:\GFABASIC\GFABASIC.PRG'
launcher "$L/wordplus.st" 'B:\WORDPLUS.PRG' --res medium
launcher "$L/degas.st" 'B:\DEGELITE.PRG'
launcher "$L/desktop.st" --files "$DEV/st/documents/desktop" --window 'A:\*.*'
python3 "$DEV/st/make_set128.py" "$WORK/set128" > /dev/null
launcher "$L/desktop-set128.st" --files "$WORK/set128" --window 'A:\*.*'
launcher "$L/signum.st" 'C:\SIGNUM3\SIGNUM3.PRG'

# 1st Word Plus with its Epson FX table.
copy "$P/1st_word_plus_2_02/files/1stWordPlus1.MSA" "$D/wordplus-fx.st" \
  --copy 'PRINTERS\EPSON_FX.CFG=PRINTER.CFG'
copy "$P/1st_word_plus_2_02/files/1stWordPlus1.MSA" "$D/wordplus-set128.st" \
  --copy 'PRINTERS\EPSON_FX.CFG=PRINTER.CFG' --put "$WORK/set128/SET128.TXT=DOCS\SET128.TXT"

# Easy Draw: GDOS loads from the boot disk's AUTO, so the disk itself boots.
python3 "$DEV/make_launcher.py" "$WORK/easydraw" 'A:\EASYDRAW.PRG' --res medium --inf-only > /dev/null
copy "$P/easy_draw_2_35/files/easy_draw_2_35_st_review.msa" "$D/easydraw.st" \
  --put "$WORK/easydraw/DESKTOP.INF=DESKTOP.INF" --put "$WORK/easydraw/NEWDESK.INF=NEWDESK.INF"
export MTOOLS_SKIP_CHECK=1
mcopy -n -i "$D/easydraw.st" ::/ASSIGN.SYS "$WORK/assign.sys"
mcopy -n -i "$P/timeworks_dtp_1_11/files/timeworks_dtp_1_11_timeworks_disk_4.st" \
  ::/PART6/HP_LJET.SYS "$WORK/HP_LJET.SYS"
python3 -I - "$WORK/assign.sys" <<'EOF'
import re, sys
p = sys.argv[1]
d = open(p, "rb").read()
open(p, "wb").write(re.sub(rb"21 FX80\.SYS\r?\n.*?(?=22 JX80\.SYS)", b"21 HP_LJET.SYS\r\n", d, flags=re.S))
EOF
epson_fonts=()
for f in $(mdir -b -i "$D/easydraw.st" ::/GEMSYS | grep -iE "EP\.FNT$|/EPSHSS07\.FNT$"); do
  epson_fonts+=(--delete "${f#::/}")
done
copy "$D/easydraw.st" "$D/easydraw-laserjet.st" "${epson_fonts[@]}" \
  --put "$WORK/HP_LJET.SYS=GEMSYS\HP_LJET.SYS" --put "$WORK/assign.sys=ASSIGN.SYS"

# Calamus: CALAMUS.SET's folders are on the uploader's E:, and its printer an
# Epson LQ driver the disk does not have: the folders on A:, the HP LaserJet
# driver the disk has.
python3 "$DEV/make_launcher.py" "$WORK/calamus" 'A:\CALAMUS.PRG' --inf-only > /dev/null
export MTOOLS_SKIP_CHECK=1
copy "$P/calamus_1_09n/files/calamus_1_09n_dmc_de.msa" "$WORK/calamus.st"
mcopy -n -i "$WORK/calamus.st" ::/CALAMUS.SET "$WORK/calamus.set"
python3 -I - "$WORK/calamus.set" "$WORK/calamus/CALAMUS.SET" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
d = d.replace(b"E:\\CALAMUS1.09N\\", b"A:\\").replace(b"E:\\CALAMUS_.SL\\", b"A:\\")
open(sys.argv[2], "wb").write(d.replace(b"EPSON_LQ.CPD", b"HP_LJETP.CPD"))
EOF
copy "$P/calamus_1_09n/files/calamus_1_09n_dmc_de.msa" "$D/calamus.st" \
  --put "$WORK/calamus/DESKTOP.INF=DESKTOP.INF" --put "$WORK/calamus/NEWDESK.INF=NEWDESK.INF" \
  --put "$WORK/calamus/CALAMUS.SET=CALAMUS.SET"

# XControl looks for CPX files at A:\*.CPX.
copy "$P/xcontrol/files/xcontrol_-_extensible_control_panel_atari_corp_1_0..msa" "$D/xcontrol.st" \
  --copy 'CPX\PRINTER.CPX=PRINTER.CPX'

# Signum 3 on an ACSI disk: its print driver's path is the uploader's E:.
mkdir -p "$WORK/signum"
cp -R "$P/signum_3_1_30/files/signum3" "$WORK/signum/SIGNUM3"
python3 -I - "$WORK/signum/SIGNUM3/prdrv.inf" <<'EOF'
import sys
p = sys.argv[1]
open(p, "wb").write(open(p, "rb").read().replace(b"E:\\SIGNUM3\\", b"C:\\SIGNUM3\\"))
EOF
python3 "$DEV/make_launcher.py" "$WORK/signum" 'C:\SIGNUM3\SIGNUM3.PRG' --inf-only > /dev/null
"$DEV/make_acsi.sh" "$WORK/signum" "$D/signum.img" 32
