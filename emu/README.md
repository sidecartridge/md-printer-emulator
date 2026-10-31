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
ln -s "/path/to/TOS 2.06 - English-UK - 50Hz"     emu/tos/tos206uk.img
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
| `althelp-reference` | stock Hatari: Alt-Help's dump as the printer port receives it |
| `impossible` | must FAIL: the harness can fail |

`text` reads the setup terminal back as text: `template_extra.c` matches each
cell of the framebuffer against the terminal's own font when the harness asks
(a file in the run's `requests/`, polled from `select_poll()`).

## Disks

Hatari never gets a GEMDOS drive: it would take the first 1 KB of the
cartridge, so the firmware's boot code would not run. Programs come on
floppies and ACSI hard-disk images:

```sh
tools/dev/make_floppy.sh FOLDER disk.st [LABEL]   # 720 KB, checked file by file
tools/dev/make_acsi.sh FOLDER disk.img [MB]       # PPDRIVER, by ../atari-hd (ATARI_HD)
tools/dev/make_bench_disks.sh                     # the tests' own, in tools/dev/builds/disks/
tools/dev/st/build.sh                             # the bench's ST programs (tools/dev/st/*.s)
```

`.ST`, `.MSA` and Pasti `.STX` images all load. After a run with `--acsi`, the
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
  a memory test (ESC cuts it short), needs a floppy drive to boot, and places
  the desktop's icons differently from TOS 1.x.
- **A warm reset leaves the Multi-device running; a cold reset power-cycles
  it**, as on hardware.
- **SELECT cannot be pressed**: emumd's stand-in discards the firmware's
  callbacks.
- **Keys sent before a program reads them stay in the keyboard buffer**: an
  ESC meant for a Mega STE's memory test reaches the next program on another
  machine.
