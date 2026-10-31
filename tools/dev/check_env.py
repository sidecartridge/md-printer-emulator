#!/usr/bin/env python3
"""Check what the two benches need on this computer: ok or FAIL per entry,
exit status 1 on any FAIL. Standard library only.

  Emulated (emu/README.md): emumd at its submodule commit, its Hatari fork
  built, the TOS images, mtools, atari-hd, Docker with the Atari toolchain.
  Device (tools/dev/README.md): the ARM toolchain, OpenOCD, the Debug Probe's
  UART, the SDK submodules at rp/build.sh's pins.

Usage:  python3 tools/dev/check_env.py [--emulated | --device]
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

DEV = Path(__file__).resolve().parent
ROOT = DEV.parent.parent
FAILED = []


def check(name, ok, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))
    if not ok:
        FAILED.append(name)


def run(*cmd, cwd=None):
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=60)
        return r.returncode, (r.stdout + r.stderr).strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return 1, str(e)


def submodule(path, want=None):
    """The submodule's commit, and whether it is the one recorded (or `want`,
    a tag) and clean."""
    rc, head = run("git", "-C", str(ROOT / path), "rev-parse", "HEAD")
    if rc:
        return False, "not checked out (git submodule update --init)"
    if want:
        rc, tag = run("git", "-C", str(ROOT / path), "rev-parse", f"{want}^{{commit}}")
        ok = rc == 0 and tag == head
        detail = f"{head[:7]}, want {want}"
    else:
        rc, rec = run("git", "-C", str(ROOT), "ls-tree", "HEAD", path)
        rec = rec.split()[2] if rc == 0 and rec else ""
        if not rec:  # added but not committed yet: the index has it
            rc, rec = run("git", "-C", str(ROOT), "ls-files", "-s", path)
            rec = rec.split()[1] if rc == 0 and rec else ""
        ok = head == rec
        detail = f"{head[:7]}, recorded {rec[:7]}"
    rc, dirty = run("git", "-C", str(ROOT / path), "status", "--porcelain")
    if dirty:
        ok, detail = False, detail + ", local changes"
    return ok, detail


def emulated():
    print("emulated bench")
    ok, detail = submodule("emu/emumd")
    check("emu/emumd at the recorded commit, unmodified", ok, detail)
    fork = DEV / "builds/emumd-cache/hatari-fork/build/src/hatari"
    check("emumd's Hatari fork built", fork.exists(), "tools/dev/build_hatari_fork.sh"
          if not fork.exists() else str(fork.relative_to(ROOT)))
    for tos in ("tos104uk.img", "tos162uk.img", "tos206uk.img"):
        p = ROOT / "emu/tos" / tos
        check(f"emu/tos/{tos}", p.exists(), "a link to the TOS image" if not p.exists() else "")
    check("emu/sd/ (the emulated card)", (ROOT / "emu/sd").is_dir())
    for tool in ("mcopy", "mformat", "rsync"):
        check(f"{tool} (mtools, rsync)", shutil.which(tool) is not None)
    atari_hd = Path(os.environ.get("ATARI_HD", ROOT.parent / "atari-hd"))
    check("atari-hd (ATARI_HD)", (atari_hd / "atari_hd.py").exists(), str(atari_hd))
    check("stock Hatari (/usr/local/bin/hatari)", Path("/usr/local/bin/hatari").exists())
    rc, out = run("docker", "info", "--format", "{{.ServerVersion}}")
    check("Docker running", rc == 0, out.splitlines()[-1] if out else "")
    check("stcmd (atarist-toolkit-docker)", shutil.which("stcmd") is not None)


def device():
    print("device bench")
    tc = os.environ.get("PICO_TOOLCHAIN_PATH", "")
    gcc = Path(tc) / "arm-none-eabi-gcc"
    check("PICO_TOOLCHAIN_PATH has arm-none-eabi-gcc", gcc.exists(), tc or "unset")
    if gcc.exists():
        rc, out = run(str(gcc), "-dumpversion")
        check("ARM GNU Toolchain 14.2", out.startswith("14.2"), out)
    ocd = os.environ.get("PICO_OPENOCD_PATH", "")
    check("PICO_OPENOCD_PATH (OpenOCD's tcl folder)", bool(ocd) and Path(ocd).is_dir(), ocd or "unset")
    probe = sorted(Path("/dev").glob("cu.usbmodem*"))
    check("a Debug Probe UART (/dev/cu.usbmodem*)", bool(probe), ", ".join(map(str, probe)))
    for path, tag in (("pico-sdk", "2.2.0"), ("pico-extras", "sdk-2.2.0"), ("fatfs-sdk", "v3.6.2")):
        ok, detail = submodule(path, tag)
        check(f"{path} at {tag}", ok, detail)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--emulated", action="store_true")
    ap.add_argument("--device", action="store_true")
    args = ap.parse_args()
    both = not (args.emulated or args.device)
    if both or args.emulated:
        emulated()
    if both or args.device:
        device()
    print(f"{'all ok' if not FAILED else str(len(FAILED)) + ' FAIL'}")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
