#!/usr/bin/env bash
#
# mdfw.sh: emumd's mdfw (emu/emumd/tools/mdfw), run from the repository's
# root with this project's emumd cache (tools/dev/builds/emumd-cache/, built
# by tools/dev/build_hatari_fork.sh), so it uses the Hatari fork that emumd's
# submodule pins. Every argument is passed on.
#
# Usage:  tools/dev/mdfw.sh build
#         tools/dev/mdfw.sh run [--headless --frames N --screenshot out.png]

set -euo pipefail

DEV="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DEV/../.." && pwd)"
export EMUMD_CACHE="$DEV/builds/emumd-cache"
cd "$ROOT"
exec "$ROOT/emu/emumd/tools/mdfw" "$@"
