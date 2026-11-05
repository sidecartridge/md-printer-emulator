#!/usr/bin/env python3
"""Run the firmware in emumd's Hatari (or a program in stock Hatari), drive it
with steps, and end in PASS or FAIL with the evidence.

  --hatari fork   emumd's fork of Hatari with this firmware as its Multi-device
                  plugin (build/md-printer-emulator.mdfw): the cartridge boots
                  from the firmware's own image, the RP side is the firmware's
                  own C code (the default).
  --hatari stock  stock Hatari (/usr/local/bin/hatari), no cartridge: the
                  reference, e.g. what a program sends to the printer port
                  (--printer).
  --hatari bare   emumd's fork with no firmware: a reference run that needs
                  the mouse (stock Hatari has no mouse events).

Setups: st-104 (ST, TOS 1.04), ste-162 (STE, TOS 1.62), mste-206 (Mega STE,
TOS 2.06); TOS images linked into emu/tos/ (gitignored). Hatari runs at the
machine's own speed unless --fast; times in steps are emulated frames (VBLs,
50 a second), read from Hatari's debugger. Hatari never gets a GEMDOS drive
(it would take the first 1 KB of the cartridge): disks are floppies (--disk-a,
--disk-b) and ACSI images (--acsi).

Steps, in order (--step, or --test NAME for tools/dev/emu_tests/NAME.steps;
a test's "@disk-a PATH", "@disk-b PATH" or "@acsi PATH" line gives the run that
disk, relative to the repository ($PROGRAM_DISKS: the downloaded programs'
disks, by default /Volumes/MacData/md-printer-emulator-disks), unless the
command line does; "@hatari
stock", "@printer", "@monitor mono", "@memsize N", "@mouse-step N" and "@keep-raw
off" set those options):

  wait FRAMES           let FRAMES frames pass
  key KEY [FRAMES]      press and release a key, in one event; with FRAMES,
                        hold it that long at least (the harness reads the
                        frame count every few frames, and TOS repeats a key
                        held too long): an ST scancode in hex (0x1c is
                        Return) or one character
  keydown KEY, keyup KEY
                        hold or release a key (Alt-Help: keydown 0x38,
                        key 0x62, keyup 0x38)
  type TEXT             type TEXT, one key at a time (lower-case letters,
                        digits, space; \\n is Return)
  mouse X Y             the pointer at X, Y in ST pixels (pinned in the top
                        left corner the first time, moved from where it is
                        after that)
  mousemove DX DY       move the pointer by DX, DY ST pixels
  click [left|right] [FRAMES]
                        press a mouse button for FRAMES frames (5)
  dblclick [GAP_MS]     a left double click, GAP_MS (100) between its events
  hdblclick             Hatari's own double click event
  shot NAME             a screenshot, shots/NAME.png
  log REGEX FRAMES      wait until Hatari's output (the firmware's log
                        included) matches REGEX, anywhere since the start
  newlog REGEX FRAMES   the same, only in output that arrives after the step
                        starts
  text REGEX FRAMES     wait until the firmware's setup terminal shows text
                        matching REGEX (fork only; text.txt in the run folder)
  screen same|differs NAME FRAMES
                        wait until the screen equals (or differs from) the
                        reference tools/dev/emu_refs/SETUP/NAME.png, within
                        SCREEN_TOLERANCE of its pixels (--update-refs writes
                        the reference from the screen instead, and passes)
  desktop FRAMES        wait until the screen is the TOS desktop (colour)
  printer NBYTES FRAMES [QUIET]
                        wait until the printer port has had NBYTES bytes, then
                        until it has been quiet for QUIET frames (500)
                        (--printer)
  printwatch            count the entries into TOS's printer routine from now
                        on, by caller: from ROM to within 63, from RAM exactly,
                        next to the printer port's bytes in report.json (not
                        with the firmware: a breakpoint slows every byte)
  hookstate [NAME]      the printer hook, resident (found from xconout[0]):
                        its state saved as NAME.bin, its counters and buffer
                        in the report; FAILs unless the four printer vectors
                        and a VBL slot reach it
  svar INDEX VALUE      shared variable INDEX becomes VALUE (hex), as the
                        firmware writes it (fork only)
  setting KEY VALUE     the app's setting KEY becomes VALUE, saved, as the
                        setup menu's put and save do (fork only)
  cardfile PATTERN NBYTES FRAMES [QUIET]
                        wait until the printer hook has taken NBYTES since
                        the ST booted, then nothing for QUIET frames (500),
                        and the card's files matching PATTERN (the jobs,
                        e.g. PRINTER/*.PRN) hold all it took (fork only)
  cardsame PATH FILE    FAILs unless PATH on the card equals FILE, byte for byte
                        (FILE in the repository, or card:PATH on the card)
  hookdone NBYTES FRAMES [QUIET]
                        wait as cardfile does, then FAILs unless the RP
                        answered every block the hook sent (fork only)
  cardput PATH          an empty file PATH on the card (a file where the
                        firmware wants a folder: the card fails for it)
  cardnone PATTERN      FAILs if a file on the card matches PATTERN
  cardfull FREE         the card full but FREE bytes: a 1 MB FAT volume
                        (macOS hdiutil) over the run's sd/, filled with one
                        file; its files are kept in card-full/ at the end
  cartexec watch|none   watch for the CPU running code in the cartridge
                        ($FA0000-$FAFFFF), then FAIL if it has since then,
                        with the registers at the first time (the hook runs
                        from RAM: an RP restart leaves no code to run there)
  rom4 NAME             the 64 KB cartridge window, NAME.bin (fork only)
  reset warm|cold       reset the ST (a cold reset power-cycles the
                        Multi-device too; a warm one leaves it running)
  debug CMD             a Hatari debugger command; its answer goes to
                        hatari.out

Every step fails after its own frame limit, and the whole run after --timeout
seconds of the host's time; Hatari is always stopped. The run folder,
tools/dev/logs/emu/<date>-<setup>-<name>/, keeps command.txt, hatari.out (the
firmware's log with the fork), shots/ (end.png last), sd/ (a copy of emu/sd,
the firmware's card), the disk copies, acsi-files/ (every file on the ACSI
image after the run, with --acsi), printer.out with --printer, and
report.json: PASS or FAIL per step with its frame and time, and the result.
The disks given are never written. Standard library only.

Usage:  emu_harness.py --setup st-104 --test template-menu
        emu_harness.py --setup st-104 --step 'text "Select an option" 500'
"""
import argparse
import datetime
import json
import os
import re
import select
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DEV = Path(__file__).resolve().parent
ROOT = DEV.parent.parent
sys.path.insert(0, str(DEV))
import screens  # noqa: E402

FORK = DEV / "builds/emumd-cache/hatari-fork/build/src/hatari"
# Where the test programs' disks are (downloaded, never committed); a test
# names them as $PROGRAM_DISKS/...
os.environ.setdefault("PROGRAM_DISKS", "/Volumes/MacData/md-printer-emulator-disks")
STOCK = Path("/usr/local/bin/hatari")
PLUGIN = ROOT / "build/md-printer-emulator.mdfw"
TESTS = DEV / "emu_tests"
REFS = DEV / "emu_refs"
SETUPS = {"st-104": ("st", "emu/tos/tos104uk.img"),
          "ste-162": ("ste", "emu/tos/tos162uk.img"),
          "mste-206": ("megaste", "emu/tos/tos206us.img")}


def read_layout(path):
    """The plain numbers a vasm include defines ("NAME equ 12", "equ $4D"):
    the printer hook's layout, as the ST builds it."""
    layout = {}
    for line in path.read_text().splitlines():
        m = re.match(r"(\w+)\s+equ\s+(\$[0-9A-Fa-f]+|\d+)\s*(?:;.*)?$", line)
        if m:
            value = m.group(2)
            layout[m.group(1)] = int(value[1:], 16) if value.startswith("$") else int(value)
    return layout


PRT = read_layout(ROOT / "target/atarist/src/inc/printer_layout.s")
# The RP ends a print job after IDLE_SECONDS without a block: its default.
IDLE_FRAMES = 50 * int(re.search(r'ACONFIG_PARAM_IDLE_SECONDS, SETTINGS_TYPE_INT, "(\d+)"',
                                 (ROOT / "rp/src/aconfig.c").read_text()).group(1))
SCREEN_TOLERANCE = 0.002  # a screen equals its reference: this share of pixels may differ
MOUSE_STEP = 8            # pixels a frame at most: Hatari drops what the IKBD has no room for
MOUSE_PIN = (640, 400)    # more than any ST screen
MOUSE_SETTLE = 10         # frames after a move before anything else
WATCH_SKIP = 64           # printwatch: the ROM breakpoint reports every 64th hit
M68K_SOURCES = ROOT / "target/atarist"
TARGET_FIRMWARE = ROOT / "rp/src/include/target_firmware.h"

# Hatari warns of every bus and address error. TOS's start-up makes some on
# purpose, sizing memory and probing hardware from its ROM; any other is a crash.
CRASH_RE = re.compile(rb"WARN : (Bus|Address) Error \w+ at address \$([0-9a-f]+), PC=\$([0-9a-f]+)")
TOS_ROM = (0xE00000, 0x1000000)
# Hardware programs probe for, catching the bus error when it is absent: not
# crashes. $FFFA40: an SFP-004 FPU (Calamus); $FF8A00: the blitter; $FF8900:
# DMA sound; $FFFC20: a Mega ST's clock.
PROBES = {0xFFFFFA40, 0xFFFF8A00, 0xFFFF8900, 0xFFFFFC20, 0xFFFA40, 0xFF8A00, 0xFF8900, 0xFFFC20}

# Characters Hatari's keypress takes as themselves (letters, digits); the
# rest by scancode: Hatari does not deliver punctuation typed as characters.
TYPE_KEYS = {"\n": "0x1c", " ": "0x39", ".": "0x34", ",": "0x33", "-": "0x0c",
             "/": "0x35", ";": "0x27"}
TYPE_SHIFTED = {":": "0x27"}
# Characters whose key depends on the TOS's keyboard (its country, in the TOS
# header's os_conf): (scancode, shifted).
LAYOUTS = {
    0: {'"': ("0x28", True), "@": ("0x03", True), "'": ("0x28", False),   # USA
        "\\": ("0x2b", False), "_": ("0x0c", True)},
    3: {'"': ("0x03", True), "@": ("0x28", True), "'": ("0x28", False),   # UK
        "\\": ("0x60", False), "_": ("0x0c", True)},
}


def is_desktop(screen):
    """The TOS desktop in colour: green and white or grey (borders included).
    TOS 2.06 dithers its green with white on alternate pixels, so the screen
    is sampled every third pixel (every eighth would see only the white);
    a window open on it leaves about a quarter green."""
    share = screens.colours(screen, step=3)
    green = sum(v for (r, g, b), v in share.items() if r == 0 and b == 0 and g >= 200)
    light = sum(v for (r, g, b), v in share.items() if r == g == b and r >= 200)
    return green > 0.15 and light > 0.2


class StepFailed(Exception):
    pass


def crashes(out_bytes):
    found = []
    for m in CRASH_RE.finditer(out_bytes):
        if int(m.group(2), 16) in PROBES:
            continue
        if not TOS_ROM[0] <= int(m.group(3), 16) < TOS_ROM[1]:
            found.append(m.group(0).decode(errors="replace")[7:])
    return found


def m68k_stale():
    """The m68k sources newer than the target_firmware.h built from them."""
    built = TARGET_FIRMWARE.stat().st_mtime
    sources = [p for p in (M68K_SOURCES / "src").rglob("*") if p.is_file()]
    sources.append(M68K_SOURCES / "Makefile")
    return [p for p in sources if p.stat().st_mtime > built]


def build():
    stale = m68k_stale()
    if stale:
        print(f"emu_harness: m68k sources changed ({stale[0].relative_to(ROOT)}...): "
              "target/atarist/build.sh", flush=True)
        subprocess.run(["./build.sh", str(M68K_SOURCES), "release", "1"],
                       cwd=M68K_SOURCES, check=True)
        if m68k_stale():
            raise SystemExit("emu_harness: target_firmware.h is still older than the m68k sources")
    subprocess.run([str(DEV / "mdfw.sh"), "build"], check=True,
                   stdout=subprocess.DEVNULL)


class Hatari:
    """A running Hatari, its output (through a pseudo-terminal: through a file
    it arrives late) and its control socket."""

    def __init__(self, cmd, env, run_dir):
        self.out_path = run_dir / "hatari.out"
        self.shots = run_dir / "shots"
        self.sock_dir = tempfile.mkdtemp(prefix="eh", dir="/tmp")  # macOS: 104-byte paths
        sock_path = os.path.join(self.sock_dir, "ctl.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(sock_path)
        server.listen(1)
        server.settimeout(30)
        master, slave = os.openpty()
        self.proc = subprocess.Popen(cmd + ["--control-socket", sock_path], env=env,
                                     cwd=run_dir, stdin=subprocess.DEVNULL,
                                     stdout=slave, stderr=slave)
        os.close(slave)
        self.master = master
        self.out = open(self.out_path, "wb")
        try:
            self.conn, _ = server.accept()
        except socket.timeout:
            self.stop()
            raise RuntimeError("Hatari never opened its control socket")
        finally:
            server.close()
        self.deadline = float("inf")

    def pump(self):
        while select.select([self.master], [], [], 0)[0]:
            try:
                data = os.read(self.master, 65536)
            except OSError:
                return
            if not data:
                return
            self.out.write(data)
        self.out.flush()

    def output(self):
        self.pump()
        return self.out_path.read_bytes()

    def send(self, text):
        # Hatari takes one command per read of its socket: two sent together
        # lose the second.
        self.conn.sendall((text + "\n").encode())
        time.sleep(0.05)

    def vbl(self, timeout=10):
        """The frame count, from the debugger's VBL variable."""
        if time.time() > self.deadline:
            raise StepFailed("the run's time limit (--timeout) is over")
        self.pump()
        mark = self.out_path.stat().st_size
        self.send("hatari-debug e VBL")
        end = time.time() + timeout
        while time.time() < end:
            time.sleep(0.05)
            self.pump()
            with open(self.out_path, "rb") as f:
                f.seek(mark)
                m = re.search(rb"#(\d+) \(dec\)", f.read())
            if m:
                return int(m.group(1))
            if self.proc.poll() is not None:
                raise StepFailed("Hatari stopped")
        raise StepFailed("no answer from Hatari's debugger")

    def shot(self, name):
        before = set(os.listdir(self.shots))
        self.send("hatari-shortcut screenshot")
        end = time.time() + 10
        while time.time() < end:
            self.pump()
            new = [f for f in os.listdir(self.shots) if f not in before]
            if new:
                size = -1
                while time.time() < end:  # complete: non-empty, no longer growing
                    now = (self.shots / new[0]).stat().st_size
                    if now and now == size:
                        os.replace(self.shots / new[0], self.shots / f"{name}.png")
                        return self.shots / f"{name}.png"
                    size = now
                    time.sleep(0.2)
            time.sleep(0.1)
        raise StepFailed(f"no screenshot {name}")

    def stop(self):
        self.pump()
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.pump()
        self.out.close()
        os.close(self.master)
        shutil.rmtree(self.sock_dir, ignore_errors=True)


class Run:
    def __init__(self, hatari, args, run_dir):
        self.hatari, self.args, self.run_dir = hatari, args, run_dir
        self.start = time.time()
        self.shot_count = 0
        self.pointer = None  # where the pointer is, once pinned
        self.report_extra = {}  # what steps add to report.json
        self.card_volume = None  # cardfull's disk image, mounted on sd/
        tos = (ROOT / SETUPS[args.setup][1]).read_bytes()[0x1C:0x1E]
        self.tos_country = int.from_bytes(tos, "big") >> 1  # os_conf's country
    def frames_from_now(self, frames):
        return self.hatari.vbl() + int(frames)

    def wait_frames(self, frames):
        target = self.frames_from_now(frames)
        while self.hatari.vbl() < target:
            time.sleep(0.05)

    def wait_for(self, frames, check, what):
        """Calls check() until it returns a true value or FRAMES frames pass."""
        deadline = self.frames_from_now(frames)
        while True:
            found = check()
            if found:
                return found
            if self.hatari.vbl() > deadline:
                raise StepFailed(f"{what} not seen after {frames} frames")
            time.sleep(0.1)

    def need_fork(self, step):
        if self.args.hatari != "fork":
            raise StepFailed(f"{step} needs the fork (the firmware's plugin)")

    def request(self, name, answer, frames=100):
        """Asks the firmware for something (emu/template_extra.c) and waits for it."""
        self.need_fork(name)
        out = self.run_dir / "requests" / answer
        out.unlink(missing_ok=True)
        (self.run_dir / "requests" / f"{name}.req").touch()
        return self.wait_for(frames, lambda: out if out.exists() else None,
                             f"the firmware's answer to {name}")

    # -- steps --------------------------------------------------------------

    def step_wait(self, frames):
        self.wait_frames(frames)

    def press(self, key, frames=None):
        """Without FRAMES, the key goes down and up in one event: released
        by the harness, a key stays down until its next look at the frame
        count, several frames late, and TOS repeats it when that comes
        late enough."""
        if frames is None:
            self.hatari.send(f"hatari-event keypress {key}")
        else:
            self.hatari.send(f"hatari-event keydown {key}")
            self.wait_frames(frames)
            self.hatari.send(f"hatari-event keyup {key}")
        self.wait_frames(2)

    def step_key(self, key, frames=None):
        self.press(key, None if frames is None else int(frames))

    def step_keydown(self, key):
        self.hatari.send(f"hatari-event keydown {key}")
        self.wait_frames(2)

    def step_keyup(self, key):
        self.hatari.send(f"hatari-event keyup {key}")
        self.wait_frames(2)

    def step_type(self, *words):
        """Types TEXT: letters, digits and the layout's own characters
        (LAYOUTS: " @ ' \\ _) by the key the running TOS expects."""
        text = " ".join(words).encode().decode("unicode_escape")
        layout = LAYOUTS.get(self.tos_country, {})
        for ch in text:
            if ch in layout:
                key, shifted = layout[ch]
                if shifted:
                    self.hatari.send("hatari-event keydown 0x2a")
                self.press(key)
                if shifted:
                    self.hatari.send("hatari-event keyup 0x2a")
            elif ch in TYPE_SHIFTED:
                self.hatari.send("hatari-event keydown 0x2a")
                self.press(TYPE_SHIFTED[ch])
                self.hatari.send("hatari-event keyup 0x2a")
            else:
                self.press(TYPE_KEYS.get(ch, ch))

    def move_mouse(self, dx, dy):
        while dx or dy:
            step = self.args.mouse_step
            sx = max(-step, min(step, dx))
            sy = max(-step, min(step, dy))
            self.hatari.send(f"hatari-event mousemove {sx} {sy}")
            self.wait_frames(1)
            dx, dy = dx - sx, dy - sy
        self.wait_frames(MOUSE_SETTLE)

    def step_mouse(self, x, y):
        # Pinned in the top left corner the first time only: later moves go
        # from where the pointer is, since crossing GEM's menu bar on the way
        # to the corner opens its menus. Positions assume a pixel per mouse
        # unit, as TOS's pointer moves.
        x, y = int(x, 0), int(y, 0)
        if self.pointer is None:
            self.move_mouse(-MOUSE_PIN[0], -MOUSE_PIN[1])
            self.pointer = (0, 0)
        self.move_mouse(x - self.pointer[0], y - self.pointer[1])
        self.pointer = (x, y)

    def step_mousemove(self, dx, dy):
        self.move_mouse(int(dx, 0), int(dy, 0))
        if self.pointer is not None:
            self.pointer = (self.pointer[0] + int(dx, 0), self.pointer[1] + int(dy, 0))

    def step_click(self, *words):
        words = list(words)
        button = words.pop(0) if words and words[0] in ("left", "right") else "left"
        self.hatari.send(f"hatari-event {button}down")
        self.wait_frames(int(words[0]) if words else 5)
        self.hatari.send(f"hatari-event {button}up")
        self.wait_frames(2)

    def step_dblclick(self, gap_ms="100"):
        """A left double click: down, up, down, up, GAP_MS host milliseconds
        apart (Hatari reads its control socket once a frame or so, and the
        ST's keyboard processor reports a button change only if it lasts)."""
        for event in ("leftdown", "leftup", "leftdown", "leftup"):
            self.hatari.conn.sendall(f"hatari-event {event}\n".encode())
            time.sleep(int(gap_ms) / 1000)
        self.wait_frames(10)

    def step_hdblclick(self):
        """Hatari's own double click event."""
        self.hatari.send("hatari-event doubleclick")
        self.wait_frames(10)

    def step_shot(self, name):
        self.hatari.shot(name)

    def step_log(self, regex, frames):
        pattern = re.compile(regex.encode())
        self.wait_for(frames, lambda: pattern.search(self.hatari.output()),
                      f"log /{regex}/")

    def step_newlog(self, regex, frames):
        pattern = re.compile(regex.encode())
        mark = len(self.hatari.output())
        self.wait_for(frames, lambda: pattern.search(self.hatari.output()[mark:]),
                      f"new log /{regex}/")

    def step_text(self, regex, frames):
        pattern = re.compile(regex)

        def check():
            path = self.request("text", "text.txt", int(frames))
            text = path.read_text()
            shutil.copy(path, self.run_dir / "text-last.txt")
            return pattern.search(text)
        self.wait_for(frames, check, f"terminal text /{regex}/")

    def step_screen(self, mode, name, frames):
        if mode not in ("same", "differs"):
            raise StepFailed(f"screen {mode}: same or differs")
        ref = REFS / self.args.setup / f"{name}.png"
        if self.args.update_refs and mode == "same":
            self.wait_frames(int(frames) // 2)
            ref.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(self.hatari.shot(f"ref-{name}"), ref)
            return
        if not ref.exists():
            raise StepFailed(f"no reference {ref.relative_to(ROOT)} (--update-refs makes it)")
        expected = screens.load(ref)

        def check():
            self.shot_count += 1
            shot = screens.load(self.hatari.shot(f"screen-{name}-{self.shot_count}"))
            diff = screens.difference(shot, expected)
            return diff <= SCREEN_TOLERANCE if mode == "same" else diff > SCREEN_TOLERANCE
        self.wait_for(frames, check, f"screen {mode} {name}")

    def step_desktop(self, frames):
        def check():
            self.shot_count += 1
            return is_desktop(screens.load(self.hatari.shot(f"desktop-{self.shot_count}")))
        self.wait_for(frames, check, "the TOS desktop")

    def step_printer(self, nbytes, frames, quiet="500"):
        """Wait until the printer port has received at least NBYTES bytes
        (--printer), and then until it has been quiet for QUIET frames (500:
        programs pause between passes and pages, Signum for over 100)."""
        out = self.run_dir / "printer.out"
        if not self.args.printer:
            raise StepFailed("printer needs --printer")
        self.wait_for(frames, lambda: out.exists() and out.stat().st_size >= int(nbytes),
                      f"{nbytes} bytes on the printer port")
        size = -1
        while out.stat().st_size != size:
            size = out.stat().st_size
            self.wait_frames(int(quiet))

    def step_printwatch(self):
        """Breakpoints that count every entry into TOS's printer routine
        (xconout[0], read from $57E): those called from TOS's ROM (the BIOS
        trap's dispatch, the screen dump through prv_lst) and those called
        from RAM (a program calling the routine itself). The report's
        "printwatch" puts them next to the bytes the printer port got
        (--printer): more bytes than entries means the program drives the
        port's hardware."""
        mark = len(self.hatari.output())
        self.hatari.send("hatari-debug m $57e 4")
        found = self.wait_for(100, lambda: re.search(
            rb"0000057[eE]: ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2})",
            self.hatari.output()[mark:]), "xconout[0]")
        routine = int(b"".join(found.groups()), 16)
        rom = 0xFC0000 if routine >= 0xFC0000 else 0xE00000
        self.printwatch = {"routine": f"${routine:06x}", "rom": f"${rom:06x}"}
        # Every 64th hit only: a line for each of a screen dump's 64 KB slows
        # Hatari to a crawl. Calls from RAM are rare and counted exactly.
        self.hatari.send(f"hatari-debug b pc = ${routine:x} && (a7).l > ${rom - 1:x} :trace :{WATCH_SKIP}")
        self.hatari.send(f"hatari-debug b pc = ${routine:x} && (a7).l < ${rom:x} :trace")
        self.wait_frames(2)

    def printwatch_counts(self):
        """The breakpoints' hits, from Hatari's output, by caller."""
        if not getattr(self, "printwatch", None):
            return None
        text = self.hatari.out_path.read_bytes().decode("latin1")
        hits = re.findall(r"CPU breakpoint condition\(s\) matched (\d+) times\.\s*\n\s*(.*)", text)
        result = dict(self.printwatch)
        rom = [int(n) for n, h in hits if "> $" in h]
        result["from_rom"] = f"{max(rom, default=0)} to {max(rom, default=0) + WATCH_SKIP - 1}"
        result["from_ram"] = sum(1 for n, h in hits if "< $" in h)
        out = self.run_dir / "printer.out"
        result["port_bytes"] = out.stat().st_size if out.exists() else 0
        return result

    def read_mem(self, address, length):
        """ST memory, through the debugger's savebin."""
        path = self.run_dir / "requests" / f"mem-{address:06x}-{length}.bin"
        path.unlink(missing_ok=True)
        self.hatari.send(f"hatari-debug savebin {path} ${address:x} ${length:x}")
        self.wait_for(100, lambda: path.exists() and path.stat().st_size == length,
                      f"{length} bytes at ${address:06x}")
        return path.read_bytes()

    def xbra_entry(self, vector):
        """The routine of the printer hook in the XBRA chain from VECTOR (a
        system variable), or None: each routine follows 'XBRA', its id and
        the vector it replaced."""
        entry = int.from_bytes(self.read_mem(vector, 4), "big")
        for _ in range(16):
            if not entry:
                return None
            header = self.read_mem(entry - 12, 12)
            if header[:4] != b"XBRA":
                return None
            if int.from_bytes(header[4:8], "big") == PRT["PRT_XBRA_ID"]:
                return entry
            entry = int.from_bytes(header[8:12], "big")
        return None

    def hook_base(self):
        """The printer hook's state: its output routine, which xconout[0]
        reaches, is PRT_CONOUT_ENTRY bytes in (inc/printer_layout.s)."""
        entry = self.xbra_entry(PRT["xconout0"])
        if entry is None:
            raise StepFailed("the printer hook is not in xconout[0]'s XBRA chain")
        return entry - PRT["PRT_CONOUT_ENTRY"]

    def step_hookstate(self, name="hookstate"):
        """The printer hook, resident (inc/printer_layout.s): found from
        xconout[0], its state saved as NAME.bin, its counters and buffer in
        the report; FAILs unless the four vectors and a VBL slot reach its
        routines."""
        base = self.hook_base()
        block = self.read_mem(base, PRT["PRT_STATE_BYTES"])
        (self.run_dir / f"{name}.bin").write_bytes(block)
        word = lambda o: int.from_bytes(block[o:o + 4], "big")
        state = {"state": f"${base:06x}",
                 "stamp": word(0) == PRT["PRT_MAGIC"] and word(PRT["PRT_SELF"]) == base,
                 "taken": word(PRT["PRT_TAKEN"]), "dropped": word(PRT["PRT_DROPPED"]),
                 "buffered": word(PRT["PRT_WIDX"]), "sequence": word(PRT["PRT_SEQ"]),
                 "sent": word(PRT["PRT_SENT"]), "failed": word(PRT["PRT_FAILED"]),
                 "machine": f"${word(PRT['PRT_MACHINE']):08x}"}
        for vname in ("xconout0", "xcostat0", "prv_lst", "prv_lsto"):
            value = int.from_bytes(self.read_mem(PRT[vname], 4), "big")
            entry = self.xbra_entry(PRT[vname])
            state[vname] = {"vector": f"${value:06x}", "hooked": entry is not None}
            if entry is not None:
                previous = int.from_bytes(self.read_mem(entry - 4, 4), "big")
                state[vname]["previous"] = f"${previous:06x}"
        nvbls = int.from_bytes(self.read_mem(PRT["nvbls_var"], 2), "big")
        queue = int.from_bytes(self.read_mem(PRT["vblqueue_var"], 4), "big")
        slots = self.read_mem(queue, nvbls * 4) if queue and nvbls else b""
        slots = [int.from_bytes(slots[i:i + 4], "big") for i in range(0, len(slots), 4)]
        ours = lambda a: a > 12 and self.read_mem(a - 12, 8) == b"XBRA" + PRT["PRT_XBRA_ID"].to_bytes(4, "big")
        state["vbl"] = {"nvbls": nvbls, "queue": f"${queue:06x}",
                        "slots": [f"${a:06x}" for a in slots],
                        "hooked": any(ours(a) for a in slots if a)}
        buffer = word(PRT["PRT_BUFFER"])
        held = min(word(PRT["PRT_WIDX"]), PRT["PRT_BUF_SIZE"])
        state["buffer"] = self.read_mem(buffer, held).decode("latin1") if buffer and held else ""
        self.report_extra[name] = state
        (self.run_dir / f"{name}.json").write_text(json.dumps(state, indent=2) + "\n")
        vectors = ("xconout0", "xcostat0", "prv_lst", "prv_lsto")
        if not state["stamp"] or not all(state[v]["hooked"] for v in vectors) or not state["vbl"]["hooked"]:
            raise StepFailed(f"the printer hook is not installed: {json.dumps(state)[:300]}")

    def step_svar(self, index, value):
        """Shared variable INDEX (hex) becomes VALUE (hex), as the firmware
        writes it (emu/template_extra.c's svar request)."""
        self.need_fork("svar")
        done = self.run_dir / "requests" / "svar.done"
        done.unlink(missing_ok=True)
        (self.run_dir / "requests" / "svar.req.tmp").write_text(f"{index} {value}\n")
        os.replace(self.run_dir / "requests" / "svar.req.tmp", self.run_dir / "requests" / "svar.req")
        self.wait_for(100, lambda: done.exists(), "the firmware's answer to svar")

    def step_setting(self, key, value):
        """The app's setting KEY becomes VALUE, saved (emu/template_extra.c's
        setting request)."""
        self.need_fork("setting")
        done = self.run_dir / "requests" / "setting.done"
        done.unlink(missing_ok=True)
        (self.run_dir / "requests" / "setting.req.tmp").write_text(f"{key} {value}\n")
        os.replace(self.run_dir / "requests" / "setting.req.tmp", self.run_dir / "requests" / "setting.req")
        self.wait_for(1500, lambda: done.exists(), "the firmware's answer to setting")
        result = done.read_text().strip()
        if result != "ok":
            raise StepFailed(f"setting {key} {value}: {result}")

    def wait_hook(self, nbytes, frames, quiet):
        """Waits until the printer hook has taken at least NBYTES bytes since
        the ST booted, then until it has taken nothing for QUIET frames;
        returns a function that reads its counters (taken, dropped,
        buffered)."""
        base = self.hook_base()
        read = lambda o: int.from_bytes(self.read_mem(base + PRT[o], 4), "big")
        counters = lambda: (read("PRT_TAKEN"), read("PRT_DROPPED"), read("PRT_WIDX"))
        self.wait_for(frames, lambda: counters()[0] >= int(nbytes),
                      f"{nbytes} bytes taken by the printer hook")
        taken = -1
        while counters()[0] != taken:
            taken = counters()[0]
            self.wait_frames(int(quiet))
        return counters

    def step_cardfile(self, pattern, nbytes, frames, quiet="500"):
        """Wait until the printer hook has taken NBYTES and then nothing for
        QUIET frames (as the printer step's quiet), and until the files
        matching PATTERN on the firmware's card (the run's sd/; a job is a
        file once it has ended) hold every byte it took."""
        self.need_fork("cardfile")
        counters = self.wait_hook(nbytes, frames, quiet)
        card = lambda: sum(f.stat().st_size for f in (self.run_dir / "sd").glob(pattern))
        def complete():
            taken, dropped, buffered = counters()
            return buffered == 0 and card() == taken - dropped
        # A job is a file once the RP's idle timeout has ended it.
        self.wait_for(2 * IDLE_FRAMES, complete, f"{pattern} with every byte the hook took")

    def step_hookdone(self, nbytes, frames, quiet="500"):
        """Wait as cardfile does, then FAIL unless the RP answered every
        block: the buffer is empty and nothing was dropped."""
        self.need_fork("hookdone")
        counters = self.wait_hook(nbytes, frames, quiet)
        self.wait_for(100, lambda: counters()[2] == 0, "the hook's buffer sent")
        taken, dropped, buffered = counters()
        if dropped:
            raise StepFailed(f"the hook dropped {dropped} of {taken} bytes")

    def step_cardput(self, path):
        """An empty file PATH on the firmware's card: where the firmware
        expects a folder, the card fails for it."""
        (self.run_dir / "sd" / path).parent.mkdir(parents=True, exist_ok=True)
        (self.run_dir / "sd" / path).write_bytes(b"")

    def step_cardfull(self, free):
        """The card full but FREE bytes: a 1 MB FAT volume mounted over the
        run's sd/ (the firmware opens its files by path, so it writes there
        from now on), its files copied onto it, then one file that leaves
        FREE bytes free. release_card() unmounts it at the end."""
        sd = self.run_dir / "sd"
        image = self.run_dir / "card-full.dmg"
        subprocess.run(["hdiutil", "create", "-size", "1m", "-fs", "MS-DOS FAT12",
                        "-volname", "CARD", "-layout", "NONE", str(image)],
                       check=True, capture_output=True)
        shutil.copytree(sd, self.run_dir / "card-before", dirs_exist_ok=True)
        subprocess.run(["hdiutil", "attach", "-nobrowse", "-mountpoint", str(sd), str(image)],
                       check=True, capture_output=True)
        self.card_volume = image
        shutil.copytree(self.run_dir / "card-before", sd, dirs_exist_ok=True)
        # Filled until the volume refuses (its reported free space is more
        # than a file can take), then FREE bytes given back, in clusters.
        fs = os.statvfs(sd)
        cluster = fs.f_frsize
        with open(sd / "FILLER.BIN", "wb", buffering=0) as f:  # a refused write is not kept
            try:
                while f.write(bytes(cluster)):
                    pass
            except OSError:
                pass
            size = f.seek(0, os.SEEK_END) // cluster * cluster
            give = -(-int(free) // cluster) * cluster
            if give > size:
                raise StepFailed(f"cardfull {free}: the volume holds only {size} bytes")
            f.truncate(size - give)
        fs = os.statvfs(sd)
        self.report_extra["cardfull"] = {"free": fs.f_bavail * fs.f_frsize, "cluster": fs.f_frsize}

    def release_card(self):
        """cardfull's volume: its files into card-full/, then unmounted."""
        if self.card_volume is None:
            return
        sd = self.run_dir / "sd"
        shutil.copytree(sd, self.run_dir / "card-full", dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns(".*"))
        subprocess.run(["hdiutil", "detach", str(sd)], capture_output=True)
        self.card_volume = None

    def step_cardnone(self, pattern):
        """FAILs if a file on the firmware's card matches PATTERN."""
        found = sorted(str(f.relative_to(self.run_dir / "sd")) for f in (self.run_dir / "sd").glob(pattern))
        if found:
            raise StepFailed(f"on the card: {', '.join(found)}")

    def step_cardsame(self, path, expected):
        """FAILs unless PATH on the firmware's card equals the file EXPECTED
        (relative to the repository, or card:PATH on the card), byte for
        byte."""
        got = (self.run_dir / "sd" / path).read_bytes()
        if expected.startswith("card:"):
            want = (self.run_dir / "sd" / expected[5:]).read_bytes()
        else:
            want = (ROOT / expected).read_bytes()
        if got != want:
            first = next((i for i, (a, b) in enumerate(zip(got, want)) if a != b), min(len(got), len(want)))
            raise StepFailed(f"{path}: {len(got)} bytes, {expected}: {len(want)}; first difference at {first}")

    def step_rom4(self, name):
        shutil.copy(self.request("rom4", "rom4.bin"), self.run_dir / f"{name}.bin")

    def step_reset(self, kind):
        if kind not in ("warm", "cold"):
            raise StepFailed(f"reset {kind}: warm or cold")
        self.hatari.send(f"hatari-shortcut {kind}reset")

    def step_cartexec(self, what):
        """watch: a breakpoint on the first instruction run in the
        cartridge, which prints the registers and removes itself; none:
        FAILs if it has been hit since the watch began."""
        cond = "pc > $f9ffff && pc < $fb0000"
        if what == "watch":
            regs = self.run_dir / "requests" / "cartexec.dbg"
            regs.write_text("r\n")
            self.cartexec_mark = len(self.hatari.output())
            self.hatari.send(f"hatari-debug b {cond} :trace :once :file {regs}")
            self.wait_frames(2)
        elif what == "none":
            self.wait_frames(2)
            out = self.hatari.output()[self.cartexec_mark:].decode("latin1")
            hit = re.search(r"breakpoint condition\(s\) matched \d+ times\.\s*\n\s*pc > \$f9ffff.*", out)
            if hit:
                # The register dump ends with the instruction at the PC.
                pc = re.search(r"\nPrefetch [^\n]*\n([0-9a-f]{8}) ", out[hit.start():])
                where = f" at ${int(pc.group(1), 16):06x}" if pc else ""
                raise StepFailed(f"the CPU ran code in the cartridge{where}")
        else:
            raise StepFailed(f"cartexec {what}: watch or none")

    def step_debug(self, *words):
        self.hatari.send("hatari-debug " + " ".join(words))


def acsi_files(image, out):
    """Every file of the image's first partition, copied out with mtools."""
    with open(image, "rb") as f:
        mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa":
        return "no MBR"
    lba = int.from_bytes(mbr[0x1C6:0x1CA], "little")
    out.mkdir()
    r = subprocess.run(["mcopy", "-s", "-n", "-m", "-i", f"{image}@@{lba * 512}", "::/*", str(out)],
                       env=dict(os.environ, MTOOLS_SKIP_CHECK="1"),
                       capture_output=True, text=True)
    count = sum(1 for p in out.rglob("*") if p.is_file())
    return f"{count} files" if r.returncode == 0 else f"mcopy failed: {r.stderr.strip()[:200]}"


def load_steps(args):
    """The tests' steps, then --step's. A test's line "@disk-a PATH" (or
    @disk-b, @acsi) gives the run that disk, PATH relative to the
    repository, unless the command line gives one; "@hatari stock|fork" and
    "@printer" set those options."""
    steps = []
    for name in args.test:
        path = TESTS / f"{name}.steps"
        for line in path.read_text().splitlines():
            line = line.strip()
            if line.startswith("@"):
                option, _, value = line[1:].partition(" ")
                attr = option.replace("-", "_")
                if attr in ("disk_a", "disk_b", "acsi"):
                    if not getattr(args, attr):
                        disk = os.path.expandvars(value.strip())
                        setattr(args, attr, str(ROOT / disk))  # absolute stays absolute
                elif attr == "hatari":
                    args.hatari = value.strip()
                elif attr == "printer":
                    args.printer = True
                elif attr == "monitor":
                    args.monitor = value.strip()
                elif attr == "memsize":
                    args.memsize = int(value.strip())
                elif attr == "mouse_step":
                    args.mouse_step = int(value.strip())
                elif attr == "keep_raw":
                    args.keep_raw = value.strip()
                else:
                    raise SystemExit(f"{path.name}: unknown option {line}")
            elif line and not line.startswith("#"):
                steps.append(line)
    steps += args.step
    if args.via_firmware:
        args.hatari = "fork"  # the firmware is only in the fork
        # The firmware's setup menu first: [F]irmware installs the printer
        # hook, switched off first if asked (shared variable 2).
        first = ['text "Atari +: (ST|STE|Mega STE), TOS" 1500']
        if args.hook == "off":
            first.append("svar 2 0")
        first += ["key f", "key 0x1c"]
        steps = first + steps
        if args.hook == "on":
            # What reaches the printer port with the hook off reaches the card.
            steps = [re.sub(r"^printer (\d+ \d+(?: \d+)?)$",
                            r"cardfile PRINTER/*.PRN \1", st) for st in steps]
    if args.hatari == "fork" and args.keep_raw == "on":
        # The tests compare the jobs' bytes: the firmware deletes a job once
        # its PDF is written, unless KEEP_RAW is on. After what a test puts
        # on the card before the firmware boots: the setting waits for it.
        first = 0
        while first < len(steps) and steps[first].startswith("cardput "):
            first += 1
        steps.insert(first, "setting KEEP_RAW true")
    return steps


def hatari_command(args, run_dir):
    machine, tos = SETUPS[args.setup]
    binary = STOCK if args.hatari == "stock" else FORK
    cmd = [str(binary), "--tos", str(ROOT / tos), "--machine", machine,
           "--memsize", str(args.memsize), "--sound", "off", "--statusbar", "off",
           "--drive-led", "off", "--harddrive", "none",
           "--screenshot-dir", str(run_dir / "shots"),
           "--fast-forward", "on" if args.fast else "off"]
    cmd += ["--window"]
    for drive, disk in (("a", args.disk_a), ("b", args.disk_b)):
        if disk:
            copy = run_dir / Path(disk).name
            shutil.copy(disk, copy)
            cmd += [f"--disk-{drive}", str(copy), "--protect-floppy", "off"]
        elif drive == "b":
            cmd += ["--drive-b", "off"]
    if args.acsi:
        copy = run_dir / Path(args.acsi).name
        shutil.copy(args.acsi, copy)
        cmd += ["--acsi", f"0={copy}"]
    if args.printer:
        cmd += ["--printer", str(run_dir / "printer.out")]
    if args.trace:
        cmd += ["--trace", args.trace]
    if args.monitor:
        cmd += ["--monitor", args.monitor]
    if args.hatari == "fork":
        shutil.copytree(ROOT / "emu/sd", run_dir / "sd")
        cmd += ["--md-sd", str(run_dir / "sd"), "--md-verbose", "on",
                "--md-firmware", str(PLUGIN)]
    return cmd


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split("\n\n", 1)[1])
    ap.add_argument("--setup", choices=SETUPS, default="st-104")
    ap.add_argument("--hatari", choices=("fork", "stock", "bare"), default="fork")
    ap.add_argument("--test", action="append", default=[], help="tools/dev/emu_tests/NAME.steps")
    ap.add_argument("--step", action="append", default=[])
    ap.add_argument("--name", help="the run folder's suffix (default: the tests' names)")
    ap.add_argument("--disk-a")
    ap.add_argument("--disk-b")
    ap.add_argument("--acsi", help="an ACSI hard-disk image, ACSI ID 0")
    ap.add_argument("--printer", action="store_true", help="Hatari's printer port to printer.out")
    ap.add_argument("--trace", help="Hatari's --trace, e.g. gemdos,bios,xbios (into hatari.out)")
    ap.add_argument("--monitor", choices=("mono", "rgb"),
                    help="Hatari's monitor: mono boots TOS in high resolution, 640 x 400")
    ap.add_argument("--mouse-step", type=int, default=MOUSE_STEP,
                    help="pixels a frame a mouse move goes at most (a program that "
                         "accelerates the pointer needs fewer)")
    ap.add_argument("--memsize", type=int, default=1, help="ST RAM in MB (0 is 512 KB)")
    ap.add_argument("--fast", action="store_true", help="fast-forward")
    ap.add_argument("--window", action="store_true", help="show Hatari's window")
    ap.add_argument("--update-refs", action="store_true")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--via-firmware", action="store_true",
                    help="(fork) pass the firmware's setup menu with [F]irmware first, which "
                         "installs the printer hook")
    ap.add_argument("--hook", choices=("on", "off"), default="on",
                    help="with --via-firmware: off clears the printer's switch first, so the "
                         "printer port gets what the hook would")
    ap.add_argument("--keep-raw", choices=("on", "off"), default="on",
                    help="(fork) on: the firmware keeps each job's bytes beside its PDF (the "
                         "KEEP_RAW setting), for the tests that compare them")
    ap.add_argument("--timeout", type=float, default=300, help="host seconds for the whole run")
    args = ap.parse_args()

    steps = load_steps(args)
    # Hatari puts its own cartridge code in place of the firmware's whenever
    # one of these traces is on (cart.c, Cart_UseBuiltinCartridge).
    cart_traces = {"gemdos", "os_base", "vdi", "aes", "os_all", "all"}
    if args.hatari == "fork" and args.trace and cart_traces & set(args.trace.split(",")):
        ap.error("--trace " + args.trace + ": Hatari would replace the firmware's cartridge "
                 "(gemdos, os_base, vdi and aes need Hatari's own); use bios, xbios or --hatari stock")
    if not steps:
        ap.error("no steps: --test or --step")
    if args.hatari == "fork" and not args.no_build:
        build()

    name = args.name or "+".join(args.test) or "steps"
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = DEV / "logs/emu" / f"{stamp}-{args.setup}-{args.hatari}-{name}"
    (run_dir / "shots").mkdir(parents=True)
    (run_dir / "requests").mkdir()
    (run_dir / "home").mkdir()  # Hatari reads no saved configuration
    cmd = hatari_command(args, run_dir)
    env = dict(os.environ, HOME=str(run_dir / "home"),
               EMU_REQUESTS=str(run_dir / "requests"))
    if not args.window:
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    (run_dir / "command.txt").write_text(shlex.join(cmd) + "\n")
    print(f"emu_harness: {run_dir.relative_to(ROOT)}", flush=True)

    report = {"setup": args.setup, "hatari": args.hatari, "tests": args.test,
              "command": cmd, "steps": [], "result": "FAIL"}
    hatari = Hatari(cmd, env, run_dir)
    hatari.deadline = time.time() + args.timeout
    run = Run(hatari, args, run_dir)
    failed = None
    try:
        for step in steps:
            words = shlex.split(step)
            entry = {"step": step}
            t0 = time.time()
            try:
                handler = getattr(run, "step_" + words[0], None)
                if handler is None:
                    raise StepFailed(f"unknown step {words[0]}")
                handler(*words[1:])
                entry["status"] = "PASS"
            except (StepFailed, TypeError, ValueError) as e:
                entry["status"] = "FAIL"
                entry["detail"] = str(e)
                failed = step
            try:
                entry["frame"] = hatari.vbl()
            except StepFailed:
                pass
            entry["seconds"] = round(time.time() - t0, 2)
            report["steps"].append(entry)
            print(f"  {entry['status']}  {step}" + (f"  ({entry['detail']})" if "detail" in entry else ""),
                  flush=True)
            if failed:
                break
        try:
            hatari.shot("end")
        except StepFailed:
            pass
    finally:
        hatari.stop()
        run.release_card()
    if args.acsi:
        report["acsi_files"] = acsi_files(run_dir / Path(args.acsi).name, run_dir / "acsi-files")
        print(f"  ACSI image's files in acsi-files/: {report['acsi_files']}")
    report.update(run.report_extra)
    watch = run.printwatch_counts()
    if watch:
        report["printwatch"] = watch
        print(f"  printwatch: {watch['port_bytes']} bytes on the port; the printer routine "
              f"{watch['routine']} entered {watch['from_rom']} times from ROM, "
              f"{watch['from_ram']} from RAM")
    report["crashes"] = crashes(hatari.out_path.read_bytes())
    for c in report["crashes"]:
        print(f"  CRASH {c}")
    report["result"] = "PASS" if not failed and not report["crashes"] else "FAIL"
    report["seconds"] = round(time.time() - run.start, 1)
    (run_dir / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"emu_harness: {report['result']} in {report['seconds']} s")
    return 0 if report["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
