# The firmware in Hatari (EmuMD)

[EmuMD](https://github.com/neilrackett/emumd) builds this firmware's own C code
for the host as a `.mdfw` and runs it in its fork of Hatari, which has a
Multi-device on the cartridge port: the ST boots the firmware's cartridge
image, the firmware's `main()` runs on the host and reads the ST's ROM3
accesses as it does on the device. It is the fast loop for logic, the ST side
and the setup menu. It times nothing (the emulated Multi-device is infinitely
fast), has no PIO, DMA or USB, and never presses SELECT: timing, the bus and
sign-off are on the device, with the tools in `tools/dev/` (`README.md` there).

## Set-up

```sh
git submodule update --init emu/emumd
tools/dev/build_hatari_fork.sh       # emumd's Hatari, about 2.5 min
mkdir -p emu/tos emu/sd
ln -s "/path/to/TOS 1.04 - English UK - 50Hz.img" emu/tos/tos104uk.img
ln -s "/path/to/TOS 1.62 - English-UK - 50Hz"     emu/tos/tos162uk.img
ln -s "/path/to/TOS v2.06 (1991)(Atari Corp)(Mega-STE)(US).img" emu/tos/tos206us.img
```

`emu/tos/` and `emu/sd/` (the emulated microSD card) are gitignored.
`build_hatari_fork.sh` runs emumd's own `hatari/build-hatari.sh` unchanged,
with its cache in `tools/dev/builds/emumd-cache/` rather than `~/.cache/emumd`
(which other projects share at other emumd versions), and with
`tools/dev/hatari_fork.cmake`, which fixes two things on a Mac: an old
XQuartz's `/opt/X11` headers (a libpng 1.5 `png.h` that breaks screenshots) are
ignored, and `/Library/Frameworks` is in the binary's rpath for SDL2.

## Build and run by hand

```sh
tools/dev/mdfw.sh build              # build/md-printer-emulator.mdfw
tools/dev/mdfw.sh run --no-user-config                     # ST, TOS 1.04, a window
tools/dev/mdfw.sh run --no-user-config --tos emu/tos/tos162uk.img --machine ste
```

`tools/dev/mdfw.sh` is emumd's `mdfw` with this repository's cache.
`mdfw.ini` (at the root) builds everything in `rp/src` but the hardware layer
emumd stands in for (the bus, SELECT, the SD card driver), and this folder.
In the setup menu, `f` then Return launches the user firmware.

What building `rp/src` for a 64-bit host needed, in the firmware's own sources
and without changing the device's build (its flash image is byte-identical):
the cartridge window's address kept in `uintptr_t`, and `tprotocol.h`'s Thumb
`strh` under `defined(__arm__)` with the same store in C otherwise.

## The harness

```sh
python3 tools/dev/emu_harness.py --setup st-104|ste-162|mste-206 --test template-menu
python3 tools/dev/emu_harness.py --setup st-104 --step 'text "Select an option" 600'
```

It builds what it runs (the m68k side when a source under `target/atarist` is
newer than `target_firmware.h`, then the plugin), starts Hatari with its own
`HOME` (no saved configuration) at the machine's own speed, drives it through
Hatari's control socket, and ends in PASS or FAIL. Each run has a folder in
`tools/dev/logs/emu/` with `report.json`, `hatari.out` (the firmware's log),
`shots/`, the firmware's card `sd/` and the disk copies. The steps are listed in
the script's help (`--help`): keys, the mouse, screenshots, log lines, the setup
terminal's text, screens against references, resets, the printer port.

Tests are `tools/dev/emu_tests/*.steps`: one step a line; `@disk-a PATH`,
`@disk-b PATH`, `@acsi PATH`, `@hatari stock` and `@printer` set the run's
options. References are `tools/dev/emu_refs/<setup>/<name>.png`, made with
`--update-refs` (and looked at before they are kept).

| Test | What it checks |
| --- | --- |
| `template-menu` | the setup menu appears; the ST's hello, machine and TOS arrive |
| `template-firmware` | `[F]irmware` runs the user firmware's demo |
| `template-reset` | warm resets: the menu again, then the user firmware once per reset |
| `template-coldreset` | a cold reset power-cycles the Multi-device: the menu again |
| `floppy-auto` | a floppy's `AUTO` program runs after the cartridge, then the desktop |
| `print-paths` | stock Hatari: one line through each TOS print path (`st/prpaths.s`) |
| `print-...` | a program prints (the desktop, Alt-Help, ST Writer, 1st Word Plus, GFA BASIC, Degas Elite, GDOS, XControl's printer settings, our ESC/P pages, Calamus, Signum 3), and `printwatch` says whether through TOS |
| `hook-capture`, `hook-off`, `hook-reset`, `hook-stwriter` | the printer hook: installed, switched off, after a reset, a GEM program into it |
| `hook-escp` | our ESC/P pages reach the RP's card (`PRINTER/00000001.PRN`) byte for byte |
| `hook-jobs` | each printing is a job, numbered; the number survives a cold reset (the RP's flash) |
| `hook-nocard` | a card that fails: the ST prints to the end, the RP loses the job and says so |
| `hook-romfree` | once the hook is installed, the CPU runs no code in the cartridge (it is resident in RAM) |
| `hook-althelp-hold` | Alt-Help held, then pressed again, during a screen dump: the job is one whole dump |
| `impossible` | must FAIL: the harness can fail |

`tools/dev/hook_fidelity.sh TEST SETUP` runs a printing test twice on the firmware after
`[F]irmware` (`--via-firmware`): with the printer switched off (`--hook off`), the printer port
gets the reference; switched on, the RP's jobs on the card, in order, must hold the same bytes.
A job is a file only once it has ended, so with the hook on a test's `printer` step becomes
`cardfile`, which waits until the hook has taken nothing for the same quiet time, its buffer is
empty and the jobs hold every byte it took.

`text` reads the setup terminal back as text: `template_extra.c` matches each
cell of the framebuffer against the terminal's own font when the harness asks
(a file in the run's `requests/`, polled from `select_poll()`).

## Disks

Hatari never gets a GEMDOS drive: it would take the first 1 KB of the
cartridge, so the firmware's boot code would not run. Programs come on
floppies and ACSI hard-disk images:

```sh
tools/dev/make_program_disks.sh                   # the program tests' launchers and work copies
tools/dev/make_floppy.sh FOLDER disk.st [LABEL]   # 720 KB, checked file by file
tools/dev/make_acsi.sh FOLDER disk.img [MB]       # PPDRIVER, by ../atari-hd (ATARI_HD)
tools/dev/make_bench_disks.sh                     # the tests' own, in tools/dev/builds/disks/
tools/dev/st/build.sh                             # the bench's ST programs (tools/dev/st/*.s)
tools/dev/make_test_disk.sh                       # every test program on one ACSI image, for a real ST
```

`make_test_disk.sh` puts the programs the printing tests use, set up as the tests set them up
but with their paths on C:, on one bootable 32 MB image (`builds/disks/printer-tests.img`; it
holds commercial programs and is never committed). On an ACSI2STM it is `acsi2stm/hd0.img` at
the root of the SD card. `C:\README.TXT` on it says what to print in each folder and the MD5 a
debug build should log for the job (TOS 1.62). Pasti disks are read with `stx_to_st.py`, which
reports the sectors a protection left unreadable (Degas Elite: tracks 80-81, past its files).

`.ST`, `.MSA` and Pasti `.STX` images all load.

The programs the printer is tested with are downloaded disks outside the repository
(`$PROGRAM_DISKS`, by default `/Volumes/MacData/md-printer-emulator-disks`), never modified. A
*launcher* floppy in drive A starts a program from its own disk in drive B by itself: the `#Z`
line of `DESKTOP.INF` (TOS 1.x) and `NEWDESK.INF` (TOS 2.x), with `#E` setting the resolution
(`make_launcher.py`). A program that looks for its files on its own drive (Easy Draw with GDOS
in its AUTO folder, Calamus) boots from a *work copy* of its disk with those INF files put on
it (`disk_copy.py --put`); a program too large for a floppy (Signum 3) from an ACSI image.
`--hatari bare` (the fork with no firmware) is the reference for programs that need the
mouse; `printwatch` counts every entry into TOS's printer routine, by caller.

The TOS 2.06 is the US build (country 0, NTSC, 60 Hz): the UK images on this Mac were either
the US one under a UK name or a patched SuperTOS. `type` presses the characters whose key
depends on the keyboard (`"`, `@`, `'`) where the running TOS's country puts them. After a run with `--acsi`, the
image's files are copied out to the run's `acsi-files/`.

## Gotchas

- **GEMDOS, OS base, VDI and AES traces replace the cartridge**: with them on,
  Hatari loads its own cartridge code instead of the firmware's (`cart.c`). The
  harness refuses them on the fork; BIOS and XBIOS traces are fine.
- **Stock Hatari has no mouse events** (`mousemove`, `leftdown`, `leftup` are
  the fork's): mouse steps need the fork.
- **The first mouse click after boot is lost** on TOS's desktop; tests click an
  empty part of the desktop first. A double click is not reliable through the
  control socket: select with a click and use the File menu's Open.
- **`mouse X Y` pins the pointer in the top left corner once**, then moves from
  where it is: crossing GEM's menu bar on the way opens its menus.
- **TOS 2.06 dithers the desktop** (green and white on alternate pixels), runs
  a memory test (ESC cuts it short: tests send one early, which the other TOS
  ignore), and places the desktop's icons differently from TOS 1.x. It crashes
  with no floppy drive connected: the harness never turns drive A off (only
  drive B, when no disk is given for it).
- **A warm reset leaves the Multi-device running; a cold reset power-cycles
  it**, as on hardware.
- **SELECT cannot be pressed**: emumd's stand-in discards the firmware's
  callbacks.
- **Keys sent before a program reads them stay in the keyboard buffer**: an
  ESC meant for a Mega STE's memory test reaches the next program on another
  machine.
- **A key the harness holds down repeats**: it releases it only when it next
  reads the frame count, several frames late, and TOS repeats a key held long
  enough (in the setup terminal, a key meant to be held 8 frames typed four).
  `key` and `type` send each key down and up in one event; `key KEY FRAMES`
  holds it.
- **Hatari does not deliver punctuation typed as characters**: `type` sends it
  by scancode, and the layout-dependent ones (`" @ ' \ _`) where the running
  TOS's country puts them.
- **GEM's file selector takes an 8.3 name**, no path: enter folders by
  clicking them, then type the name.
- **Programs differ in double clicks and pointers**: XControl opens a CPX
  with a 60 ms double click (100 ms is too slow on TOS 2.06); Signum
  accelerates the pointer (`--mouse-step 2`) and takes clicks one row above it.
- **Printing in the first seconds after boot, straight after the floppy is
  read, loses every byte on Hatari's printer port** though TOS's routine runs:
  test programs wait for a key first.
- **Breakpoints that trace every hit slow Hatari to a crawl**: `printwatch`
  reports the ROM calls every 64th hit.
