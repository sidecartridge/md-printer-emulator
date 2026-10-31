#!/usr/bin/env python3
"""A launcher floppy: boots, and TOS starts one program by itself (the #Z line
of the desktop's INF file, TOS 1.04 and later). The program stays on its own
disk, unmodified, in drive B (any format Hatari reads, Pasti included); the
launcher carries DESKTOP.INF (TOS 1.x) and NEWDESK.INF (TOS 2.x) and any
files given, such as the documents a test prints.

Usage:  make_launcher.py OUT.st [PROGRAM] [--tos-program] [--files DIR]
                        [--window 'A:\\*.*'] [--res low|medium]
        PROGRAM is the ST path of the program, e.g. 'B:\\STWRITER.PRG';
        without one TOS boots to its desktop. --tos-program starts it as a
        TOS program (no GEM), else as GEM. --window opens a desktop window on
        that path at boot. --inf-only writes just the two INF files into the
        folder OUT (for a work copy of a program's own boot disk,
        disk_copy.py --put).

The INF files follow the desktop's own (written by TOS 1.04's and 2.06's
"Save Desktop"); TOS 1.04 crashes with a newer one after the program exits.
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DEV = Path(__file__).resolve().parent

DESKTOP_INF = (
    "#a000000\r\n"
    "#b000000\r\n"
    "#c7770007000600070055200505552220770557075055507703111103\r\n"
    "#d                                             \r\n"
    "{autostart}"
    "#E 18 1{res} \r\n"
    "#W 00 00 {geometry} {open} {window}@\r\n"
    "#W 00 00 0A 0F 1A 09 00 @\r\n"
    "#W 00 00 0E 01 1A 09 00 @\r\n"
    "#M 01 00 00 FF C HARD DISK@ @ \r\n"
    "#M 00 00 00 FF A FLOPPY DISK@ @ \r\n"
    "#M 00 01 00 FF B FLOPPY DISK@ @ \r\n"
    "#T 00 03 02 FF   TRASH@ @ \r\n"
    "#F FF 04   @ *.*@ \r\n"
    "#D FF 01   @ *.*@ \r\n"
    "#G 03 FF   *.APP@ @ \r\n"
    "#G 03 FF   *.PRG@ @ \r\n"
    "#P 03 FF   *.TTP@ @ \r\n"
    "#F 03 04   *.TOS@ @ \r\n"
    "\x1a")

NEWDESK_INF = (
    "#a000000\r\n"
    "#b000000\r\n"
    "#c7770007000600070055200505552220770557075055507703111103\r\n"
    "#d                                             \r\n"
    "#K 4F 53 4C 00 46 42 43 57 45 58 00 00 00 00 00 00 00 00 00 00 00 00 00 52 00 00 4D 56 50 00 @\r\n"
    "{autostart}"
    "#E 18 0{res} 00 06 \r\n"
    "#Q 41 40 43 40 43 40 \r\n"
    "#W 00 00 {geometry} {open} {window}@\r\n"
    "#W 00 00 0A 0F 1A 09 00 @\r\n"
    "#W 00 00 0E 01 1A 09 00 @\r\n"
    "#W 00 00 04 07 26 0C 00 @\r\n"
    "#W 00 00 0C 0B 26 09 00 @\r\n"
    "#W 00 00 08 0F 1A 09 00 @\r\n"
    "#W 00 00 06 01 1A 09 00 @\r\n"
    "#N FF 04 000 @ *.*@ @ \r\n"
    "#D FF 01 000 @ *.*@ @ \r\n"
    "#G 03 FF 000 *.APP@ @ @ \r\n"
    "#G 03 FF 000 *.PRG@ @ @ \r\n"
    "#Y 03 FF 000 *.GTP@ @ @ \r\n"
    "#P 03 FF 000 *.TTP@ @ @ \r\n"
    "#F 03 04 000 *.TOS@ @ @ \r\n"
    "#M 00 01 00 FF C HARD DISK@ @ \r\n"
    "#M 00 00 00 FF A FLOPPY DISK@ @ \r\n"
    "#M 01 00 00 FF B FLOPPY DISK@ @ \r\n"
    "#T 00 03 02 FF   TRASH@ @ \r\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("program", nargs="?")
    ap.add_argument("--tos-program", action="store_true")
    ap.add_argument("--files", help="a folder whose files go on the launcher too")
    ap.add_argument("--window", default="", help="a path the desktop opens a window on")
    ap.add_argument("--inf-only", action="store_true",
                    help="write only DESKTOP.INF and NEWDESK.INF into the folder OUT")
    ap.add_argument("--res", choices=("low", "medium"), default="low",
                    help="the colour resolution TOS boots in (#E line's 2nd value)")
    args = ap.parse_args()
    autostart = ""
    if args.program:
        program = args.program.upper()
        if len(program) < 3 or program[1] != ":" or "\\" not in program:
            ap.error("PROGRAM is a full ST path, e.g. 'B:\\STWRITER.PRG'")
        autostart = f"#Z {'00' if args.tos_program else '01'} {program}@ \r\n"
    window = args.window.upper()
    # The window line's last number is 09 for a window the desktop opens at
    # boot (as TOS's own "Save Desktop" writes it), 00 for an unused one.
    open_ = "09" if window else "00"
    # An open window fills the screen below the menu bar (in characters:
    # x 0, y 1, 40 wide, 22 high); the unused one keeps TOS's default place.
    geometry = "00 01 28 16" if window else "02 0B 26 09"
    res = {"low": 1, "medium": 2}[args.res]
    desktop = DESKTOP_INF.format(autostart=autostart, window=window, open=open_, res=res,
                                 geometry=geometry).encode("latin1")
    newdesk = NEWDESK_INF.format(autostart=autostart, window=window, open=open_, res=res,
                                 geometry=geometry).encode("latin1")
    if args.inf_only:
        Path(args.out).mkdir(parents=True, exist_ok=True)
        (Path(args.out) / "DESKTOP.INF").write_bytes(desktop)
        (Path(args.out) / "NEWDESK.INF").write_bytes(newdesk)
        print(f"make_launcher.py: {args.out}/DESKTOP.INF, NEWDESK.INF")
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        folder = Path(tmp) / "launcher"
        if args.files:
            shutil.copytree(args.files, folder)
        else:
            folder.mkdir()
        (folder / "DESKTOP.INF").write_bytes(desktop)
        (folder / "NEWDESK.INF").write_bytes(newdesk)
        subprocess.run([str(DEV / "make_floppy.sh"), str(folder), args.out, "LAUNCHER"], check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
