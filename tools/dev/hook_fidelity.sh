#!/usr/bin/env bash
#
# hook_fidelity.sh: does what a program prints reach the RP intact, and come
# out as the pages and PDFs the renderer draws for it? Runs a printing test
# (tools/dev/emu_tests/NAME.steps) twice on the firmware, after [F]irmware has
# installed the printer hook: switched off, the printer port gets the bytes
# (the reference, the same screen and the same cartridge as the second run);
# switched on, the hook sends them to the RP, which writes them, one file per
# job, to PRINTER/ on its card, and renders each job into its PDF. A job's
# file is deleted once its PDF is written: the RP's debug log gives each job's
# size and MD5 as it closes, and each page's MD5 as it is rendered. PASS when
# the port's bytes, cut at the jobs' sizes in order, give the jobs' MD5s with
# nothing left over, and each cut, rendered on this computer
# (tools/printer/render, on the paper the run used), gives the job's pages and
# its PDF on the card, byte for byte.
#
# Usage:  tools/dev/hook_fidelity.sh TEST SETUP [--paper a4|letter] [--timeout S]

set -Euo pipefail
DEV="$(cd "$(dirname "$0")" && pwd)"
TEST="$1"; SETUP="$2"; shift 2
PAPER=a4
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
  [ "${args[i]}" = --paper ] && PAPER="${args[i + 1]:-a4}"
done
run() {  # $1 on|off, then the harness's options; prints the run's folder
  local hook="$1"; shift
  python3 "$DEV/emu_harness.py" --setup "$SETUP" --test "$TEST" --via-firmware --hook "$hook" \
    --name "fidelity-$hook-$PAPER-$TEST" --no-build "$@" > /dev/null 2>&1
  ls -td "$DEV"/logs/emu/*-"$SETUP"-fork-fidelity-"$hook"-"$PAPER"-"$TEST" | head -1
}
python3 "$DEV/emu_harness.py" --setup "$SETUP" --step "wait 1" --name build --timeout 60 > /dev/null 2>&1 || true
make -s -C "$DEV/../printer"
off="$(run off "$@")"
on="$(run on "$@")"
ref="$off/printer.out"
verdict() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['result'])" "$1/report.json" 2>/dev/null || echo "?"; }
echo "off: $(verdict "$off") $(stat -f %z "$ref" 2>/dev/null || echo 0) bytes on the port  ($off)"
echo "on:  $(verdict "$on")  ($on)"
# Prints the jobs and pages compared and, when something differs, what.
if ! diff="$(python3 - "$ref" "$on" "$DEV/builds/printer/render" "$PAPER" <<'PY'
import hashlib, re, subprocess, sys, tempfile
from pathlib import Path
ref_path, on, render, paper = sys.argv[1:]
ref = Path(ref_path).read_bytes() if Path(ref_path).is_file() else b""
log = (Path(on) / "hatari.out").read_bytes()
jobs = re.findall(rb"printer: job (\d{8}) closed, (\d+) bytes, MD5 ([0-9a-f]{32})", log)
logged = {}
for number, page, size, md5 in re.findall(
        rb"pages: job (\d{8}) page (\d+) (\d+x\d+) MD5 ([0-9a-f]{32})", log):
    logged.setdefault(number.decode(), []).append(f"page {int(page)} {size.decode()} {md5.decode()}")

def differ(why):
    print(why)
    sys.exit(1)

pos = pages = 0
with tempfile.TemporaryDirectory() as tmp:
    for number, size, md5 in jobs:
        number = number.decode()
        part = ref[pos:pos + int(size)]
        if hashlib.md5(part).hexdigest() != md5.decode():
            differ(f"job {number} ({int(size)} bytes from byte {pos}) differs")
        pos += int(size)
        (Path(tmp) / "job.prn").write_bytes(part)
        (Path(tmp) / "pages").mkdir(exist_ok=True)
        options = ["--letter"] if paper == "letter" else []
        out = subprocess.run([render, *options, "--pdf", f"{tmp}/job.pdf", f"{tmp}/job.prn",
                              f"{tmp}/pages"], capture_output=True, text=True, check=True)
        want = [line for line in out.stdout.splitlines() if line.startswith("page ")]
        if logged.get(number, []) != want:
            differ(f"job {number}: the RP drew {logged.get(number, [])}, render {want}")
        card = Path(on) / "sd/PRINTER" / f"{number}.pdf"
        if want and (not card.is_file() or card.read_bytes() != (Path(tmp) / "job.pdf").read_bytes()):
            differ(f"job {number}: {card.name} is not render's PDF")
        if not want and card.exists():
            differ(f"job {number}: no page, yet {card.name} is on the card")
        pages += len(want)
if not ref or pos != len(ref):
    differ(f"the jobs hold {pos} bytes, the port {len(ref)}")
print(f"{len(jobs)} jobs, {pos} bytes, {pages} pages on {paper}")
PY
)"; then
  echo "hook_fidelity: FAIL $TEST $SETUP: $diff"
  exit 1
fi
echo "hook_fidelity: PASS $TEST $SETUP: the RP got the printer port's bytes, and drew render's pages and PDFs ($diff)"
