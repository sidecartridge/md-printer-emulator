#!/usr/bin/env bash
#
# hook_fidelity.sh: does what a program prints reach the RP intact? Runs a
# printing test (tools/dev/emu_tests/NAME.steps) twice on the firmware, after
# [F]irmware has installed the printer hook: switched off, the printer port
# gets the bytes (the reference, the same screen and the same cartridge as
# the second run); switched on, the hook sends them to the RP, which writes
# them, one file per job, to PRINTER/ on its card. PASS when the port's bytes
# equal the jobs' files, in order, byte for byte.
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
got="$on/card.prn"
cat "$on"/sd/PRINTER/*.PRN > "$got" 2>/dev/null
verdict() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['result'])" "$1/report.json" 2>/dev/null || echo "?"; }
echo "off: $(verdict "$off") $(stat -f %z "$ref" 2>/dev/null || echo 0) bytes on the port  ($off)"
echo "on:  $(verdict "$on") $(stat -f %z "$got" 2>/dev/null || echo 0) bytes on the card  ($on)"
if [[ -s "$ref" ]] && cmp -s "$ref" "$got"; then
  echo "hook_fidelity: PASS $TEST $SETUP: the RP got the printer port's bytes, byte for byte"
else
  echo "hook_fidelity: FAIL $TEST $SETUP: $(cmp "$ref" "$got" 2>&1 | head -1)"
  exit 1
fi
