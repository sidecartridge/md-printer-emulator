#!/usr/bin/env bash
#
# hook_fidelity.sh: does what a program prints reach the RP intact? Runs a
# printing test (tools/dev/emu_tests/NAME.steps) twice on the firmware, after
# [F]irmware has installed the printer hook: switched off, the printer port
# gets the bytes (the reference, the same screen and the same cartridge as
# the second run); switched on, the hook sends them to the RP, which writes
# them, one file per job, to PRINTER/ on its card. A job's file is deleted
# once its PDF is written: the RP's debug log gives each job's size and MD5 as
# it closes. PASS when the port's bytes, cut at the jobs' sizes in order, give
# the jobs' MD5s, with nothing left over.
#
# Usage:  tools/dev/hook_fidelity.sh TEST SETUP [--timeout S]

set -Euo pipefail
DEV="$(cd "$(dirname "$0")" && pwd)"
TEST="$1"; SETUP="$2"; shift 2
run() {  # $1 on|off, then the harness's options; prints the run's folder
  local hook="$1"; shift
  python3 "$DEV/emu_harness.py" --setup "$SETUP" --test "$TEST" --via-firmware --hook "$hook" \
    --name "fidelity-$hook-$TEST" --no-build "$@" > /dev/null 2>&1
  ls -td "$DEV"/logs/emu/*-"$SETUP"-fork-fidelity-"$hook"-"$TEST" | head -1
}
python3 "$DEV/emu_harness.py" --setup "$SETUP" --step "wait 1" --name build --timeout 60 > /dev/null 2>&1 || true
off="$(run off "$@")"
on="$(run on "$@")"
ref="$off/printer.out"
verdict() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['result'])" "$1/report.json" 2>/dev/null || echo "?"; }
echo "off: $(verdict "$off") $(stat -f %z "$ref" 2>/dev/null || echo 0) bytes on the port  ($off)"
echo "on:  $(verdict "$on")  ($on)"
# Prints the jobs' total and, when they differ from the port's bytes, where.
if ! diff="$(python3 - "$ref" "$on/hatari.out" <<'PY'
import hashlib, re, sys
ref = open(sys.argv[1], "rb").read() if sys.argv[1] else b""
log = open(sys.argv[2], "rb").read()
jobs = re.findall(rb"printer: job (\d{8}) closed, (\d+) bytes, MD5 ([0-9a-f]{32})", log)
pos = 0
for number, size, md5 in jobs:
    part = ref[pos:pos + int(size)]
    if hashlib.md5(part).hexdigest() != md5.decode():
        print(f"job {number.decode()} ({int(size)} bytes from byte {pos}) differs")
        sys.exit(1)
    pos += int(size)
if not ref or pos != len(ref):
    print(f"the jobs hold {pos} bytes, the port {len(ref)}")
    sys.exit(1)
print(f"{len(jobs)} jobs, {pos} bytes")
PY
)"; then
  echo "hook_fidelity: FAIL $TEST $SETUP: $diff"
  exit 1
fi
echo "hook_fidelity: PASS $TEST $SETUP: the RP got the printer port's bytes, byte for byte ($diff)"
