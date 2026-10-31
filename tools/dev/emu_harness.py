#!/usr/bin/env python3
"""Run the firmware in emumd's Hatari (or a program in stock Hatari), drive it
with steps, and end in PASS or FAIL with the evidence.

  --hatari fork   emumd's fork of Hatari with this firmware as its Multi-device
                  plugin (build/md-printer-emulator.mdfw): the cartridge boots
                  from the firmware's own image, the RP side is the firmware's
                  own C code (the default).
  --hatari stock  stock Hatari (dev: /usr/local/bin/hatari), no cartridge: the
                  reference, e.g. what a program sends to the printer port
                  (--printer).

Setups: st-104 (ST, TOS 1.04), ste-162 (STE, TOS 1.62), mste-206 (Mega STE,
TOS 2.06); TOS images linked into emu/tos/ (gitignored). Hatari runs at the
machine's own speed unless --fast; times in steps are emulated frames (VBLs,
50 a second), read from Hatari's debugger. Hatari never gets a GEMDOS drive
(it would take the first 1 KB of the cartridge): disks are floppies (--disk-a,
--disk-b) and ACSI images (--acsi).

Steps, in order (--step, or --test NAME for tools/dev/emu_tests/NAME.steps;
a test's "@disk-a PATH", "@disk-b PATH" or "@acsi PATH" line gives the run that
disk, relative to the repository, unless the command line does; "@hatari
stock" and "@printer" set those options):

  wait FRAMES           let FRAMES frames pass
  key KEY [FRAMES]      press a key for FRAMES frames (3), then release it: an
                        ST scancode in hex (0x1c is Return) or one character
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
  printer NBYTES FRAMES wait until the printer port has had NBYTES bytes, then
                        until it has been quiet for 100 frames (--printer)
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
STOCK = Path("/usr/local/bin/hatari")
PLUGIN = ROOT / "build/md-printer-emulator.mdfw"
TESTS = DEV / "emu_tests"
REFS = DEV / "emu_refs"
SETUPS = {"st-104": ("st", "emu/tos/tos104uk.img"),
          "ste-162": ("ste", "emu/tos/tos162uk.img"),
          "mste-206": ("megaste", "emu/tos/tos206uk.img")}
SCREEN_TOLERANCE = 0.002  # a screen equals its reference: this share of pixels may differ
MOUSE_STEP = 8            # pixels a frame at most: Hatari drops what the IKBD has no room for
MOUSE_PIN = (640, 400)    # more than any ST screen
MOUSE_SETTLE = 10         # frames after a move before anything else
M68K_SOURCES = ROOT / "target/atarist"
TARGET_FIRMWARE = ROOT / "rp/src/include/target_firmware.h"

# Hatari warns of every bus and address error. TOS's start-up makes some on
# purpose, sizing memory and probing hardware from its ROM; any other is a crash.
CRASH_RE = re.compile(rb"WARN : (Bus|Address) Error \w+ at address \$([0-9a-f]+), PC=\$([0-9a-f]+)")
TOS_ROM = (0xE00000, 0x1000000)

# Characters Hatari's keypress takes as themselves; the rest by scancode.
TYPE_KEYS = {"\n": "0x1c", " ": "0x39"}


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

    def press(self, key, frames=3):
        self.hatari.send(f"hatari-event keydown {key}")
        self.wait_frames(frames)
        self.hatari.send(f"hatari-event keyup {key}")
        self.wait_frames(2)

    def step_key(self, key, frames="3"):
        self.press(key, int(frames))

    def step_keydown(self, key):
        self.hatari.send(f"hatari-event keydown {key}")
        self.wait_frames(2)

    def step_keyup(self, key):
        self.hatari.send(f"hatari-event keyup {key}")
        self.wait_frames(2)

    def step_type(self, *words):
        text = " ".join(words).encode().decode("unicode_escape")
        for ch in text:
            self.press(TYPE_KEYS.get(ch, ch))

    def move_mouse(self, dx, dy):
        while dx or dy:
            sx = max(-MOUSE_STEP, min(MOUSE_STEP, dx))
            sy = max(-MOUSE_STEP, min(MOUSE_STEP, dy))
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

    def step_printer(self, nbytes, frames):
        """Wait until the printer port has received at least NBYTES bytes
        (--printer), and until it has been quiet for 100 frames."""
        out = self.run_dir / "printer.out"
        if not self.args.printer:
            raise StepFailed("printer needs --printer")
        self.wait_for(frames, lambda: out.exists() and out.stat().st_size >= int(nbytes),
                      f"{nbytes} bytes on the printer port")
        size = -1
        while out.stat().st_size != size:
            size = out.stat().st_size
            self.wait_frames(100)

    def step_rom4(self, name):
        shutil.copy(self.request("rom4", "rom4.bin"), self.run_dir / f"{name}.bin")

    def step_reset(self, kind):
        if kind not in ("warm", "cold"):
            raise StepFailed(f"reset {kind}: warm or cold")
        self.hatari.send(f"hatari-shortcut {kind}reset")

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
                        setattr(args, attr, str(ROOT / value.strip()))
                elif attr == "hatari":
                    args.hatari = value.strip()
                elif attr == "printer":
                    args.printer = True
                else:
                    raise SystemExit(f"{path.name}: unknown option {line}")
            elif line and not line.startswith("#"):
                steps.append(line)
    return steps + args.step


def hatari_command(args, run_dir):
    machine, tos = SETUPS[args.setup]
    binary = FORK if args.hatari == "fork" else STOCK
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
    ap.add_argument("--hatari", choices=("fork", "stock"), default="fork")
    ap.add_argument("--test", action="append", default=[], help="tools/dev/emu_tests/NAME.steps")
    ap.add_argument("--step", action="append", default=[])
    ap.add_argument("--name", help="the run folder's suffix (default: the tests' names)")
    ap.add_argument("--disk-a")
    ap.add_argument("--disk-b")
    ap.add_argument("--acsi", help="an ACSI hard-disk image, ACSI ID 0")
    ap.add_argument("--printer", action="store_true", help="Hatari's printer port to printer.out")
    ap.add_argument("--trace", help="Hatari's --trace, e.g. gemdos,bios,xbios (into hatari.out)")
    ap.add_argument("--memsize", type=int, default=1, help="ST RAM in MB (0 is 512 KB)")
    ap.add_argument("--fast", action="store_true", help="fast-forward")
    ap.add_argument("--window", action="store_true", help="show Hatari's window")
    ap.add_argument("--update-refs", action="store_true")
    ap.add_argument("--no-build", action="store_true")
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
    if args.acsi:
        report["acsi_files"] = acsi_files(run_dir / Path(args.acsi).name, run_dir / "acsi-files")
        print(f"  ACSI image's files in acsi-files/: {report['acsi_files']}")
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
